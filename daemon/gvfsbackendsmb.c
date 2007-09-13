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

#include "gvfsbackendsmb.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobgetinfo.h"
#include "gvfsjobgetfsinfo.h"
#include "gvfsjobqueryattributes.h"
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

  while (uri->len > 0 &&
	 uri->str[uri->len - 1] == '/')
    g_string_erase (uri, uri->len - 1, 1);
  
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
	  GMountSource *mount_source,
	  gboolean is_automount)
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
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Failed to allocate smb context"));
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
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Failed to initialize smb context"));
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
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Failed to mount smb share"));
      return;
    }
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount (GVfsBackend *backend,
	   GVfsJobMount *job,
	   GMountSpec *mount_spec,
	   GMountSource *mount_source,
	   gboolean is_automount)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  const char *server, *share, *user, *domain;

  server = g_mount_spec_get (mount_spec, "server");
  share = g_mount_spec_get (mount_spec, "share");

  if (server == NULL || share == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
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

  uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  file = op_backend->smb_context->open (op_backend->smb_context, uri, O_RDONLY, 0);
  g_free (uri);

  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
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
  ssize_t res;

  res = op_backend->smb_context->read (op_backend->smb_context, (SMBCFILE *)handle, buffer, bytes_requested);

  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
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
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Unsupported seek type"));
      return;
    }

  res = op_backend->smb_context->lseek (op_backend->smb_context, (SMBCFILE *)handle, offset, whence);

  if (res == (off_t)-1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_seek_read_set_offset (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

  return;
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  ssize_t res;

  res = op_backend->smb_context->close_fn (op_backend->smb_context, (SMBCFILE *)handle);
  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

typedef struct {
  SMBCFILE *file;
  char *uri;
  char *tmp_uri;
  char *backup_uri;
} SmbWriteHandle;

static void
smb_write_handle_free (SmbWriteHandle *handle)
{
  g_free (handle->uri);
  g_free (handle->tmp_uri);
  g_free (handle->backup_uri);
  g_free (handle);
}

static void
do_create (GVfsBackend *backend,
	   GVfsJobOpenForWrite *job,
	   const char *filename)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  char *uri;
  SMBCFILE *file;
  SmbWriteHandle *handle;

  uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  file = op_backend->smb_context->open (op_backend->smb_context, uri,
					O_CREAT|O_WRONLY|O_EXCL, 0666);
  g_free (uri);

  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      handle = g_new0 (SmbWriteHandle, 1);
      handle->file = file;

      g_vfs_job_open_for_write_set_can_seek (job, TRUE);
      g_vfs_job_open_for_write_set_handle (job, handle);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_append_to (GVfsBackend *backend,
	      GVfsJobOpenForWrite *job,
	      const char *filename)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  char *uri;
  SMBCFILE *file;
  SmbWriteHandle *handle;
  off_t initial_offset;

  uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  file = op_backend->smb_context->open (op_backend->smb_context, uri,
					O_CREAT|O_WRONLY|O_APPEND, 0666);
  g_free (uri);

  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      handle = g_new0 (SmbWriteHandle, 1);
      handle->file = file;

      initial_offset = op_backend->smb_context->lseek (op_backend->smb_context, file,
						       0, SEEK_CUR);
      if (initial_offset == (off_t) -1)
	g_vfs_job_open_for_write_set_can_seek (job, FALSE);
      else
	{
	  g_vfs_job_open_for_write_set_initial_offset (job, initial_offset);
	  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
	}
      g_vfs_job_open_for_write_set_handle (job, handle);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}


static void
random_chars (char *str, int len)
{
  int i;
  const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  for (i = 0; i < len; i++)
    str[i] = chars[g_random_int_range (0, strlen(chars))];
}

static char *
get_dir_from_uri (const char *uri)
{
  const char *prefix_end;

  prefix_end = uri + strlen (uri);

  /* Skip slashes at end */
  while (prefix_end > uri &&
	 *(prefix_end - 1) == '/')
    prefix_end--;

  /* Skip to next slash */
  while (prefix_end > uri &&
	 *(prefix_end - 1) != '/')
    prefix_end--;

  return g_strndup (uri, prefix_end - uri);
}

static SMBCFILE *
open_tmpfile (GVfsBackendSmb *backend,
	      const char *uri,
	      char **tmp_uri_out)
{
  char *dir_uri, *tmp_uri;
  char filename[] = "~gvfXXXX.tmp";
  SMBCFILE *file;

  dir_uri = get_dir_from_uri (uri);
 
  do {
    random_chars (filename + 4, 4);
    tmp_uri = g_strconcat (dir_uri, filename, NULL);

    file = backend->smb_context->open (backend->smb_context, tmp_uri,
				       O_CREAT|O_WRONLY|O_EXCL, 0666);
  } while (file == NULL && errno == EEXIST);

  g_free (dir_uri);
  
  if (file)
    {
      *tmp_uri_out = tmp_uri;
      return file;
    }
  else
    {
      g_free (tmp_uri);
      return NULL;
    }
}

static gboolean
copy_file (GVfsBackendSmb *backend,
	   GVfsJob *job,
	   const char *from_uri,
	   const char *to_uri)
{
  SMBCFILE *from_file, *to_file;
  char buffer[4096];
  size_t buffer_size;
  ssize_t res;
  char *p;
  gboolean succeeded;
  

  from_file = NULL;
  to_file = NULL;

  succeeded = FALSE;
  
  from_file = backend->smb_context->open (backend->smb_context, from_uri,
					  O_RDONLY, 0666);
  if (from_file == NULL || g_vfs_job_is_cancelled (job))
    goto out;
  
  to_file = backend->smb_context->open (backend->smb_context, to_uri,
					O_CREAT|O_WRONLY|O_TRUNC, 0666);

  if (from_file == NULL || g_vfs_job_is_cancelled (job))
    goto out;

  while (1)
    {
      
      res = backend->smb_context->read (backend->smb_context, from_file,
					buffer, sizeof(buffer));
      if (res < 0 || g_vfs_job_is_cancelled (job))
	goto out;
      if (res == 0)
	break; /* Succeeded */

      buffer_size = res;
      p = buffer;
      while (buffer_size > 0)
	{
	  res = backend->smb_context->write (backend->smb_context, to_file,
					     p, buffer_size);
	  if (res < 0 || g_vfs_job_is_cancelled (job))
	    goto out;
	  buffer_size -= res;
	  p += res;
	}
    }
  succeeded = TRUE;
 
 out: 
  if (to_file)
    backend->smb_context->close_fn (backend->smb_context, to_file);
  if (from_file)
    backend->smb_context->close_fn (backend->smb_context, from_file);
  return succeeded;
}

static char *
create_etag (struct stat *statbuf)
{
  return g_strdup_printf ("%ld", statbuf->st_mtime);
}

static void
do_replace (GVfsBackend *backend,
	    GVfsJobOpenForWrite *job,
	    const char *filename,
	    const char *etag,
	    gboolean make_backup)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat original_stat;
  int res;
  char *uri, *tmp_uri, *backup_uri, *current_etag;
  SMBCFILE *file;
  GError *error = NULL;
  SmbWriteHandle *handle;

  uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  tmp_uri = NULL;
  if (make_backup)
    backup_uri = g_strconcat (uri, "~", NULL);
  else
    backup_uri = NULL;
  
  file = op_backend->smb_context->open (op_backend->smb_context, uri,
					O_CREAT|O_WRONLY|O_EXCL, 0);
  if (file == NULL && errno != EEXIST)
    {
      g_set_error (&error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   g_strerror (errno));
      goto error;
    }
  else if (file == NULL && errno == EEXIST)
    {
      if (etag != NULL)
	{
	  res = op_backend->smb_context->stat (op_backend->smb_context, uri, &original_stat);
	  
	  if (res == 0)
	    {
	      current_etag = create_etag (&original_stat);
	      if (strcmp (etag, current_etag) != 0)
		{
		  g_free (current_etag);
		  g_set_error (&error,
			       G_IO_ERROR,
			       G_IO_ERROR_WRONG_ETAG,
			       _("The file was externally modified"));
		  goto error;
		}
	      g_free (current_etag);
	    }
	}
      
      /* Backup strategy:
       *
       * By default we:
       *  1) save to a tmp file (that doesn't exist already)
       *  2) rename orig file to backup file
       *     (or delete it if no backup)
       *  3) rename tmp file to orig file
       *
       * However, this can fail if we can't write to the directory.
       * In that case we just truncate the file, after having 
       * copied directly to the backup filename.
       */

      file = open_tmpfile (op_backend, uri, &tmp_uri);
      if (file == NULL)
	{
	  if (make_backup)
	    {
	      if (!copy_file (op_backend, G_VFS_JOB (job), uri, backup_uri))
		{
		  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
		    g_set_error (&error,
				 G_IO_ERROR,
				 G_IO_ERROR_CANCELLED,
				 _("Operation was cancelled"));
		  else
		    g_set_error (&error,
				 G_IO_ERROR,
				 G_IO_ERROR_CANT_CREATE_BACKUP,
				 _("Backup file creation failed"));
		  goto error;
		}
	      g_free (backup_uri);
	      backup_uri = NULL;
	    }
	  
	  file = op_backend->smb_context->open (op_backend->smb_context, uri,
						O_CREAT|O_WRONLY|O_TRUNC, 0);
	  if (file == NULL)
	    {
	      g_set_error (&error, G_IO_ERROR,
			   g_io_error_from_errno (errno),
			   g_strerror (errno));
	      goto error;
	    }
	}
    }
  else
    {
      /* Doesn't exist. Just write away */
      g_free (backup_uri);
      backup_uri = NULL;
    }

  handle = g_new (SmbWriteHandle, 1);
  handle->file = file;
  handle->uri = uri;
  handle->tmp_uri = tmp_uri;
  handle->backup_uri = backup_uri;
  
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_handle (job, handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  return;
  
 error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
  g_free (backup_uri);
  g_free (tmp_uri);
  g_free (uri);
}


static void
do_write (GVfsBackend *backend,
	  GVfsJobWrite *job,
	  GVfsBackendHandle _handle,
	  char *buffer,
	  gsize buffer_size)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  SmbWriteHandle *handle = _handle;
  ssize_t res;

  res = op_backend->smb_context->write (op_backend->smb_context, handle->file,
					buffer, buffer_size);
  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_write_set_written_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_seek_on_write (GVfsBackend *backend,
		  GVfsJobSeekWrite *job,
		  GVfsBackendHandle _handle,
		  goffset    offset,
		  GSeekType  type)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  SmbWriteHandle *handle = _handle;
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
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Unsupported seek type"));
      return;
    }

  res = op_backend->smb_context->lseek (op_backend->smb_context, handle->file, offset, whence);

  if (res == (off_t)-1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_seek_write_set_offset (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

  return;
}

static void
do_close_write (GVfsBackend *backend,
		GVfsJobCloseWrite *job,
		GVfsBackendHandle _handle)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  SmbWriteHandle *handle = _handle;
  struct stat stat_at_close;
  int stat_res;
  ssize_t res;

  stat_res = op_backend->smb_context->fstat (op_backend->smb_context, handle->file, &stat_at_close);
  
  res = op_backend->smb_context->close_fn (op_backend->smb_context, handle->file);

  if (res == -1)
    {
      g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
      
      if (handle->tmp_uri)
	op_backend->smb_context->unlink (op_backend->smb_context, handle->tmp_uri);
      goto out;
    }

  if (handle->tmp_uri)
    {
      if (handle->backup_uri)
	{
	  res = op_backend->smb_context->rename (op_backend->smb_context, handle->uri,
						 op_backend->smb_context, handle->backup_uri);
	  if (res ==  -1)
	    {
	      op_backend->smb_context->unlink (op_backend->smb_context, handle->tmp_uri);
	      g_vfs_job_failed (G_VFS_JOB (job),
				G_IO_ERROR, G_IO_ERROR_CANT_CREATE_BACKUP,
				_("Backup file creation failed: %d"), errno);
	      goto out;
	    }
	}
      else
	op_backend->smb_context->unlink (op_backend->smb_context, handle->uri);
      
      res = op_backend->smb_context->rename (op_backend->smb_context, handle->tmp_uri,
					     op_backend->smb_context, handle->uri);
      if (res ==  -1)
	{
	  op_backend->smb_context->unlink (op_backend->smb_context, handle->tmp_uri);
	  g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
	  goto out;
	}
    }
  
  if (stat_res == 0)
    {
      char *etag;
      etag = create_etag (&stat_at_close);
      g_vfs_job_close_write_set_etag (job, etag);
      g_free (etag);
    }
  
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  smb_write_handle_free (handle);  
}

static void
set_info_from_stat (GFileInfo *info, struct stat *statbuf,
		    GFileAttributeMatcher *matcher)
{
  GFileType file_type;
  GTimeVal t;

  file_type = G_FILE_TYPE_UNKNOWN;

  if (S_ISREG (statbuf->st_mode))
    file_type = G_FILE_TYPE_REGULAR;
  else if (S_ISDIR (statbuf->st_mode))
    file_type = G_FILE_TYPE_DIRECTORY;
  else if (S_ISCHR (statbuf->st_mode) ||
	   S_ISBLK (statbuf->st_mode) ||
	   S_ISFIFO (statbuf->st_mode)
#ifdef S_ISSOCK
	   || S_ISSOCK (statbuf->st_mode)
#endif
	   )
    file_type = G_FILE_TYPE_SPECIAL;
  else if (S_ISLNK (statbuf->st_mode))
    file_type = G_FILE_TYPE_SYMBOLIC_LINK;

  g_file_info_set_file_type (info, file_type);
  g_file_info_set_size (info, statbuf->st_size);

  t.tv_sec = statbuf->st_mtime;
#if defined (HAVE_STRUCT_STAT_ST_MTIMENSEC)
  t.tv_usec = statbuf->st_mtimensec / 1000;
#elif defined (HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
  t.tv_usec = statbuf->st_mtim.tv_nsec / 1000;
#else
  t.tv_usec = 0;
#endif
  g_file_info_set_modification_time (info, &t);

  /* Don't trust n_link, uid, gid, etc returned from libsmb, its just made up.
     These are ok though: */

  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE, statbuf->st_dev);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE, statbuf->st_ino);

  /* If file is dos-readonly, libsmbclient doesn't set S_IWUSR, we use this to
     trigger ACCESS_WRITE = FALSE: */
  if (!(statbuf->st_mode & S_IWUSR))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, statbuf->st_atime);
#if defined (HAVE_STRUCT_STAT_ST_ATIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, statbuf->st_atimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, statbuf->st_atim.tv_nsec / 1000);
#endif
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CHANGED, statbuf->st_ctime);
#if defined (HAVE_STRUCT_STAT_ST_CTIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC, statbuf->st_ctimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_CTIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC, statbuf->st_ctim.tv_nsec / 1000);
#endif

  /* Libsmb sets the X bit on files to indicate some special things: */
  if ((statbuf->st_mode & S_IFDIR) == 0) {
    
    if (statbuf->st_mode & S_IXOTH)
      g_file_info_set_is_hidden (info, TRUE);
    
    if (statbuf->st_mode & S_IXUSR)
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_DOS_IS_ARCHIVE, TRUE);
    
    if (statbuf->st_mode & S_IXGRP)
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_DOS_IS_SYSTEM, TRUE);
  }

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ETAG_VALUE))
    {
      char *etag = create_etag (statbuf);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
      g_free (etag);
    }
}

static void
do_get_info (GVfsBackend *backend,
	     GVfsJobGetInfo *job,
	     const char *filename,
	     const char *attributes,
	     GFileGetInfoFlags flags)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  GFileAttributeMatcher *matcher;
  struct stat st = {0};
  char *uri;
  int res, saved_errno;
  GFileInfo *info;

  uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  res = op_backend->smb_context->stat (op_backend->smb_context, uri, &st);
  saved_errno = errno;
  g_free (uri);

  if (res == 0)
    {
      matcher = g_file_attribute_matcher_new (attributes);
      
      info = g_file_info_new ();
      set_info_from_stat (info, &st, matcher);
      
      g_vfs_job_get_info_set_info (job, info);

      g_vfs_job_succeeded (G_VFS_JOB (job));
      g_object_unref (info);
      
      g_file_attribute_matcher_free (matcher);
    }
  else
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), saved_errno);

}

static void
do_get_fs_info (GVfsBackend *backend,
		GVfsJobGetFsInfo *job,
		const char *filename,
		const char *attributes)
{
  GFileInfo *info;

  info = g_file_info_new ();
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FS_TYPE, "cifs");
  
  g_vfs_job_get_fs_info_set_info (job, info);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_object_unref (info);
}


static gboolean
try_query_settable_attributes (GVfsBackend *backend,
			       GVfsJobQueryAttributes *job,
			       const char *filename)
{
  GFileAttributeInfoList *list;

  list = g_file_attribute_info_list_new ();

  /* TODO: Add all settable attributes here */
  /*
  g_file_attribute_info_list_add (list,
				  "smb:test",
				  G_FILE_ATTRIBUTE_TYPE_UINT32);
  */

  g_vfs_job_query_attributes_set_list (job, list);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  g_file_attribute_info_list_free (list);
  return TRUE;
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      const char *attributes,
	      GFileGetInfoFlags flags)
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
  GFileAttributeMatcher *matcher;

  uri = create_smb_uri_string (op_backend->server, op_backend->share, filename);
  dir = op_backend->smb_context->opendir (op_backend->smb_context, uri->str);

  if (dir == NULL)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   g_strerror (errno));
      goto error;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  if (uri->str[uri->len - 1] != '/')
    g_string_append_c (uri, '/');
  uri_start_len = uri->len;

  matcher = g_file_attribute_matcher_new (attributes);
  
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

	      if (matcher == NULL ||
		  g_file_attribute_matcher_matches_only (matcher, G_FILE_ATTRIBUTE_STD_NAME))
		{
		  info = g_file_info_new ();
		  g_file_info_set_name (info, dirp->name);
		  files = g_list_prepend (files, info);
		}
	      else
		{
		  stat_res = op_backend->smb_context->stat (op_backend->smb_context,
							    uri->str, &st);
		  if (stat_res == 0)
		    {
		      info = g_file_info_new ();
		      g_file_info_set_name (info, dirp->name);
		      
		      set_info_from_stat (info, &st, matcher);
		      files = g_list_prepend (files, info);
		    }
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
      
  g_file_attribute_matcher_free (matcher);

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
do_set_display_name (GVfsBackend *backend,
		     GVfsJobSetDisplayName *job,
		     const char *filename,
		     const char *display_name)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  char *from_uri, *to_uri;
  char *dirname, *new_path;
  int res, errsv;

  dirname = g_path_get_basename (filename);

  /* TODO: display name is in utf8, atm we assume libsmb uris
     are in utf8, but this might not be true if the user changed
     the smb.conf file. Can we check this and convert? */

  new_path = g_build_filename (dirname, display_name, NULL);
  g_free (dirname);
  
  from_uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  to_uri = create_smb_uri (op_backend->server, op_backend->share, new_path);
  
  res = op_backend->smb_context->rename (op_backend->smb_context, from_uri,
					 op_backend->smb_context, to_uri);
  errsv = errno;
  g_free (from_uri);
  g_free (to_uri);

  if (res != 0)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
  else
    {
      g_vfs_job_set_display_name_set_new_path (job, new_path);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  g_free (new_path);
}

static void
do_delete (GVfsBackend *backend,
	   GVfsJobDelete *job,
	   const char *filename)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat statbuf;
  char *uri;
  int errsv, res;

  uri = create_smb_uri (op_backend->server, op_backend->share, filename);

  res = op_backend->smb_context->stat (op_backend->smb_context, uri, &statbuf);
  if (res == -1)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR,
			g_io_error_from_errno (errno),
			_("Error deleting file: %s"),
			g_strerror (errno));
      g_free (uri);
      return;
    }

  if (S_ISDIR (statbuf.st_mode))
    res = op_backend->smb_context->rmdir (op_backend->smb_context, uri);
  else
    res = op_backend->smb_context->unlink (op_backend->smb_context, uri);
  errsv = errno;
  g_free (uri);

  if (res != 0)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_make_directory (GVfsBackend *backend,
		   GVfsJobMakeDirectory *job,
		   const char *filename)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  char *uri;
  int errsv, res;

  uri = create_smb_uri (op_backend->server, op_backend->share, filename);
  res = op_backend->smb_context->mkdir (op_backend->smb_context, uri, 0666);
  errsv = errno;
  g_free (uri);

  if (res != 0)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_move (GVfsBackend *backend,
	 GVfsJobMove *job,
	 const char *source,
	 const char *destination,
	 GFileCopyFlags flags,
	 GFileProgressCallback progress_callback,
	 gpointer progress_callback_data)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  char *source_uri, *dest_uri, *backup_uri;
  gboolean destination_exist, source_is_dir;
  struct stat statbuf;
  int res, errsv;
  
  source_uri = create_smb_uri (op_backend->server, op_backend->share, source);

  res = op_backend->smb_context->stat (op_backend->smb_context, source_uri, &statbuf);
  if (res == -1)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR,
			g_io_error_from_errno (errno),
			_("Error moving file: %s"),
			g_strerror (errno));
      g_free (source_uri);
      return;
    }
  else
    source_is_dir = S_ISDIR (statbuf.st_mode);

  dest_uri = create_smb_uri (op_backend->server, op_backend->share, destination);
  
  destination_exist = FALSE;
  res = op_backend->smb_context->stat (op_backend->smb_context, dest_uri, &statbuf);
  if (res == 0)
    {
      destination_exist = TRUE; /* Target file exists */

      if (flags & G_FILE_COPY_OVERWRITE)
	{
	  /* Always fail on dirs, even with overwrite */
	  if (S_ISDIR (statbuf.st_mode))
	    {
	      g_vfs_job_failed (G_VFS_JOB (job),
				G_IO_ERROR,
				G_IO_ERROR_IS_DIRECTORY,
				_("Can't move over directory"));
	      g_free (source_uri);
	      g_free (dest_uri);
	      return;
	    }
	}
      else
	{
	  g_vfs_job_failed (G_VFS_JOB (job),
			    G_IO_ERROR,
			    G_IO_ERROR_EXISTS,
			    _("Target file already exists"));
	  g_free (source_uri);
	  g_free (dest_uri);
	  return;
	}
    }

  if (flags & G_FILE_COPY_BACKUP && destination_exist)
    {
      backup_uri = g_strconcat (dest_uri, "~", NULL);
      res = op_backend->smb_context->rename (op_backend->smb_context, dest_uri,
					     op_backend->smb_context, backup_uri);
      if (res == -1)
	{
	  g_vfs_job_failed (G_VFS_JOB (job),
			    G_IO_ERROR,
			    G_IO_ERROR_CANT_CREATE_BACKUP,
			    _("Backup file creation failed"));
	  g_free (source_uri);
	  g_free (dest_uri);
	  g_free (backup_uri);
	  return;
	}
      g_free (backup_uri);
      destination_exist = FALSE; /* It did, but no more */
    }

  if (source_is_dir && destination_exist && (flags & G_FILE_COPY_OVERWRITE))
    {
      /* Source is a dir, destination exists (and is not a dir, because that would have failed
	 earlier), and we're overwriting. Manually remove the target so we can do the rename. */
      res = op_backend->smb_context->unlink (op_backend->smb_context, dest_uri);
      errsv = errno;
      if (res == -1)
	{
	  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			    g_io_error_from_errno (errsv),
			    _("Error removing target file: %s"),
			    g_strerror (errsv));
	  g_free (source_uri);
	  g_free (dest_uri);
	  return;
	}
    }

  
  g_print ("rename %s to %s\n", source_uri, dest_uri);
  res = op_backend->smb_context->rename (op_backend->smb_context, source_uri,
					 op_backend->smb_context, dest_uri);
  errsv = errno;
  g_print ("rename errno: %d\n", errsv);
  g_free (source_uri);
  g_free (dest_uri);

  /* Catch moves across device boundaries */
  if (res != 0)
    {
      if (errsv == EXDEV ||
	  /* Unfortunately libsmbclient doesn't correctly return EXDEV, but falls back
	     to EINVAL, so we try to guess when this happens: */
	  (errsv == EINVAL && source_is_dir))
	g_vfs_job_failed (G_VFS_JOB (job), 
			  G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
			  _("Can't recursively move directory"));
      else
	g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
    }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));
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
  backend_class->create = do_create;
  backend_class->append_to = do_append_to;
  backend_class->replace = do_replace;
  backend_class->write = do_write;
  backend_class->seek_on_write = do_seek_on_write;
  backend_class->close_write = do_close_write;
  backend_class->get_info = do_get_info;
  backend_class->get_fs_info = do_get_fs_info;
  backend_class->enumerate = do_enumerate;
  backend_class->set_display_name = do_set_display_name;
  backend_class->delete = do_delete;
  backend_class->make_directory = do_make_directory;
  backend_class->move = do_move;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
}
