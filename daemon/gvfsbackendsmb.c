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
  if (smb_backend->server != NULL)
    return 1;

  smb_backend->server_name = g_strdup (server_name);
  smb_backend->share_name = g_strdup (share_name);
  smb_backend->domain = g_strdup (domain);
  smb_backend->username = g_strdup (username);
  smb_backend->server = new;

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
  if (smb_backend->server == server)
    {
      g_free (smb_backend->server_name);
      smb_backend->server_name = NULL;
      g_free (smb_backend->share_name);
      smb_backend->share_name = NULL;
      g_free (smb_backend->domain);
      smb_backend->domain = NULL;
      g_free (smb_backend->username);
      smb_backend->username = NULL;
      smb_backend->server = NULL;
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
  if (smb_backend->server != NULL &&
      strcmp (smb_backend->server_name, server_name) == 0 &&
      strcmp (smb_backend->share_name, server_name) == 0 &&
      strcmp (smb_backend->domain, domain) == 0 &&
      strcmp (smb_backend->username, username) == 0)
    return smb_backend->server;

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
  if (smb_backend->server)
    remove_cached_server(context, smb_backend->server);
  
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

static char *
create_smb_uri (const char *server,
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
  return g_string_free (uri, FALSE);
}


GVfsBackendSmb *
g_vfs_backend_smb_new (const char *server,
		       const char *share)
{
  GVfsBackendSmb *backend;
  char *obj_path, *bus_name;
  SMBCCTX *smb_context;
  int res;
  char *uri;
  struct stat st;

  g_assert (smb_backend == NULL);

  smb_context = smbc_new_context ();
  if (smb_context == NULL)
    return NULL;

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
      g_print ("init context failed\n");
      smbc_free_context (smb_context, FALSE);
      return NULL;
    }
  
  obj_path = g_strdup_printf (G_VFS_DBUS_MOUNTPOINT_PATH"smbshare/h_%s/f_%s", server, share);
  bus_name = g_strdup_printf (G_VFS_DBUS_MOUNTPOINT_NAME"smbshare.h_%s.f_%s", server, share);
  
  backend = g_object_new (G_TYPE_VFS_BACKEND_SMB,
			  "object-path", obj_path,
			  "bus-name", bus_name,
			  NULL);
  smb_backend = backend;
  backend->smb_context = smb_context;
  
  uri = create_smb_uri (server, share, NULL);
  res = smb_context->stat (smb_context, uri, &st);
  g_free (uri);
  if (res != 0)
    {
      g_object_unref (backend);
      return NULL;
    }
  
  return backend;
}

static gboolean 
open_idle_cb (gpointer data)
{
  GVfsJobOpenForRead *job = data;
  int fd;

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_VFS_ERROR,
			G_VFS_ERROR_CANCELLED,
			_("Operation was cancelled"));
      return FALSE;
    }
  
  fd = g_open (job->filename, O_RDONLY);
  if (fd == -1)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_FILE_ERROR,
			g_file_error_from_errno (errno),
			"Error opening file %s: %s",
			job->filename, g_strerror (errno));
    }
  else
    {
      g_vfs_job_open_for_read_set_can_seek (job, TRUE);
      g_vfs_job_open_for_read_set_handle (job, GINT_TO_POINTER (fd));
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  return FALSE;
}

static void
open_read_cancelled_cb (GVfsJob *job, gpointer data)
{
  guint tag = GPOINTER_TO_INT (data);

  g_print ("open_read_cancelled_cb\n");
  
  if (g_source_remove (tag))
    g_vfs_job_failed (job, G_VFS_ERROR,
		      G_VFS_ERROR_CANCELLED,
		      _("Operation was cancelled"));
}

static gboolean 
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  char *filename)
{
  GError *error;

  g_print ("open_for_read (%s)\n", filename);
  
  if (strcmp (filename, "/fail") == 0)
    {
      error = g_error_new (G_FILE_ERROR, G_FILE_ERROR_IO, "Smb error");
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      return TRUE;
    }
  else
    {
      guint tag = g_timeout_add (0, open_idle_cb, job);
      g_signal_connect (job, "cancelled", (GCallback)open_read_cancelled_cb, GINT_TO_POINTER (tag));
      return TRUE;
    }
}

static gboolean 
read_idle_cb (gpointer data)
{
  GVfsJobRead *job = data;
  int fd;
  ssize_t res;

  fd = GPOINTER_TO_INT (job->handle);

  res = read (fd, job->buffer, job->bytes_requested);

  if (res == -1)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_FILE_ERROR,
			g_file_error_from_errno (errno),
			"Error reading from file: %s",
			g_strerror (errno));
    }
  else
    {
      g_vfs_job_read_set_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  
  return FALSE;
}

static void
read_cancelled_cb (GVfsJob *job, gpointer data)
{
  guint tag = GPOINTER_TO_INT (job->backend_data);

  g_source_remove (tag);
  g_vfs_job_failed (job, G_VFS_ERROR,
		    G_VFS_ERROR_CANCELLED,
		    _("Operation was cancelled"));
}

static gboolean
do_read (GVfsBackend *backend,
	 GVfsJobRead *job,
	 GVfsBackendHandle handle,
	 char *buffer,
	 gsize bytes_requested)
{
  guint tag;

  g_print ("read (%d)\n", bytes_requested);

  tag = g_timeout_add (0, read_idle_cb, job);
  G_VFS_JOB (job)->backend_data = GINT_TO_POINTER (tag);
  g_signal_connect (job, "cancelled", (GCallback)read_cancelled_cb, NULL);
  
  return TRUE;
}

static gboolean
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  int whence;
  int fd;
  off_t final_offset;

  g_print ("seek_on_read (%d, %d)\n", (int)offset, type);

  switch (type)
    {
    default:
    case G_SEEK_SET:
      whence = SEEK_SET;
      break;
    case G_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case G_SEEK_END:
      whence = SEEK_END;
      break;
    }
      
  
  fd = GPOINTER_TO_INT (handle);

  final_offset = lseek (fd, offset, whence);
  
  if (final_offset == (off_t)-1)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_FILE_ERROR,
			g_file_error_from_errno (errno),
			"Error seeking in file: %s",
			g_strerror (errno));
    }
  else
    {
      g_vfs_job_seek_read_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

  return TRUE;
}

static gboolean
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  int fd;

  g_print ("close ()\n");

  fd = GPOINTER_TO_INT (handle);
  close(fd);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  return TRUE;
}

static gboolean
do_get_info (GVfsBackend *backend,
	     GVfsJobGetInfo *job,
	     char *filename,
	     GFileInfoRequestFlags requested,
	     const char *attributes,
	     gboolean follow_symlinks)
{
  GFile *file;
  GFileInfo *info;
  GError *error;

  file = g_file_local_new (filename);

  error = NULL;
  info = g_file_get_info (file, requested, attributes, follow_symlinks,
			  NULL, &error);

  if (info)
    {
      g_vfs_job_get_info_set_info (job, requested, info);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);

  g_object_unref (info);
  g_object_unref (file);
  
  return TRUE;
}

static gboolean
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      char *filename,
	      GFileInfoRequestFlags requested,
	      const char *attributes,
	      gboolean follow_symlinks)
{
  GFileInfo *info1, *info2;;
  GList *l;
  
  g_vfs_job_enumerate_set_result (job, requested);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  info1 = g_file_info_new ();
  info2 = g_file_info_new ();
  g_file_info_set_name (info1, "file1");
  g_file_info_set_file_type (info1, G_FILE_TYPE_REGULAR);
  g_file_info_set_name (info2, "file2");
  g_file_info_set_file_type (info2, G_FILE_TYPE_REGULAR);
  
  l = NULL;
  l = g_list_append (l, info1);
  l = g_list_append (l, info2);

  g_vfs_job_enumerate_add_info (job, l);

  g_list_free (l);
  g_object_unref (info1);
  g_object_unref (info2);

  g_vfs_job_enumerate_done (job);
  
  return TRUE;
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
