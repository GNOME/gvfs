/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsbackendsmbbrowse.h"
#include "gvfsjobmountmountable.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gvfskeyring.h"
#include "gmounttracker.h"

#include <libsmbclient.h>
#include "libsmb-compat.h"

#ifdef HAVE_GCONF
#include <gconf/gconf-client.h>
#endif

/* We load a default workgroup from gconf */
#define PATH_GCONF_GNOME_VFS_SMB_WORKGROUP "/system/smb/workgroup"

/* The magic "default workgroup" hostname */
#define DEFAULT_WORKGROUP_NAME "X-GNOME-DEFAULT-WORKGROUP"

/* Time in seconds before we mark dirents cache outdated */
#define DEFAULT_CACHE_EXPIRATION_TIME 10


#define PRINT_DEBUG 

#ifdef PRINT_DEBUG 
#define DEBUG(msg...) g_print("### SMB-BROWSE: " msg)
#else 
#define DEBUG(...) 
#endif 

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
  char *mounted_server; /* server or DEFAULT_WORKGROUP_NAME */
  SMBCCTX *smb_context;

  char *last_user;
  char *last_domain;
  char *last_password;

  GMountSource *mount_source;
  int mount_try;
  gboolean mount_try_again;
  gboolean mount_cancelled;
  
  gboolean password_in_keyring;
  GPasswordSave password_save;

  GMutex *entries_lock;
  GMutex *update_cache_lock;
  time_t last_entry_update;
  GList *entries;
  int entry_errno;
};

static char *default_workgroup = NULL;

static GHashTable *server_cache = NULL;
static GMountTracker *mount_tracker = NULL;

typedef struct {
  char *server_name;
  char *share_name;
  char *domain;
  char *username;
} CachedServer;

G_DEFINE_TYPE (GVfsBackendSmbBrowse, g_vfs_backend_smb_browse, G_VFS_TYPE_BACKEND)

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
  g_free (backend->mounted_server);
  g_free (backend->server);
  
  g_mutex_free (backend->entries_lock);
  g_mutex_free (backend->update_cache_lock);

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
  backend->update_cache_lock = g_mutex_new ();

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
  char *ask_password, *ask_user, *ask_domain;
  gboolean handled, abort;

  backend = smbc_getOptionUserData (context);

  strncpy (password_out, "", pwmaxlen);

  if (backend->domain)
    strncpy (domain_out, backend->domain, domainmaxlen);
  if (backend->user)
    strncpy (username_out, backend->user, unmaxlen);

  if (backend->mount_cancelled)
    {
      /*  Don't prompt for credentials, let smbclient finish the mount loop  */
      strncpy (username_out, "ABORT", unmaxlen);
      strncpy (password_out, "", pwmaxlen);
      DEBUG ("auth_callback - mount_cancelled\n");
      return;
    }

  if (backend->mount_source == NULL)
    {
      /* Not during mount, use last password */
      if (backend->last_user)
        strncpy (username_out, backend->last_user, unmaxlen);
      if (backend->last_domain)
        strncpy (domain_out, backend->last_domain, domainmaxlen);
      if (backend->last_password)
        strncpy (password_out, backend->last_password, pwmaxlen);
      return;
    }

  if (backend->mount_try == 0 &&
      backend->user == NULL &&
      backend->domain == NULL)
    {
      /* Try again if kerberos login + anonymous fallback fails */
      backend->mount_try_again = TRUE;
      DEBUG ("auth_callback - anonymous pass\n");
    }
  else
    {
      gboolean in_keyring = FALSE;

      DEBUG ("auth_callback - normal pass\n");

      if (!backend->password_in_keyring)
        {
	  in_keyring = g_vfs_keyring_lookup_password (backend->user,
						      backend->server,
						      backend->domain,
						      "smb",
						      NULL,
						      NULL,
						      0,
						      &ask_user,
						      &ask_domain,
						      &ask_password);
	  backend->password_in_keyring = in_keyring;
	}

      if (!in_keyring)
        {
	  GAskPasswordFlags flags = G_ASK_PASSWORD_NEED_PASSWORD;
	  char *message;

	  if (g_vfs_keyring_is_available ())
	    flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;
	  if (backend->domain == NULL)
	    flags |= G_ASK_PASSWORD_NEED_DOMAIN;
	  if (backend->user == NULL)
	    flags |= G_ASK_PASSWORD_NEED_USERNAME;

	  DEBUG ("auth_callback - asking for password...\n");

	  /* translators: %s is a server name */
	  message = g_strdup_printf (_("Password required for %s"),
				     server_name);
	  handled = g_mount_source_ask_password (backend->mount_source,
						 message,
						 username_out,
						 domain_out,
						 flags,
						 &abort,
						 &ask_password,
						 &ask_user,
						 &ask_domain,
						 NULL,
						 &(backend->password_save));
	  g_free (message);
	  if (!handled)
	    goto out;

	  if (abort)
	    {
	      strncpy (username_out, "ABORT", unmaxlen);
	      strncpy (password_out, "", pwmaxlen);
	      backend->mount_cancelled = TRUE;
	      goto out;
	    }
	}

      /* Try again if this fails */
      backend->mount_try_again = TRUE;

      strncpy (password_out, ask_password, pwmaxlen);
      if (ask_user && *ask_user)
	strncpy (username_out, ask_user, unmaxlen);
      if (ask_domain && *ask_domain)
	strncpy (domain_out, ask_domain, domainmaxlen);

    out:
      g_free (ask_password);
      g_free (ask_user);
      g_free (ask_domain);
    }

  backend->last_user = g_strdup (username_out);
  backend->last_domain = g_strdup (domain_out);
  backend->last_password = g_strdup (password_out);
  DEBUG ("auth_callback - out: last_user = '%s', last_domain = '%s'\n", 
         backend->last_user, backend->last_domain);
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

static gboolean
update_cache (GVfsBackendSmbBrowse *backend, SMBCFILE *supplied_dir)
{
  GString *uri;
  char dirents[1024*4];
  struct smbc_dirent *dirp;
  GList *entries;
  int entry_errno;
  SMBCFILE *dir;
  int res;
  smbc_opendir_fn smbc_opendir;
  smbc_getdents_fn smbc_getdents;
  smbc_closedir_fn smbc_closedir;


  entries = NULL;
  entry_errno = 0;
  res = -1;

  g_mutex_lock (backend->update_cache_lock);
  
  DEBUG ("update_cache - updating...\n");
  
  /* Update Cache */
  uri = g_string_new ("smb://");

  if (backend->server)
    {
      g_string_append_encoded (uri, backend->server, NULL, NULL);
      g_string_append_c (uri, '/');
    }

  smbc_opendir = smbc_getFunctionOpendir (backend->smb_context);
  smbc_getdents = smbc_getFunctionGetdents (backend->smb_context);
  smbc_closedir = smbc_getFunctionClosedir (backend->smb_context);

  dir = supplied_dir ? supplied_dir : smbc_opendir (backend->smb_context, uri->str);
  g_string_free (uri, TRUE);
  if (dir == NULL)
    {
      entry_errno = errno;
      goto out;
    }

  while (TRUE)
    {
      res = smbc_getdents (backend->smb_context, dir, (struct smbc_dirent *)dirents, sizeof (dirents));
      if (res <= 0)
        {
          if (res < 0)
            DEBUG ("update_cache - smbc_getdents returned %d, errno = [%d] %s\n", 
                   res, errno, g_strerror (errno));
	  break;
	}  
      
      dirp = (struct smbc_dirent *)dirents;
      while (res > 0)
	{
	  unsigned int dirlen;

	  if (dirp->smbc_type != SMBC_IPC_SHARE &&
	      dirp->smbc_type != SMBC_COMMS_SHARE &&
	      dirp->smbc_type != SMBC_PRINTER_SHARE &&
	      strcmp (dirp->name, ".") != 0 &&
	      strcmp (dirp->name, "..") != 0)
	    {
	      BrowseEntry *entry = g_new (BrowseEntry, 1);
	      gboolean valid_utf8;

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

  if (! supplied_dir)
    smbc_closedir (backend->smb_context, dir);


 out:

  g_mutex_lock (backend->entries_lock);
  
  /* Clear old cache */
  g_list_foreach (backend->entries, (GFunc)browse_entry_free, NULL);
  g_list_free (backend->entries);
  backend->entries = entries;
  backend->entry_errno = entry_errno;
  backend->last_entry_update = time (NULL);

  DEBUG ("update_cache - done.\n");

  g_mutex_unlock (backend->entries_lock);
  g_mutex_unlock (backend->update_cache_lock);

  return (res >= 0);
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
  time_t now;
  gboolean res;

  /*  If there's already cache update in progress, lock and wait until update is finished, then recheck  */
  g_mutex_lock (backend->update_cache_lock);
  now = time (NULL);
  res = now < backend->last_entry_update ||
    (now - backend->last_entry_update) > DEFAULT_CACHE_EXPIRATION_TIME;
  g_mutex_unlock (backend->update_cache_lock);
  
  return res; 
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
  SMBCFILE *dir;
  char *display_name;
  const char *debug;
  int debug_val;
  char *icon;
  GString *uri;
  gboolean res;
  GMountSpec *browse_mount_spec;
  smbc_opendir_fn smbc_opendir;
  smbc_closedir_fn smbc_closedir;
  
  smb_context = smbc_new_context ();
  if (smb_context == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			"Failed to allocate smb context");
      return;
    }

  smbc_setOptionUserData (smb_context, backend);

  debug = g_getenv ("GVFS_SMB_DEBUG");
  if (debug)
    debug_val = atoi (debug);
  else
#ifdef PRINT_DEBUG
    debug_val = 4;
#else
    debug_val = 0;
#endif

  smbc_setDebug (smb_context, debug_val);
  smbc_setFunctionAuthDataWithContext (smb_context, auth_callback);
  
  smbc_setFunctionAddCachedServer (smb_context, add_cached_server);
  smbc_setFunctionGetCachedServer (smb_context, get_cached_server);
  smbc_setFunctionRemoveCachedServer (smb_context, remove_cached_server);
  smbc_setFunctionPurgeCachedServers (smb_context, purge_cached);
  
  /* FIXME: is strdup() still needed here? -- removed */
  if (default_workgroup != NULL)
    smbc_setWorkgroup (smb_context, default_workgroup);

#ifndef DEPRECATED_SMBC_INTERFACE
  smb_context->flags = 0;
#endif

  /* Initial settings: 
   *   - use Kerberos (always) 
   *   - in case of no username specified, try anonymous login 
   */
  smbc_setOptionUseKerberos (smb_context, 1);
  smbc_setOptionFallbackAfterKerberos (smb_context, op_backend->user != NULL);
  smbc_setOptionNoAutoAnonymousLogin (smb_context, op_backend->user != NULL);

#if 0
  smbc_setOptionDebugToStderr (smb_context, 1);
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

  /* Convert DEFAULT_WORKGROUP_NAME to real domain */
  if (op_backend->mounted_server != NULL &&
      g_ascii_strcasecmp (op_backend->mounted_server, DEFAULT_WORKGROUP_NAME) == 0)
    op_backend->server = g_strdup (smbc_getWorkgroup (smb_context));
  else
    op_backend->server = g_strdup (op_backend->mounted_server);

  icon = NULL;
  if (op_backend->server == NULL)
    {
      display_name = g_strdup (_("Windows Network"));
      browse_mount_spec = g_mount_spec_new ("smb-network");
      icon = "network-workgroup";
    }
  else
    {
      /* translators: Name for the location that lists the smb shares
	 availible on a server (%s is the name of the server) */
      display_name = g_strdup_printf (_("Windows shares on %s"), op_backend->server);
      browse_mount_spec = g_mount_spec_new ("smb-server");
      g_mount_spec_set (browse_mount_spec, "server", op_backend->mounted_server);
      icon = "network-server";
    }

  if (op_backend->user)
    g_mount_spec_set (browse_mount_spec, "user", op_backend->user);
  if (op_backend->domain)
    g_mount_spec_set (browse_mount_spec, "domain", op_backend->domain);
  
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  if (icon)
    g_vfs_backend_set_icon_name (backend, icon);
  g_vfs_backend_set_user_visible (backend, FALSE);  
  g_vfs_backend_set_mount_spec (backend, browse_mount_spec);
  g_mount_spec_unref (browse_mount_spec);

  op_backend->mount_source = mount_source;
  op_backend->mount_try = 0;
  op_backend->password_save = G_PASSWORD_SAVE_NEVER;

  smbc_opendir = smbc_getFunctionOpendir (smb_context);
  smbc_closedir = smbc_getFunctionClosedir (smb_context);

  uri = g_string_new ("smb://");

  if (op_backend->server)
    {
      g_string_append_encoded (uri, op_backend->server, NULL, NULL);
      g_string_append_c (uri, '/');
    }

  DEBUG ("do_mount - URI = %s\n", uri->str);

  do
    {
      op_backend->mount_try_again = FALSE;
      op_backend->mount_cancelled = FALSE;

      DEBUG ("do_mount - try #%d \n", op_backend->mount_try);

      dir = smbc_opendir (smb_context, uri->str);

      DEBUG ("do_mount - [%s; %d] dir = %p, cancelled = %d, errno = [%d] '%s' \n", 
             uri->str, op_backend->mount_try, dir, op_backend->mount_cancelled, 
             errno, g_strerror (errno));

      if (dir == NULL && 
          (op_backend->mount_cancelled || (errno != EPERM && errno != EACCES)))
        {
	  DEBUG ("do_mount - (errno != EPERM && errno != EACCES), breaking\n");
	  break;
	}

      if (dir != NULL)
        {
          /*  Let update_cache() do enumeration, check for the smbc_getdents() result */
          res = update_cache (op_backend, dir);
          smbc_closedir (smb_context, dir);
          DEBUG ("do_mount - login successful, res = %d\n", res);
          if (res)
            break;
        }
	else {
	  /*  Purge the cache, we need to have clean playground for next auth try  */
	  purge_cached (smb_context);
	}

      /* The first round is Kerberos-only.  Only if this fails do we enable
       * NTLMSSP fallback (turning off anonymous fallback, which we've
       * already tried and failed with).
       */
      if (op_backend->mount_try == 0)
        {
          DEBUG ("do_mount - after anon, enabling NTLMSSP fallback\n");
          smbc_setOptionFallbackAfterKerberos (op_backend->smb_context, 1);
          smbc_setOptionNoAutoAnonymousLogin (op_backend->smb_context, 1);
        }
      op_backend->mount_try++;
    }
  while (op_backend->mount_try_again);

  g_string_free (uri, TRUE);

  op_backend->mount_source = NULL;

  if (dir == NULL)
    {
      if (op_backend->mount_cancelled)
        g_vfs_job_failed (G_VFS_JOB (job),
                         G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED,
                         _("Password dialog cancelled"));
      else
        /* TODO: Error from errno? */
        g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_FAILED,
			  /* translators: We tried to mount a windows (samba) share, but failed */
			  _("Failed to retrieve share list from server"));

      return;
    }

  g_vfs_keyring_save_password (op_backend->last_user,
			       op_backend->server,
			       op_backend->last_domain,
			       "smb",
			       NULL,
			       NULL,
			       0,
			       op_backend->last_password,
			       op_backend->password_save);

  g_vfs_job_succeeded (G_VFS_JOB (job));
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
  op_backend->mounted_server = g_strdup (server);
  
  return FALSE;
}

static void
run_mount_mountable (GVfsBackendSmbBrowse *backend,
		     GVfsJobMountMountable *job,
		     const char *filename,
		     GMountSource *mount_source)
{
  BrowseEntry *entry;
  GError *error = NULL;
  GMountSpec *mount_spec;

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
	g_set_error_literal (&error,
			     G_IO_ERROR, G_IO_ERROR_NOT_MOUNTABLE_FILE,
			     _("The file is not a mountable"));
    }
  else
    g_set_error_literal (&error,
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

  update_cache (op_backend, NULL);

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

  update_cache (op_backend, NULL);

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
  GIcon *icon;
  
  g_file_info_set_name (info, entry->name);
  g_file_info_set_display_name (info, entry->name_utf8);
  g_file_info_set_edit_name (info, entry->name_utf8);
  g_file_info_set_attribute_string (info, "smb::comment", entry->comment);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
  g_file_info_set_content_type (info, "inode/directory");

  icon = NULL;
  if (entry->smbc_type == SMBC_WORKGROUP)
    icon = g_themed_icon_new ("network-workgroup");
  else if (entry->smbc_type == SMBC_SERVER)
    icon = g_themed_icon_new ("network-server");
  else
    icon = g_themed_icon_new ("folder-remote");

  if (icon)
    {
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
    }
  
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
    
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, uri->str);
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

  update_cache (op_backend, NULL);

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
  GIcon *icon;

  if (is_root (filename))
    {
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_name (info, "/");
      g_file_info_set_display_name (info, g_vfs_backend_get_display_name (backend));
      g_file_info_set_content_type (info, "inode/directory");
      icon = g_vfs_backend_get_icon (backend);
      if (icon != NULL)
        g_file_info_set_icon (info, icon);
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

  update_cache (op_backend, NULL);

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
#ifdef HAVE_GCONF
  GConfClient *gclient;
#endif
  
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

#ifdef HAVE_GCONF
  gclient = gconf_client_get_default ();
  if (gclient)
    {
      char *workgroup;
      
      workgroup = gconf_client_get_string (gclient, 
					   PATH_GCONF_GNOME_VFS_SMB_WORKGROUP, NULL);

      if (workgroup && workgroup[0])
	default_workgroup = workgroup;
      else
	g_free (workgroup);
      
      g_object_unref (gclient);
    }
#endif

  DEBUG ("g_vfs_backend_smb_browse_class_init - default workgroup = '%s'\n", default_workgroup ? default_workgroup : "NULL");
}

void
g_vfs_smb_browse_daemon_init (void)
{
  g_set_application_name (_("Windows Network Filesystem Service"));
}
