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

G_DEFINE_TYPE (GVfsBackendSmb, g_vfs_backend_smb, G_TYPE_VFS_BACKEND);

static GVfsBackendSmb *smb_backend = NULL;

static void
g_vfs_backend_smb_finalize (GObject *object)
{
  GVfsBackendSmb *backend;

  backend = G_VFS_BACKEND_SMB (object);

  g_free (backend->share);
  g_free (backend->server);
  
  if (G_OBJECT_CLASS (g_vfs_backend_smb_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_smb_parent_class)->finalize) (object);
}

static void
g_vfs_backend_smb_init (GVfsBackendSmb *backend)
{
}

/* Authentication callback function type (traditional method)
 * 
 * Type for the the authentication function called by the library to
 * obtain authentication credentals
 *
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
auth_callback (const char *server_name, const char *share_name,
	       char *domain_out, int domainmaxlen,
	       char *username_out, int unmaxlen,
	       char *password_out, int pwmaxlen)
{
  g_print ("auth_callback: %s %s\n", server_name, share_name);
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
  g_print ("add_cached_server\n");
  if (smb_backend->cached_server != NULL)
    return 1;

  smb_backend->cached_server_name = g_strdup (server_name);
  smb_backend->cached_share_name = g_strdup (share_name);
  smb_backend->cached_domain = g_strdup (domain);
  smb_backend->cached_username = g_strdup (username);
  smb_backend->cached_server = new;

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
  g_print ("remove_cached_server\n");
  if (smb_backend->cached_server == server)
    {
      g_free (smb_backend->cached_server_name);
      smb_backend->cached_server_name = NULL;
      g_free (smb_backend->cached_share_name);
      smb_backend->cached_share_name = NULL;
      g_free (smb_backend->cached_domain);
      smb_backend->cached_domain = NULL;
      g_free (smb_backend->cached_username);
      smb_backend->cached_username = NULL;
      smb_backend->cached_server = NULL;
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
  g_print ("get_cached_server %s, %s\n", server_name, share_name);
  
  if (smb_backend->cached_server != NULL &&
      strcmp (smb_backend->cached_server_name, server_name) == 0 &&
      strcmp (smb_backend->cached_share_name, share_name) == 0 &&
      strcmp (smb_backend->cached_domain, domain) == 0 &&
      strcmp (smb_backend->cached_username, username) == 0)
    return smb_backend->cached_server;

  g_print ("get_cached_server -> miss\n");
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
  if (smb_backend->cached_server)
    remove_cached_server(context, smb_backend->cached_server);
  
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

GVfsBackendSmb *
g_vfs_backend_smb_new (GMountSpec *spec,
		       GError **error)
{
  GVfsBackendSmb *backend;
  GVfsBackend *_backend;
  SMBCCTX *smb_context;
  int res;
  char *uri;
  struct stat st;
  const char *server;
  const char *share;

  g_assert (smb_backend == NULL);

  
  server = g_mount_spec_get (spec, "server");
  share = g_mount_spec_get (spec, "share");

  if (server == NULL ||
      share == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
		   _("Invalid mount spec"));
      return NULL;
    }
 
  
  
  smb_context = smbc_new_context ();
  if (smb_context == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Failed to allocate smb context");
      return NULL;
    }

  smb_context->debug = 0;
  smb_context->callbacks.auth_fn 		     = auth_callback;
  
  smb_context->callbacks.add_cached_srv_fn    = add_cached_server;
  smb_context->callbacks.get_cached_srv_fn    = get_cached_server;
  smb_context->callbacks.remove_cached_srv_fn = remove_cached_server;
  smb_context->callbacks.purge_cached_fn      = purge_cached;
  
#if defined(HAVE_SAMBA_FLAGS) 
#if defined(SMB_CTX_FLAG_USE_KERBEROS) && defined(SMB_CTX_FLAG_FALLBACK_AFTER_KERBEROS)
  smb_context->flags |= SMB_CTX_FLAG_USE_KERBEROS | SMB_CTX_FLAG_FALLBACK_AFTER_KERBEROS;
#endif
#if defined(SMBCCTX_FLAG_NO_AUTO_ANONYMOUS_LOGON)
  //smb_context->flags |= SMBCCTX_FLAG_NO_AUTO_ANONYMOUS_LOGON;
#endif
#endif

  if (0) 
    smbc_option_set(smb_context, "debug_stderr", (void *) 1);


  if (!smbc_init_context (smb_context))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Failed to initialize smb context");
      smbc_free_context (smb_context, FALSE);
      return NULL;
    }
  
  backend = g_object_new (G_TYPE_VFS_BACKEND_SMB,
			  NULL);

  backend->server = g_strdup (server);
  backend->share = g_strdup (share);

  _backend = G_VFS_BACKEND (backend);
  _backend->display_name = g_strdup_printf ("%s on %s", share, server);
  _backend->mount_spec = g_mount_spec_new ("smb-share");
  g_mount_spec_set (_backend->mount_spec,
		    "share", share);
  g_mount_spec_set (_backend->mount_spec,
		    "server", server);
  
  smb_backend = backend;
  backend->smb_context = smb_context;
  
  uri = create_smb_uri (server, share, NULL);
  res = smb_context->stat (smb_context, uri, &st);
  g_free (uri);
  if (res != 0)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Failed to mount smb share");
      g_object_unref (backend);
      return NULL;
    }
  
  return backend;
}

static void 
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  char *filename)
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
	     char *filename,
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
	      char *filename,
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

  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->get_info = do_get_info;
  backend_class->enumerate = do_enumerate;
}
