#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gvfserror.h>
#include <gio/gfile.h>
#include <gio/gfilelocal.h>

#include "gvfsbackendsmb.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobgetinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"

#include <libsmbclient.h>

struct _GVfsBackendSmb
{
  GVfsBackend parent_instance;

  char *server;
  char *share;
  char *user;
  char *domain;
  
  SMBCCTX *smb_context;

  char *last_user;
  char *last_domain;
  char *last_password;
  
  GMountSource *mount_source; /* Only used/set during mount */
  int mount_try;
  gboolean mount_try_again;
  
  /* Cache */
  char *cached_server_name;
  char *cached_share_name;
  char *cached_domain;
  char *cached_username;
  SMBCSRV *cached_server;
};

G_DEFINE_TYPE (GVfsBackendSmb, g_vfs_backend_smb, G_VFS_TYPE_BACKEND);

static void
g_vfs_backend_smb_finalize (GObject *object)
{
  GVfsBackendSmb *backend;

  backend = G_VFS_BACKEND_SMB (object);

  g_free (backend->share);
  g_free (backend->server);
  g_free (backend->user);
  g_free (backend->domain);
  
  if (G_OBJECT_CLASS (g_vfs_backend_smb_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_smb_parent_class)->finalize) (object);
}

static void
g_vfs_backend_smb_init (GVfsBackendSmb *backend)
{
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
  GVfsBackendSmb *backend;
  char *ask_password, *ask_user, *ask_domain;
  gboolean handled, abort;

  backend = smbc_option_get (context, "user_data");

  strncpy (password_out, "", pwmaxlen);
  
  if (backend->domain)
    strncpy (domain_out, backend->domain, domainmaxlen);
  if (backend->user)
    strncpy (username_out, backend->user, unmaxlen);

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
      /* Try anon login */
      strncpy (username_out, "", unmaxlen);
      strncpy (password_out, "", pwmaxlen);
      /* Try again if anon login fails */
      backend->mount_try_again = TRUE;
    }
  else
    {
      GPasswordFlags flags = G_PASSWORD_FLAGS_NEED_PASSWORD;
      char *message;
      
      if (backend->domain == NULL)
	flags |= G_PASSWORD_FLAGS_NEED_DOMAIN;
      if (backend->user == NULL)
	flags |= G_PASSWORD_FLAGS_NEED_USERNAME;

      message = g_strdup_printf (_("Password required for share %s on %s"),
				 server_name, share_name);
      handled = g_mount_source_ask_password (backend->mount_source,
					     message,
					     username_out,
					     domain_out,
					     flags,
					     &abort,
					     &ask_password,
					     &ask_user,
					     &ask_domain);
      g_free (message);
      if (!handled)
	goto out;
      
      if (abort)
	{
	  strncpy (username_out, "ABORT", unmaxlen);
	  strncpy (password_out, "", pwmaxlen);
	  goto out;
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
  GVfsBackendSmb *backend;

  backend = smbc_option_get (context, "user_data");
  
  if (backend->cached_server != NULL)
    return 1;

  backend->cached_server_name = g_strdup (server_name);
  backend->cached_share_name = g_strdup (share_name);
  backend->cached_domain = g_strdup (domain);
  backend->cached_username = g_strdup (username);
  backend->cached_server = new;

  return 0;
}

/* Remove cached server
 *
 * @param c         pointer to smb context
 * @param srv       pointer to server to remove
 * @return          0 when found and removed. 1 on failure.
 *
 */ 
static int
remove_cached_server(SMBCCTX * context, SMBCSRV * server)
{
  GVfsBackendSmb *backend;

  backend = smbc_option_get (context, "user_data");
  
  if (backend->cached_server == server)
    {
      g_free (backend->cached_server_name);
      backend->cached_server_name = NULL;
      g_free (backend->cached_share_name);
      backend->cached_share_name = NULL;
      g_free (backend->cached_domain);
      backend->cached_domain = NULL;
      g_free (backend->cached_username);
      backend->cached_username = NULL;
      backend->cached_server = NULL;
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
  GVfsBackendSmb *backend;

  backend = smbc_option_get (context, "user_data");

  if (backend->cached_server != NULL &&
      strcmp (backend->cached_server_name, server_name) == 0 &&
      strcmp (backend->cached_share_name, share_name) == 0 &&
      strcmp (backend->cached_domain, domain) == 0 &&
      strcmp (backend->cached_username, username) == 0)
    return backend->cached_server;

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
  GVfsBackendSmb *backend;
  
  backend = smbc_option_get (context, "user_data");

  if (backend->cached_server)
    remove_cached_server(context, backend->cached_server);
  
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
			 const char *reserved_chars_allowed)
{
  char c;
  static const gchar hex[16] = "0123456789ABCDEF";
  
  while ((c = *encoded++) != 0)
    {
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

static GString *
create_smb_uri_string (const char *server,
		       const char *share,
		       const char *path)
{
  GString *uri;

  uri = g_string_new ("smb://");
  g_string_append_encoded (uri, server, NULL);
  g_string_append_c (uri, '/');
  g_string_append_encoded (uri, share, NULL);
  if (path != NULL)
    {
      if (*path != '/')
	g_string_append_c (uri, '/');
      g_string_append_encoded (uri, path, SUB_DELIM_CHARS ":@/");
    }
  
  return uri;
}

static char *
create_smb_uri (const char *server,
		const char *share,
		const char *path)
{
  GString *uri;
  uri = create_smb_uri_string (server, share, path);
  return g_string_free (uri, FALSE);
}

static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  SMBCCTX *smb_context;
  struct stat st;
  char *uri;
  int res;
  char *display_name;
  GMountSpec *smb_mount_spec;

  smb_context = smbc_new_context ();
  if (smb_context == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_FILE_ERROR, G_FILE_ERROR_IO,
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
  smb_context->flags |= SMBCCTX_FLAG_NO_AUTO_ANONYMOUS_LOGON;
#endif
#endif

#if 0
  smbc_option_set(smb_context, "debug_stderr", (void *) 1);
#endif
  
  if (!smbc_init_context (smb_context))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_FILE_ERROR, G_FILE_ERROR_IO,
			"Failed to initialize smb context");
      smbc_free_context (smb_context, FALSE);
      return;
    }

  op_backend->smb_context = smb_context;

  display_name = g_strdup_printf ("%s on %s", op_backend->share, op_backend->server);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  
  smb_mount_spec = g_mount_spec_new ("smb-share");
  g_mount_spec_set (smb_mount_spec, "share", op_backend->share);
  g_mount_spec_set (smb_mount_spec, "server", op_backend->server);
  if (op_backend->user)
    g_mount_spec_set (smb_mount_spec, "user", op_backend->user);
  if (op_backend->domain)
    g_mount_spec_set (smb_mount_spec, "domain", op_backend->domain);
  
  g_vfs_backend_set_mount_spec (backend, smb_mount_spec);
  g_mount_spec_unref (smb_mount_spec);

  uri = create_smb_uri (op_backend->server, op_backend->share, NULL);

  op_backend->mount_source = mount_source;
  op_backend->mount_try = 0;

  do
    {
      op_backend->mount_try_again = FALSE;
      
      res = smb_context->stat (smb_context, uri, &st);
      
      if (res == 0 ||
	  (errno != EACCES && errno != EPERM))
	break;
      
      op_backend->mount_try ++;      
    }
  while (op_backend->mount_try_again);
  
  g_free (uri);
  
  op_backend->mount_source = NULL;

  if (res != 0)
    {
      /* TODO: Error from errno? */
      op_backend->mount_source = NULL;
      g_vfs_job_failed (G_VFS_JOB (job),
			G_FILE_ERROR, G_FILE_ERROR_IO,
			"Failed to mount smb share");
      return;
    }
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount (GVfsBackend *backend,
	   GVfsJobMount *job,
	   GMountSpec *mount_spec,
	   GMountSource *mount_source)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  const char *server, *share, *user, *domain;

  server = g_mount_spec_get (mount_spec, "server");
  share = g_mount_spec_get (mount_spec, "share");

  if (server == NULL || share == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_FILE_ERROR, G_FILE_ERROR_INVAL,
			_("Invalid mount spec"));
      return TRUE;
    }

  user = g_mount_spec_get (mount_spec, "user");
  domain = g_mount_spec_get (mount_spec, "domain");
  
  op_backend->server = g_strdup (server);
  op_backend->share = g_strdup (share);
  op_backend->user = g_strdup (user);
  op_backend->domain = g_strdup (domain);
  
  return FALSE;
}

static void 
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  const char *filename)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  char *uri;
  SMBCFILE *file;
  GError *error;

  uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  file = op_backend->smb_context->open (op_backend->smb_context, uri, O_RDONLY, 0);
  g_free (uri);

  if (file == NULL)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   g_strerror (errno));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      g_vfs_job_open_for_read_set_can_seek (job, TRUE);
      g_vfs_job_open_for_read_set_handle (job, file);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_read (GVfsBackend *backend,
	 GVfsJobRead *job,
	 GVfsBackendHandle handle,
	 char *buffer,
	 gsize bytes_requested)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  GError *error;
  ssize_t res;

  res = op_backend->smb_context->read (op_backend->smb_context, (SMBCFILE *)handle, buffer, bytes_requested);

  if (res == -1)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   g_strerror (errno));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      g_vfs_job_read_set_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
	    
    }
}

static void
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  GError *error;
  int whence;
  off_t res;

  switch (type)
    {
    case G_SEEK_SET:
      whence = SEEK_SET;
      break;
    case G_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case G_SEEK_END:
      whence = SEEK_END;
      break;
    default:
      error = NULL;
      g_set_error (&error, G_VFS_ERROR,
		   G_VFS_ERROR_NOT_SUPPORTED,
		   _("Unsupported seek type"));
      goto error;
    }

  res = op_backend->smb_context->lseek (op_backend->smb_context, (SMBCFILE *)handle, offset, whence);

  if (res == (off_t)-1)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   g_strerror (errno));
      goto error;
    }
  else
    {
      g_vfs_job_seek_read_set_offset (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

  return;

 error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  GError *error;
  ssize_t res;

  res = op_backend->smb_context->close_fn (op_backend->smb_context, (SMBCFILE *)handle);
  if (res == -1)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   g_strerror (errno));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
	    
    }
}

static void
do_get_info (GVfsBackend *backend,
	     GVfsJobGetInfo *job,
	     const char *filename,
	     GFileInfoRequestFlags requested,
	     const char *attributes,
	     gboolean follow_symlinks)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat st;
  char *uri;
  int res;
  GFileInfo *info;
  
  uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  res = op_backend->smb_context->stat (op_backend->smb_context, uri, &st);
  g_free (uri);

  if (res == 0)
    {
      info = g_file_info_new ();
      g_file_info_set_from_stat (info, requested, &st);
      
      g_vfs_job_get_info_set_info (job, requested & ~(G_FILE_INFO_DISPLAY_NAME|G_FILE_INFO_EDIT_NAME), info);
      g_vfs_job_succeeded (G_VFS_JOB (job));
      g_object_unref (info);
    }
  else
    g_vfs_job_failed_from_error (G_VFS_JOB (job), NULL); /* TODO */
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      GFileInfoRequestFlags requested,
	      const char *attributes,
	      gboolean follow_symlinks)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat st;
  int res;
  GError *error;
  SMBCFILE *dir;
  char dirents[1024*4];
  struct smbc_dirent *dirp;
  GList *files;
  GFileInfo *info;
  GString *uri;
  int uri_start_len;

  uri = create_smb_uri_string (op_backend->server, op_backend->share, filename);
  dir = op_backend->smb_context->opendir (op_backend->smb_context, uri->str);

  if (dir == NULL)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   g_strerror (errno));
      goto error;
    }

  /* TODO: limit requested to what we support */
  g_vfs_job_enumerate_set_result (job, requested);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  if (uri->str[uri->len - 1] != '/')
    g_string_append_c (uri, '/');
  uri_start_len = uri->len;
  
  while (TRUE)
    {
      files = NULL;
      
      res = op_backend->smb_context->getdents (op_backend->smb_context, dir, (struct smbc_dirent *)dirents, sizeof (dirents));
      if (res <= 0)
	break;
      
      dirp = (struct smbc_dirent *)dirents;
      while (res > 0)
	{
	  unsigned int dirlen;

	  /* TODO: Only do stat if required for flags */
	  
	  if ((dirp->smbc_type == SMBC_DIR ||
	       dirp->smbc_type == SMBC_FILE ||
	       dirp->smbc_type == SMBC_LINK) &&
	      strcmp (dirp->name, ".") != 0 &&
	      strcmp (dirp->name, "..") != 0)
	    {
	      int stat_res;
	      g_string_truncate (uri, uri_start_len);
	      g_string_append_encoded (uri,
				       dirp->name,
				       SUB_DELIM_CHARS ":@/");
	      
	      stat_res = op_backend->smb_context->stat (op_backend->smb_context,
							uri->str, &st);
	      if (stat_res == 0)
		{
		  info = g_file_info_new ();
		  if (requested & G_FILE_INFO_NAME)
		    g_file_info_set_name (info, dirp->name);
		  
		  g_file_info_set_from_stat (info, requested, &st);
		  files = g_list_prepend (files, info);
		}
	    }
		  
	  dirlen = dirp->dirlen;
	  dirp = (struct smbc_dirent *) (((char *)dirp) + dirlen);
	  res -= dirlen;
	}
      
      if (files)
	{
	  files = g_list_reverse (files);
	  g_vfs_job_enumerate_add_info (job, files);
	  g_list_foreach (files, (GFunc)g_object_unref, NULL);
	  g_list_free (files);
	}
    }
      
  res = op_backend->smb_context->closedir (op_backend->smb_context, dir);

  g_vfs_job_enumerate_done (job);

  g_string_free (uri, TRUE);
  return;
  
 error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
  g_string_free (uri, TRUE);
}

static void
g_vfs_backend_smb_class_init (GVfsBackendSmbClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_smb_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->get_info = do_get_info;
  backend_class->enumerate = do_enumerate;
}
