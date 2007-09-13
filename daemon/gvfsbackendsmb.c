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
  if (smb_backend->cached_server != NULL &&
      strcmp (smb_backend->cached_server_name, server_name) == 0 &&
      strcmp (smb_backend->cached_share_name, server_name) == 0 &&
      strcmp (smb_backend->cached_domain, domain) == 0 &&
      strcmp (smb_backend->cached_username, username) == 0)
    return smb_backend->cached_server;

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

  backend->server = g_strdup (server);
  backend->share = g_strdup (share);

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

static void 
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  char *filename)
{
}

static void
do_read (GVfsBackend *backend,
	 GVfsJobRead *job,
	 GVfsBackendHandle handle,
	 char *buffer,
	 gsize bytes_requested)
{
}

static void
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
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
  
  uri = create_smb_uri (op_backend->server, op_backend->share, NULL);
  res = op_backend->smb_context->stat (op_backend->smb_context, uri, &st);
  g_free (uri);

  info = g_file_info_new ();

  if (res == 0)
    {
      g_file_info_set_from_stat (info, requested, &st);
    }
  
  if (info)
    {
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
