#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gioerror.h>
#include <gio/gfile.h>

#include "gvfsbackendsmbbrowse.h"
#include "gvfsjobmountmountable.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gmounttracker.h"
#include <libsmbclient.h>

typedef struct {
  unsigned int smbc_type;
  char *name;
  char *name_normalized;
  char *name_utf8;
  char *comment;
} BrowseEntry;

struct _GVfsBackendSmbBrowse
{
  GVfsBackend parent_instance;

  char *user;
  char *domain;
  char *server;
  SMBCCTX *smb_context;

  GMutex *entries_lock;
  time_t last_entry_update;
  GList *entries;
  int entry_errno;
};

static GHashTable *server_cache = NULL;
static GMountTracker *mount_tracker = NULL;

typedef struct {
  char *server_name;
  char *share_name;
  char *domain;
  char *username;
} CachedServer;

G_DEFINE_TYPE (GVfsBackendSmbBrowse, g_vfs_backend_smb_browse, G_VFS_TYPE_BACKEND);

static char *
normalize_smb_name_helper (const char *name, gssize len, gboolean valid_utf8)
{
  if (valid_utf8)
    return g_utf8_casefold (name, len);
  else
    return g_ascii_strdown (name, len);
}

static char *
normalize_smb_name (const char *name, gssize len)
{
  gboolean valid_utf8;

  valid_utf8 = g_utf8_validate (name, len, NULL);
  return normalize_smb_name_helper (name, len, valid_utf8);
}

static char *
smb_name_to_utf8 (const char *name, gboolean *valid_utf8_out)
{
  GString *string;
  const gchar *remainder, *invalid;
  gint remaining_bytes, valid_bytes;
  gboolean valid_utf8;
      
  remainder = name;
  remaining_bytes = strlen (name);
  valid_utf8 = TRUE;
  
  string = g_string_sized_new (remaining_bytes);
  while (remaining_bytes != 0) 
    {
      if (g_utf8_validate (remainder, remaining_bytes, &invalid)) 
	break;
      valid_utf8 = FALSE;
      
      valid_bytes = invalid - remainder;
      
      g_string_append_len (string, remainder, valid_bytes);
      /* append U+FFFD REPLACEMENT CHARACTER */
      g_string_append (string, "\357\277\275");
      
      remaining_bytes -= valid_bytes + 1;
      remainder = invalid + 1;
    }
  
  g_string_append (string, remainder);
  
  if (valid_utf8_out)
    *valid_utf8_out = valid_utf8;
  
  return g_string_free (string, FALSE);
}

static void
browse_entry_free (BrowseEntry *entry)
{
  g_free (entry->name);
  g_free (entry->comment);
  g_free (entry);
}

static gboolean
cached_server_equal (gconstpointer  _a,
		     gconstpointer  _b)
{
  const CachedServer *a = _a;
  const CachedServer *b = _b;

  return
    strcmp (a->server_name, b->server_name) == 0 &&
    strcmp (a->share_name, b->share_name) == 0 &&
    strcmp (a->domain, b->domain) == 0 &&
    strcmp (a->username, b->username) == 0;
}

static guint
cached_server_hash (gconstpointer key)
{
  const CachedServer *server = key;

  return
    g_str_hash (server->server_name) ^
    g_str_hash (server->share_name) ^
    g_str_hash (server->domain) ^
    g_str_hash (server->username);
}

static void
cached_server_free (CachedServer *server)
{
  g_free (server->server_name);
  g_free (server->share_name);
  g_free (server->domain);
  g_free (server->username);
  g_free (server);
}

static void
g_vfs_backend_smb_browse_finalize (GObject *object)
{
  GVfsBackendSmbBrowse *backend;

  backend = G_VFS_BACKEND_SMB_BROWSE (object);

  g_free (backend->user);
  g_free (backend->domain);
  g_free (backend->server);
  
  g_mutex_free (backend->entries_lock);

  smbc_free_context (backend->smb_context, TRUE);
  
  g_list_foreach (backend->entries, (GFunc)browse_entry_free, NULL);
  g_list_free (backend->entries);
  
  if (G_OBJECT_CLASS (g_vfs_backend_smb_browse_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_smb_browse_parent_class)->finalize) (object);
}

static void
g_vfs_backend_smb_browse_init (GVfsBackendSmbBrowse *backend)
{
  backend->entries_lock = g_mutex_new ();

  if (mount_tracker == NULL)
    mount_tracker = g_mount_tracker_new (NULL);
}

/**
 * Authentication callback function type (method that includes context)
 * 
 * Type for the the authentication function called by the library to
 * obtain authentication credentals
 *
 * @param context   Pointer to the smb context
 * @param srv       Server being authenticated to
 * @param shr       Share being authenticated to
 * @param wg        Pointer to buffer containing a "hint" for the
 *                  workgroup to be authenticated.  Should be filled in
 *                  with the correct workgroup if the hint is wrong.
 * @param wglen     The size of the workgroup buffer in bytes
 * @param un        Pointer to buffer containing a "hint" for the
 *                  user name to be use for authentication. Should be
 *                  filled in with the correct workgroup if the hint is
 *                  wrong.
 * @param unlen     The size of the username buffer in bytes
 * @param pw        Pointer to buffer containing to which password 
 *                  copied
 * @param pwlen     The size of the password buffer in bytes
 *           
 */
static void
auth_callback (SMBCCTX *context,
	       const char *server_name, const char *share_name,
	       char *domain_out, int domainmaxlen,
	       char *username_out, int unmaxlen,
	       char *password_out, int pwmaxlen)
{
  GVfsBackendSmbBrowse *backend;

  backend = smbc_option_get (context, "user_data");

  if (backend->domain)
    strncpy (domain_out, backend->domain, domainmaxlen);
  if (backend->user)
    strncpy (username_out, backend->user, unmaxlen);
  strncpy (password_out, "", pwmaxlen);
}

/* Add a server to the cache system
 *
 * @param c         pointer to smb context
 * @param srv       pointer to server to add
 * @param server    server name 
 * @param share     share name
 * @param workgroup workgroup used to connect
 * @param username  username used to connect
 * @return          0 on success. 1 on failure.
 *
 */ 
static int
add_cached_server (SMBCCTX *context, SMBCSRV *new,
		   const char *server_name, const char *share_name, 
		   const char *domain, const char *username)
{
  CachedServer *cached_server;

  cached_server = g_new (CachedServer, 1);
  cached_server->server_name = g_strdup (server_name);
  cached_server->share_name = g_strdup (share_name);
  cached_server->domain = g_strdup (domain);
  cached_server->username = g_strdup (username);

  if (server_cache == NULL)
    server_cache = g_hash_table_new_full (cached_server_hash, cached_server_equal,
					  (GDestroyNotify)cached_server_free, NULL);

  g_hash_table_insert (server_cache, cached_server, new);

  return 0;
}

static gboolean
remove_cb (gpointer  key,
	   gpointer  value,
	   gpointer  user_data)
{
  return value == user_data;
}

/* Remove cached server
 *
 * @param c         pointer to smb context
 * @param srv       pointer to server to remove
 * @return          0 when found and removed. 1 on failure.
 *
 */ 
static int
remove_cached_server (SMBCCTX * context, SMBCSRV * server)
{
  guint num;

  if (server_cache)
    {
      num = g_hash_table_foreach_remove (server_cache, remove_cb, server);
      if (num != 0)
	return 0;
    }

  return 1;
}

/* Look up a server in the cache system
 *
 * @param c         pointer to smb context
 * @param server    server name to match
 * @param share     share name to match
 * @param workgroup workgroup to match
 * @param username  username to match
 * @return          pointer to SMBCSRV on success. NULL on failure.
 *
 */ 
static SMBCSRV *
get_cached_server (SMBCCTX * context,
		   const char *server_name, const char *share_name,
		   const char *domain, const char *username)
{
  const CachedServer key = {
    (char *)server_name,
    (char *)share_name,
    (char *)domain,
    (char *)username
  };

  if (server_cache)
    return g_hash_table_lookup (server_cache, &key);
  else
    return NULL;
}

/* Try to remove all servers from the cache system and disconnect
 *
 * @param c         pointer to smb context
 *
 * @return          0 when found and removed. 1 on failure.
 *
 */ 
static int
purge_cached (SMBCCTX * context)
{
  if (server_cache)
    g_hash_table_remove_all (server_cache);
  
  return 0;
}

#define SUB_DELIM_CHARS  "!$&'()*+,;="

static gboolean
is_valid (char c, const char *reserved_chars_allowed)
{
  if (g_ascii_isalnum (c) ||
      c == '-' ||
      c == '.' ||
      c == '_' ||
      c == '~')
    return TRUE;

  if (reserved_chars_allowed &&
      strchr (reserved_chars_allowed, c) != NULL)
    return TRUE;
  
  return FALSE;
}

static void
g_string_append_encoded (GString *string,
			 const char *encoded,
			 const char *encoded_end,
			 const char *reserved_chars_allowed)
{
  char c;
  static const gchar hex[16] = "0123456789ABCDEF";

  if (encoded_end == NULL)
    encoded_end = encoded + strlen (encoded);
  
  while (encoded < encoded_end)
    {
      c = *encoded++;
      
      if (is_valid (c, reserved_chars_allowed))
	g_string_append_c (string, c);
      else
	{
	  g_string_append_c (string, '%');
	  g_string_append_c (string, hex[((guchar)c) >> 4]);
	  g_string_append_c (string, hex[((guchar)c) & 0xf]);
	}
    }
}

static void
update_cache (GVfsBackendSmbBrowse *backend)
{
  GString *uri;
  char dirents[1024*4];
  struct smbc_dirent *dirp;
  GList *entries;
  int entry_errno;
  SMBCFILE *dir;
  int res;

  entries = NULL;
  entry_errno = 0;
  
  /* Update Cache */
  uri = g_string_new ("smb://");

  if (backend->server)
    {
      g_string_append_encoded (uri, backend->server, NULL, NULL);
      g_string_append_c (uri, '/');
    }
  
  g_print ("update_cache: %s\n", uri->str);

  dir = backend->smb_context->opendir (backend->smb_context, uri->str);
  g_string_free (uri, TRUE);
  if (dir == NULL)
    {
      entry_errno = errno;
      goto out;
    }

  while (TRUE)
    {
      res = backend->smb_context->getdents (backend->smb_context, dir, (struct smbc_dirent *)dirents, sizeof (dirents));
      if (res <= 0)
	break;
      
      dirp = (struct smbc_dirent *)dirents;
      while (res > 0)
	{
	  unsigned int dirlen;

	  g_print ("type: %d, name: %s\n", dirp->smbc_type, dirp->name);
	  
	  if (dirp->smbc_type != SMBC_IPC_SHARE &&
	      dirp->smbc_type != SMBC_COMMS_SHARE &&
	      dirp->smbc_type != SMBC_PRINTER_SHARE &&
	      strcmp (dirp->name, ".") != 0 &&
	      strcmp (dirp->name, "..") != 0)
	    {
	      BrowseEntry *entry = g_new (BrowseEntry, 1);
	      gboolean valid_utf8;

	      g_print ("added %s to cache\n", dirp->name);
	      
	      entry->smbc_type = dirp->smbc_type;
	      entry->name = g_strdup (dirp->name);
	      entry->name_utf8 = smb_name_to_utf8 (dirp->name, &valid_utf8);
	      entry->name_normalized = normalize_smb_name_helper (dirp->name, -1, valid_utf8);
	      entry->comment = smb_name_to_utf8 (dirp->comment, NULL);
	      
	      entries = g_list_prepend (entries, entry);
	    }
		  
	  dirlen = dirp->dirlen;
	  dirp = (struct smbc_dirent *) (((char *)dirp) + dirlen);
	  res -= dirlen;
	}

      entries = g_list_reverse (entries);
    }
      
  backend->smb_context->closedir (backend->smb_context, dir);


 out:

  g_mutex_lock (backend->entries_lock);
  
  /* Clear old cache */
  g_list_foreach (backend->entries, (GFunc)browse_entry_free, NULL);
  g_list_free (backend->entries);
  backend->entries = entries;
  backend->entry_errno = entry_errno;
  backend->last_entry_update = time (NULL);

  g_mutex_unlock (backend->entries_lock);
  
}

static BrowseEntry *
find_entry_unlocked (GVfsBackendSmbBrowse *backend,
		     const char *filename)
{
  BrowseEntry *entry, *found;
  GList *l;
  char *end;
  int len;
  char *normalized;

  while (*filename == '/')
    filename++;

  end = strchr (filename, '/');
  if (end)
    {
      len = end - filename;

      while (*end == '/')
	end++;

      if (*end != 0)
	return NULL;
    }
  else
    len = strlen (filename);

  /* First look for an exact filename match */
  found = NULL;
  for (l = backend->entries; l != NULL; l = l->next)
    {
      entry = l->data;
      
      if (strncmp (filename, entry->name, len) == 0 &&
	  strlen (entry->name) == len)
	{
	  found = entry;
	  break;
	}
    }

  if (found == NULL)
    {
      /* That failed, try normalizing the filename */
      normalized = normalize_smb_name (filename, len);
      
      for (l = backend->entries; l != NULL; l = l->next)
	{
	  entry = l->data;
	  
	  if (strcmp (normalized, entry->name_normalized) == 0)
	    {
	      found = entry;
	      break;
	    }
	}
      g_free (normalized);
    }
  
  return found;
}

static GMountSpec *
get_mount_spec_for_share (const char *server,
			  const char *share)
{
  GMountSpec *mount_spec;
  char *normalized;
  
  mount_spec = g_mount_spec_new ("smb-share");
  normalized = normalize_smb_name (server, -1);
  g_mount_spec_set (mount_spec, "server", normalized);
  g_free (normalized);
  normalized = normalize_smb_name (share, -1);
  g_mount_spec_set (mount_spec, "share", normalized);
  g_free (normalized);
  
  return mount_spec;
}

static gboolean
is_root (const char *filename)
{
  const char *p;

  p = filename;
  while (*p == '/')
    p++;

  return *p == 0;
}

static gboolean
has_name (GVfsBackendSmbBrowse *backend,
	  const char *filename)
{
  gboolean res;
  
  g_mutex_lock (backend->entries_lock);
  res = (find_entry_unlocked (backend, filename) != NULL);
  g_mutex_unlock (backend->entries_lock);
  return res;
}

static gboolean
cache_needs_updating (GVfsBackendSmbBrowse *backend)
{
  time_t now = time (NULL);
  
  return now < backend->last_entry_update ||
    (now - backend->last_entry_update) > 10;
}

static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);
  SMBCCTX *smb_context;
  char *display_name;
  GMountSpec *browse_mount_spec;

  g_print ("do_mount\n");
  
  smb_context = smbc_new_context ();
  if (smb_context == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			"Failed to allocate smb context");
      return;
    }

  smbc_option_set (smb_context, "user_data", backend);
  
  smb_context->debug = 0;
  smb_context->callbacks.auth_fn = NULL;
  smbc_option_set (smb_context, "auth_function",
		   (void *) auth_callback);
  
  smb_context->callbacks.add_cached_srv_fn    = add_cached_server;
  smb_context->callbacks.get_cached_srv_fn    = get_cached_server;
  smb_context->callbacks.remove_cached_srv_fn = remove_cached_server;
  smb_context->callbacks.purge_cached_fn      = purge_cached;
  
  smb_context->flags = 0;
  
#if defined(HAVE_SAMBA_FLAGS) 
#if defined(SMB_CTX_FLAG_USE_KERBEROS) && defined(SMB_CTX_FLAG_FALLBACK_AFTER_KERBEROS)
  smb_context->flags |= SMB_CTX_FLAG_USE_KERBEROS | SMB_CTX_FLAG_FALLBACK_AFTER_KERBEROS;
#endif
#if defined(SMBCCTX_FLAG_NO_AUTO_ANONYMOUS_LOGON)
  //smb_context->flags |= SMBCCTX_FLAG_NO_AUTO_ANONYMOUS_LOGON;
#endif
#endif

#if 0
  smbc_option_set (smb_context, "debug_stderr", (void *) 1);
#endif
  
  if (!smbc_init_context (smb_context))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			"Failed to initialize smb context");
      smbc_free_context (smb_context, FALSE);
      return;
    }

  op_backend->smb_context = smb_context;
  
  if (op_backend->server == NULL)
    {
      display_name = g_strdup ("smb network");
      browse_mount_spec = g_mount_spec_new ("smb-network");
    }
  else
    {
      display_name = g_strdup_printf ("smb share %s", op_backend->server);
      browse_mount_spec = g_mount_spec_new ("smb-server");
      g_mount_spec_set (browse_mount_spec, "server", op_backend->server);
    }

  if (op_backend->user)
    g_mount_spec_set (browse_mount_spec, "user", op_backend->user);
  if (op_backend->domain)
    g_mount_spec_set (browse_mount_spec, "domain", op_backend->domain);
  
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  g_vfs_backend_set_mount_spec (backend, browse_mount_spec);
  g_mount_spec_unref (browse_mount_spec);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_print ("finished mount\n");
}

static gboolean
try_mount (GVfsBackend *backend,
	   GVfsJobMount *job,
	   GMountSpec *mount_spec,
	   GMountSource *mount_source,
	   gboolean is_automount)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);
  const char *server;
  const char *user, *domain;

  g_print ("try_mount\n");
  
  if (strcmp (g_mount_spec_get_type (mount_spec), "smb-network") == 0)
    server = NULL;
  else
    {
      server = g_mount_spec_get (mount_spec, "server");
      if (server == NULL)
	{
	  g_vfs_job_failed (G_VFS_JOB (job),
			    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			    "No server specified for smb-server share");
	  return TRUE;
	}
    }

  user = g_mount_spec_get (mount_spec, "user");
  domain = g_mount_spec_get (mount_spec, "domain");
  
  if (is_automount &&
      ((user != NULL) || (domain != NULL)))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Can't automount smb browsing with specified user or domain");
      return TRUE;
    }
  
  op_backend->user = g_strdup (user);
  op_backend->domain = g_strdup (domain);
  op_backend->server = g_strdup (server);
  
  return FALSE;
}

static void
run_mount_mountable (GVfsBackendSmbBrowse *backend,
		     GVfsJobMountMountable *job,
		     const char *filename,
		     GMountSource *mount_source)
{
  GFileInfo *info;
  BrowseEntry *entry;
  GError *error = NULL;
  GMountSpec *mount_spec;

  info = NULL;
  g_mutex_lock (backend->entries_lock);
  
  entry = find_entry_unlocked (backend, filename);

  if (entry)
    {
      if (backend->server != NULL &&
	  entry->smbc_type == SMBC_FILE_SHARE)
	{
	  mount_spec = get_mount_spec_for_share (backend->server, entry->name);
	  g_vfs_job_mount_mountable_set_target (job, mount_spec, "/", TRUE);
	  g_mount_spec_unref (mount_spec);
	}
      else
	g_set_error (&error,
		     G_IO_ERROR, G_IO_ERROR_NOT_MOUNTABLE_FILE,
		     _("The file is not a mountable"));
    }
  else
    g_set_error (&error,
		 G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		 _("File doesn't exist"));
      
  g_mutex_unlock (backend->entries_lock);

  if (error)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_mount_mountable (GVfsBackend *backend,
		    GVfsJobMountMountable *job,
		    const char *filename,
		    GMountSource *mount_source)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  update_cache (op_backend);

  run_mount_mountable (op_backend,
		       job,
		       filename,
		       mount_source);
}

static gboolean
try_mount_mountable (GVfsBackend *backend,
		     GVfsJobMountMountable *job,
		     const char *filename,
		     GMountSource *mount_source)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  if (is_root (filename))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_MOUNTABLE_FILE,
			_("The file is not a mountable"));
      return TRUE;
    }
  
  if (cache_needs_updating (op_backend))
    return FALSE;

  run_mount_mountable (op_backend,
		       job,
		       filename,
		       mount_source);
  return TRUE;
}

static void 
run_open_for_read (GVfsBackendSmbBrowse *backend,
		    GVfsJobOpenForRead *job,
		    const char *filename)
{
  if (has_name (backend, filename))
    g_vfs_job_failed (G_VFS_JOB (job),
		      G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
		      _("Not a regular file"));
  else
    g_vfs_job_failed (G_VFS_JOB (job),
		      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		      _("File doesn't exist"));
}

static void
do_open_for_read (GVfsBackend *backend,
	       GVfsJobOpenForRead *job,
	       const char *filename)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  update_cache (op_backend);

  run_open_for_read (op_backend, job, filename);
}

static gboolean
try_open_for_read (GVfsBackend *backend,
		   GVfsJobOpenForRead *job,
		   const char *filename)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  if (cache_needs_updating (op_backend))
    return FALSE;
 
  run_open_for_read (op_backend, job, filename);

  return TRUE;
}

static gboolean
try_read (GVfsBackend *backend,
	  GVfsJobRead *job,
	  GVfsBackendHandle handle,
	  char *buffer,
	  gsize bytes_requested)
{
  g_vfs_job_failed (G_VFS_JOB (job),
		    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    "Invalid argument");
  
  return TRUE;
}

static gboolean
try_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  g_vfs_job_failed (G_VFS_JOB (job),
		    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    "Invalid argument");
  return TRUE;
}

static gboolean
try_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  g_vfs_job_failed (G_VFS_JOB (job),
		    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    "Invalid argument");
  return TRUE;
}

static void
get_file_info_from_entry (GVfsBackendSmbBrowse *backend, BrowseEntry *entry, GFileInfo *info)
{
  GMountSpec *mount_spec;
  GString *uri;
  
  g_file_info_set_name (info, entry->name);
  g_file_info_set_display_name (info, entry->name_utf8);
  g_file_info_set_edit_name (info, entry->name_utf8);
  g_file_info_set_attribute_string (info, "smb:comment", entry->comment);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STD_IS_VIRTUAL, TRUE);

  mount_spec = NULL;
  if (backend->server)
    {
      /* browsing server/workgroup */
      if (entry->smbc_type == SMBC_WORKGROUP ||
	  entry->smbc_type == SMBC_SERVER)
	{
	  uri = g_string_new ("smb://");
	  g_string_append_encoded (uri, entry->name, NULL, NULL);
	  g_string_append_c (uri, '/');
	}
      else
	{
	  mount_spec = get_mount_spec_for_share (backend->server, entry->name);

	  uri = g_string_new ("smb://");
	  g_string_append_encoded (uri, backend->server, NULL, NULL);
	  g_string_append_c (uri, '/');
	  g_string_append_encoded (uri, entry->name, NULL, NULL);
	}
    }
  else
    {
      /* browsing network */
      uri = g_string_new ("smb://");
      g_string_append_encoded (uri, entry->name, NULL, NULL);
      g_string_append_c (uri, '/');

      /* these are auto-mounted, so no CAN_MOUNT/UNMOUNT */
    }

  if (mount_spec)
    {
      g_file_info_set_file_type (info, G_FILE_TYPE_MOUNTABLE);
      if (g_mount_tracker_has_mount_spec (mount_tracker, mount_spec))
	{
	  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, FALSE);
	  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, TRUE);
	}
      else
	{
	  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, TRUE);
	  g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, FALSE);
	}
      g_mount_spec_unref (mount_spec);
    }
  else
    g_file_info_set_file_type (info, G_FILE_TYPE_SHORTCUT);
    
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STD_TARGET_URI, uri->str);
  g_string_free (uri, TRUE);
}

static void
run_query_info (GVfsBackendSmbBrowse *backend,
		GVfsJobQueryInfo *job,
		const char *filename,
		GFileInfo *info,
		GFileAttributeMatcher *matcher)
{
  BrowseEntry *entry;

  g_mutex_lock (backend->entries_lock);
  
  entry = find_entry_unlocked (backend, filename);

  if (entry)
    get_file_info_from_entry (backend, entry, info);
      
  g_mutex_unlock (backend->entries_lock);

  if (entry)
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    g_vfs_job_failed (G_VFS_JOB (job),
		      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		      _("File doesn't exist"));
}

static void
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *matcher)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  update_cache (op_backend);

  run_query_info (op_backend, job, filename, info, matcher);
}


static gboolean
try_query_info (GVfsBackend *backend,
		GVfsJobQueryInfo *job,
		const char *filename,
		GFileQueryInfoFlags flags,
		GFileInfo *info,
		GFileAttributeMatcher *matcher)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  if (is_root (filename))
    {
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_name (info, "/");
      g_vfs_job_succeeded (G_VFS_JOB (job));
      
      return TRUE;
    }
  
  if (cache_needs_updating (op_backend))
    return FALSE;

  run_query_info (op_backend, job, filename, info, matcher);
  
  return TRUE;
}

static void
run_enumerate (GVfsBackendSmbBrowse *backend,
	       GVfsJobEnumerate *job,
	       const char *filename,
	       GFileAttributeMatcher *matcher)
{
  GList *files, *l;
  GFileInfo *info;

  if (!is_root (filename))
    {
      if (has_name (backend, filename))
	g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
			  _("Not a directory"));
      else
	g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			  _("File doesn't exist"));
      return;
    }
  
  /* TODO: limit requested to what we support */
  g_vfs_job_succeeded (G_VFS_JOB (job));

  files = NULL;
  g_mutex_lock (backend->entries_lock);
  for (l = backend->entries; l != NULL; l = l->next)
    {
      BrowseEntry *entry = l->data;

      info = g_file_info_new ();
      get_file_info_from_entry (backend, entry, info);

      files = g_list_prepend (files, info);
    }
  g_mutex_unlock (backend->entries_lock);
  
  files = g_list_reverse (files);

  g_vfs_job_enumerate_add_infos (job, files);
  g_list_foreach (files, (GFunc)g_object_unref, NULL);
  g_list_free (files);

  g_vfs_job_enumerate_done (job);
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      GFileAttributeMatcher *matcher,
	      GFileQueryInfoFlags flags)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);
  
  update_cache (op_backend);

  run_enumerate (op_backend, job, filename, matcher);
}

static gboolean
try_enumerate (GVfsBackend *backend,
	       GVfsJobEnumerate *job,
	       const char *filename,
	       GFileAttributeMatcher *matcher,
	       GFileQueryInfoFlags flags)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  if (cache_needs_updating (op_backend))
    return FALSE;
  
  run_enumerate (op_backend, job, filename, matcher);

  return TRUE;
}

static void
g_vfs_backend_smb_browse_class_init (GVfsBackendSmbBrowseClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_smb_browse_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->mount_mountable = do_mount_mountable;
  backend_class->try_mount_mountable = try_mount_mountable;
  backend_class->open_for_read = do_open_for_read;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_read = try_read;
  backend_class->try_seek_on_read = try_seek_on_read;
  backend_class->try_close_read = try_close_read;
  backend_class->query_info = do_query_info;
  backend_class->try_query_info = try_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->try_enumerate = try_enumerate;
}
