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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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

#include "gvfsbackendsmb.h"
#include "gvfsbackendsmbprivate.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobtruncate.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobmove.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsdaemonutils.h"
#include "gvfsutils.h"
#include "gvfskeyring.h"

#include <libsmbclient.h>


struct _GVfsBackendSmb
{
  GVfsBackend parent_instance;

  char *server;
  char *share;
  char *user;
  char *domain;
  char *path;
  char *default_workgroup;
  int port;
  
  SMBCCTX *smb_context;

  char *last_user;
  char *last_domain;
  char *last_password;
  
  GMountSource *mount_source; /* Only used/set during mount */
  int mount_try;
  gboolean mount_try_again;
  gboolean mount_cancelled;
	
  gboolean password_in_keyring;
  GPasswordSave password_save;
};


G_DEFINE_TYPE (GVfsBackendSmb, g_vfs_backend_smb, G_VFS_TYPE_BACKEND)

static void set_info_from_stat (GVfsBackendSmb *backend,
				GFileInfo *info,
				struct stat *statbuf,
				const char *basename,
				GFileAttributeMatcher *matcher);


static void
g_vfs_backend_smb_finalize (GObject *object)
{
  GVfsBackendSmb *backend;

  backend = G_VFS_BACKEND_SMB (object);

  g_free (backend->share);
  g_free (backend->server);
  g_free (backend->user);
  g_free (backend->domain);
  g_free (backend->path);
  g_free (backend->default_workgroup);
  
  if (G_OBJECT_CLASS (g_vfs_backend_smb_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_smb_parent_class)->finalize) (object);
}

static void
g_vfs_backend_smb_init (GVfsBackendSmb *backend)
{
  char *workgroup;
  GSettings *settings;

  /* Get default workgroup name */
  settings = g_settings_new ("org.gnome.system.smb");

  workgroup = g_settings_get_string (settings, "workgroup");
  if (workgroup && workgroup[0])
    backend->default_workgroup = workgroup;
  else
    g_free (workgroup);

  g_object_unref (settings);

  g_debug ("g_vfs_backend_smb_init: default workgroup = '%s'\n", backend->default_workgroup ? backend->default_workgroup : "NULL");
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
  gboolean handled, abort, anonymous = FALSE;

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
      g_debug ("auth_callback - mount_cancelled\n");
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
      /* Try again if kerberos login fails */
      backend->mount_try_again = TRUE;
      g_debug ("auth_callback - kerberos pass\n");
    }
  else if (backend->mount_try == 1 &&
           backend->user == NULL &&
           backend->domain == NULL)
    {
      /* Try again if ccache login fails */
      backend->mount_try_again = TRUE;
      g_debug ("auth_callback - ccache pass\n");
    }
  else
    {
      gboolean in_keyring = FALSE;

      g_debug ("auth_callback - normal pass\n");

      if (!backend->password_in_keyring)
        {
	  in_keyring = g_vfs_keyring_lookup_password (backend->user,
						      backend->server,
						      backend->domain,
						      "smb",
						      NULL,
						      NULL,
						      backend->port != -1 ? backend->port : 0,
						      &ask_user,
						      &ask_domain,
						      &ask_password);
	  backend->password_in_keyring = in_keyring;

	  if (in_keyring)
            g_debug ("auth_callback - reusing keyring credentials: user = '%s', domain = '%s'\n",
                     ask_user ? ask_user : "NULL",
                     ask_domain ? ask_domain : "NULL");
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
          if (backend->user == NULL && backend->domain == NULL)
	    flags |= G_ASK_PASSWORD_ANONYMOUS_SUPPORTED;

          g_debug ("auth_callback - asking for password...\n");

          if (backend->user)
            {
              /* Translators: First %s is a share name, second is a server name */
              message = g_strdup_printf (_("Authentication Required\nEnter password for share “%s” on “%s”:"),
                                         share_name, server_name);
            }
          else
            {
              /* Translators: First %s is a share name, second is a server name */
              message = g_strdup_printf (_("Authentication Required\nEnter user and password for share “%s” on “%s”:"),
                                         share_name, server_name);
            }

	  handled = g_mount_source_ask_password (backend->mount_source,
						 message,
						 username_out,
						 domain_out,
						 flags,
						 &abort,
						 &ask_password,
						 &ask_user,
						 &ask_domain,
						 &anonymous,
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

      smbc_setOptionNoAutoAnonymousLogin (backend->smb_context,
                                          !anonymous);

      if (anonymous)
        {
          backend->password_save = FALSE;
          g_debug ("auth_callback - anonymous enabled\n");
        }
      else
        {
          strncpy (password_out, ask_password, pwmaxlen);
          if (ask_user && *ask_user)
            strncpy (username_out, ask_user, unmaxlen);
          if (ask_domain && *ask_domain)
            strncpy (domain_out, ask_domain, domainmaxlen);
        }

    out:
      g_free (ask_password);
      g_free (ask_user);
      g_free (ask_domain);
    }

  backend->last_user = g_strdup (username_out);
  backend->last_domain = g_strdup (domain_out);
  backend->last_password = g_strdup (password_out);
  g_debug ("auth_callback - out: last_user = '%s', last_domain = '%s'\n",
           backend->last_user, backend->last_domain);
}

#define SUB_DELIM_CHARS  "!$&'()*+,;="

static GString *
create_smb_uri_string (const char *server,
		       int port,
		       const char *share,
		       const char *path)
{
  GString *uri;

  uri = g_string_new ("smb://");
  if (server == NULL)
    return uri;

  /* IPv6 server includes brackets in GMountSpec, smbclient doesn't */
  if (gvfs_is_ipv6 (server))
    {
      g_string_append_uri_escaped (uri, server + 1, NULL, FALSE);
      g_string_truncate (uri, uri->len - 3);
    }
  else
    g_string_append_uri_escaped (uri, server, NULL, FALSE);

  if (port != -1)
    g_string_append_printf (uri, ":%d", port);
  g_string_append_c (uri, '/');

  if (share != NULL)
    g_string_append_uri_escaped (uri, share, NULL, FALSE);

  if (path != NULL)
    {
      if (*path != '/')
	g_string_append_c (uri, '/');
      g_string_append_uri_escaped (uri, path, SUB_DELIM_CHARS ":@/", FALSE);
    }

  while (uri->len > 0 &&
	 uri->str[uri->len - 1] == '/')
    g_string_erase (uri, uri->len - 1, 1);
  
  return uri;
}

char *
create_smb_uri (const char *server,
		int port,
		const char *share,
		const char *path)
{
  GString *uri;
  uri = create_smb_uri_string (server, port, share, path);
  return g_string_free (uri, FALSE);
}

static void
set_default_location_to_topmost_dir (GVfsBackend  *backend,
                                     const char   *mount_path)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat st;
  char *uri;
  int res;
  char *last_good_path, *new_path;
  smbc_stat_fn smbc_stat;

  smbc_stat = smbc_getFunctionStat (op_backend->smb_context);
  last_good_path = g_strdup (mount_path);

  while (!g_str_equal (last_good_path, "/"))
    {
      new_path = g_path_get_dirname (last_good_path);
      uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, new_path);
      res = smbc_stat (op_backend->smb_context, uri, &st);
      g_free (uri);
      if (res != 0)
        {
          g_free (new_path);
          break;
        }
      g_free (last_good_path);
      last_good_path = new_path;
    }

  g_vfs_backend_set_default_location (backend, last_good_path);
  g_free (last_good_path);
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
  const char *debug;
  int debug_val;
  gchar *port_str;
  GMountSpec *smb_mount_spec;
  smbc_stat_fn smbc_stat;
  int errsv;

  smb_context = smbc_new_context ();
  if (smb_context == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Internal Error (%s)"), "Failed to allocate smb context");
      return;
    }
  smbc_setOptionUserData (smb_context, backend);

  debug = g_getenv ("GVFS_SMB_DEBUG");
  if (debug)
    debug_val = atoi (debug);
  else
    debug_val = 0;

  smbc_setDebug (smb_context, debug_val);
  smbc_setFunctionAuthDataWithContext (smb_context, auth_callback);

  if (op_backend->default_workgroup != NULL)
    smbc_setWorkgroup (smb_context, op_backend->default_workgroup);

  smbc_setOptionUseKerberos (smb_context, 1);
  smbc_setOptionFallbackAfterKerberos (smb_context,
                                       op_backend->user != NULL);
  smbc_setOptionNoAutoAnonymousLogin (smb_context, TRUE);
  smbc_setOptionUseCCache (smb_context, 1);

  if (!smbc_init_context (smb_context))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Internal Error (%s)"), "Failed to initialize smb context");
      smbc_free_context (smb_context, FALSE);
      return;
    }

  op_backend->smb_context = smb_context;

  /* Set the mountspec according to original uri, no matter whether user changes
     credentials during mount loop. Nautilus and other gio clients depend
     on correct mountspec, setting it to real (different) credentials would 
     lead to G_IO_ERROR_NOT_MOUNTED errors
   */

  /* Translators: This is "<sharename> on <servername>" and is used as name for an SMB share */
  display_name = g_strdup_printf (_("%s on %s"), op_backend->share, op_backend->server);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  g_vfs_backend_set_icon_name (backend, "folder-remote");
  g_vfs_backend_set_symbolic_icon_name (backend, "folder-remote-symbolic");

  smb_mount_spec = g_mount_spec_new ("smb-share");
  g_mount_spec_set (smb_mount_spec, "share", op_backend->share);
  g_mount_spec_set (smb_mount_spec, "server", op_backend->server);
  if (op_backend->user)
    g_mount_spec_set (smb_mount_spec, "user", op_backend->user);
  if (op_backend->domain)
    g_mount_spec_set (smb_mount_spec, "domain", op_backend->domain);
  if (op_backend->port != -1)
    {
      port_str = g_strdup_printf ("%d", op_backend->port);
      g_mount_spec_set (smb_mount_spec, "port", port_str);
      g_free (port_str);
    }

  g_vfs_backend_set_mount_spec (backend, smb_mount_spec);
  g_mount_spec_unref (smb_mount_spec);

  /* FIXME: we're stat()-ing user-specified path here, not the root. Ideally we
            would like to fallback to root when first mount attempt fails, though
            it would be tough to actually say if it was an authentication failure
            or the particular path problem. */
  uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, op_backend->path);
  g_debug ("do_mount - URI = %s\n", uri);

  /*  Samba mount loop  */
  op_backend->mount_source = mount_source;
  op_backend->mount_try = 0;
  op_backend->password_save = G_PASSWORD_SAVE_NEVER;

  errsv = 0;

  /* If user is not specified, first and second iteration is kerberos resp.
   * ccache only (this is necessary in order to prevent redundant password
   * prompts). Consequently, credentials from keyring are tried if available.
   * Finally, user is prompted over GMountOperation if available. Anonymous is
   * tried only if explicitely requested over GMountOperation.
   */
  do
    {
      /* The mount_try_again variable is here to avoid livelock in cases when
       * EPERM is returned immediately without calling the auth_callback
       * function. See: https://gitlab.gnome.org/GNOME/gvfs/-/issues/703.
       */
      op_backend->mount_try_again = FALSE;
      op_backend->mount_cancelled = FALSE;

      g_debug ("do_mount - try #%d \n", op_backend->mount_try);

      smbc_stat = smbc_getFunctionStat (smb_context);
      res = smbc_stat (smb_context, uri, &st);

      errsv = errno;
      g_debug ("do_mount - [%s; %d] res = %d, cancelled = %d, errno = [%d] '%s' \n",
             uri, op_backend->mount_try, res, op_backend->mount_cancelled,
             errsv, g_strerror (errsv));

      if (res == 0)
        break;

      if (errsv == EINVAL && op_backend->mount_try <= 1 && op_backend->user == NULL)
        {
          /* EINVAL is "expected" when kerberos/ccache is misconfigured, see:
           * https://gitlab.gnome.org/GNOME/gvfs/-/issues/611
           */
        }
      else if (op_backend->mount_cancelled || (errsv != EACCES && errsv != EPERM))
        {
          g_debug ("do_mount - (errno != EPERM && errno != EACCES), cancelled = %d, breaking\n", op_backend->mount_cancelled);
          break;
        }

      /* The first round is Kerberos-only.  Only if this fails do we enable
       * NTLMSSP fallback.
       */
      if (op_backend->mount_try == 0 &&
          op_backend->user == NULL)
        {
          g_debug ("do_mount - enabling NTLMSSP fallback\n");
          smbc_setOptionFallbackAfterKerberos (op_backend->smb_context, 1);
        }

      op_backend->mount_try ++;
    }
  while (op_backend->mount_try_again);
  
  g_free (uri);
  
  op_backend->mount_source = NULL;

  if (res != 0)
    {
      if (op_backend->mount_cancelled) 
        g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED,
			  _("Password dialog cancelled"));
      else
        g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_FAILED,
			  /* translators: We tried to mount a windows (samba) share, but failed */
			  _("Failed to mount Windows share: %s"), g_strerror (errsv));

      return;
    }

  /* Mount was successful */
  g_debug ("do_mount - login successful\n");

  set_default_location_to_topmost_dir (backend, op_backend->path);
  g_vfs_keyring_save_password (op_backend->last_user,
			       op_backend->server,
			       op_backend->last_domain,
			       "smb",
			       NULL,
			       NULL,
			       op_backend->port != -1 ? op_backend->port : 0,
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
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  const char *server, *share, *user, *domain, *path, *port;
  int port_num;

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
  port = g_mount_spec_get (mount_spec, "port");
  path = mount_spec->mount_prefix;
  
  op_backend->server = g_strdup (server);
  op_backend->share = g_strdup (share);
  op_backend->user = g_strdup (user);
  op_backend->domain = g_strdup (domain);
  op_backend->path = g_strdup (path);

  if (port && (port_num = atoi (port)))
      op_backend->port = port_num;
  else
      op_backend->port = -1;
  
  return FALSE;
}

static void
do_unmount (GVfsBackend *backend,
	    GVfsJobUnmount *job,
	    GMountUnmountFlags flags,
	    GMountSource *mount_source)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  int res;

  if (op_backend->smb_context == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Internal Error (%s)"), "SMB context has not been initialized");
      return;
    }

  /* shutdown_ctx = TRUE, "all connections and files will be closed even if they are busy" */
  res = smbc_free_context (op_backend->smb_context, TRUE);
  op_backend->smb_context = NULL;
  if (res != 0)
    {
      g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
      return;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static int
fixup_open_errno (int err)
{
  /* samba has a bug (#6228) where it doesn't set errno if path resolving failed */
  if (err == 0)
    err = ENOTDIR;
  return err;
}

static void 
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  const char *filename)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  char *uri;
  SMBCFILE *file;
  struct stat st;
  smbc_open_fn smbc_open;
  smbc_stat_fn smbc_stat;
  int res;
  int olderr;


  uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);
  smbc_open = smbc_getFunctionOpen (op_backend->smb_context);
  errno = 0;
  file = smbc_open (op_backend->smb_context, uri, O_RDONLY, 0);

  if (file == NULL)
    {
      olderr = fixup_open_errno (errno);
      
      smbc_stat = smbc_getFunctionStat (op_backend->smb_context);
      res = smbc_stat (op_backend->smb_context, uri, &st);
      if ((res == 0) && (S_ISDIR (st.st_mode)))
            g_vfs_job_failed (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                             _("Can’t open directory"));
      else
        g_vfs_job_failed_from_errno (G_VFS_JOB (job), olderr);
  }
  else
    {
      
      g_vfs_job_open_for_read_set_can_seek (job, TRUE);
      g_vfs_job_open_for_read_set_handle (job, file);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  g_free (uri);
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
  smbc_read_fn smbc_read;

  smbc_read = smbc_getFunctionRead (op_backend->smb_context);
  res = smbc_read (op_backend->smb_context, (SMBCFILE *)handle, buffer, bytes_requested);

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
  smbc_lseek_fn smbc_lseek;

  if ((whence = gvfs_seek_type_to_lseek (type)) == -1)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Unsupported seek type"));
      return;
    }

  smbc_lseek = smbc_getFunctionLseek (op_backend->smb_context);
  res = smbc_lseek (op_backend->smb_context, (SMBCFILE *)handle, offset, whence);

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
do_query_info_on_read (GVfsBackend *backend,
		       GVfsJobQueryInfoRead *job,
		       GVfsBackendHandle handle,
		       GFileInfo *info,
		       GFileAttributeMatcher *matcher)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat st = {0};
  int res, saved_errno;
  smbc_fstat_fn smbc_fstat;

  smbc_fstat = smbc_getFunctionFstat (op_backend->smb_context);
  res = smbc_fstat (op_backend->smb_context, (SMBCFILE *)handle, &st);
  saved_errno = errno;

  if (res == 0)
    {
      set_info_from_stat (op_backend, info, &st, NULL, matcher);
      
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), saved_errno);

}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  ssize_t res;
  smbc_close_fn smbc_close;

  smbc_close = smbc_getFunctionClose (op_backend->smb_context);
  res = smbc_close (op_backend->smb_context, (SMBCFILE *)handle);
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
  GVfsJobOpenForWriteMode mode;
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
open_for_write (GVfsBackend *backend,
                GVfsJobOpenForWrite *job,
                const char *filename,
                GFileCreateFlags flags,
                int open_flags)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  char *uri;
  SMBCFILE *file;
  SmbWriteHandle *handle;
  smbc_open_fn smbc_open;
  int errsv;
  off_t initial_offset = 0;

  uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);
  smbc_open = smbc_getFunctionOpen (op_backend->smb_context);
  errno = 0;
  file = smbc_open (op_backend->smb_context, uri, open_flags, 0666);
  g_free (uri);

  if (file == NULL)
    {
      errsv = fixup_open_errno (errno);

      /* We guarantee EEXIST on create on existing dir */
      if (job->mode == OPEN_FOR_WRITE_CREATE && errsv == EISDIR)
	errsv = EEXIST;
      g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
      return;
    }

  if (job->mode == OPEN_FOR_WRITE_APPEND)
    {
      smbc_lseek_fn smbc_lseek;

      smbc_lseek = smbc_getFunctionLseek (op_backend->smb_context);
      initial_offset = smbc_lseek (op_backend->smb_context, file,
						       0, SEEK_CUR);
      if (initial_offset == (off_t) -1)
        {
          g_vfs_job_failed_from_errno (G_VFS_JOB (job),
                                       fixup_open_errno (errno));
          return;
        }
    }

  handle = g_new0 (SmbWriteHandle, 1);
  handle->file = file;
  handle->mode = job->mode;

  g_vfs_job_open_for_write_set_initial_offset (job, initial_offset);

  /* The O_APPEND flag is not properly supported by the libsmbclient library
   * when seeking. See:
   * https://github.com/samba-team/samba/blob/e4e3f05/source3/libsmb/libsmb_file.c#L162-L183
   */
  g_vfs_job_open_for_write_set_can_seek (job,
                                         job->mode != OPEN_FOR_WRITE_APPEND);
  g_vfs_job_open_for_write_set_can_truncate (job, TRUE);
  g_vfs_job_open_for_write_set_handle (job, handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_create (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
  open_for_write (backend, job, filename, flags, O_CREAT|O_RDWR|O_EXCL);
}

static void
do_append_to (GVfsBackend *backend,
              GVfsJobOpenForWrite *job,
              const char *filename,
              GFileCreateFlags flags)
{
  open_for_write (backend, job, filename, flags, O_CREAT|O_RDWR|O_APPEND);
}

static void
do_edit (GVfsBackend *backend,
         GVfsJobOpenForWrite *job,
         const char *filename,
         GFileCreateFlags flags)
{
  open_for_write (backend, job, filename, flags, O_CREAT|O_RDWR);
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
  smbc_open_fn smbc_open;

  dir_uri = get_dir_from_uri (uri);
 
  do {
    gvfs_randomize_string (filename + 4, 4);
    tmp_uri = g_strconcat (dir_uri, filename, NULL);

    smbc_open = smbc_getFunctionOpen (backend->smb_context);
    errno = 0;
    file = smbc_open (backend->smb_context, tmp_uri,
                      O_CREAT|O_RDWR|O_EXCL, 0666);
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
  smbc_open_fn smbc_open;
  smbc_read_fn smbc_read;
  smbc_write_fn smbc_write;
  smbc_close_fn smbc_close;
  

  from_file = NULL;
  to_file = NULL;

  succeeded = FALSE;

  smbc_open = smbc_getFunctionOpen (backend->smb_context);
  smbc_read = smbc_getFunctionRead (backend->smb_context);
  smbc_write = smbc_getFunctionWrite (backend->smb_context);
  smbc_close = smbc_getFunctionClose (backend->smb_context);

  from_file = smbc_open (backend->smb_context, from_uri,
			 O_RDONLY, 0666);
  if (from_file == NULL || g_vfs_job_is_cancelled (job))
    goto out;
  
  to_file = smbc_open (backend->smb_context, to_uri,
		       O_CREAT|O_WRONLY|O_TRUNC, 0666);
  
  if (from_file == NULL || g_vfs_job_is_cancelled (job))
    goto out;

  while (1)
    {
      
      res = smbc_read (backend->smb_context, from_file,
					buffer, sizeof(buffer));
      if (res < 0 || g_vfs_job_is_cancelled (job))
	goto out;
      if (res == 0)
	break; /* Succeeded */

      buffer_size = res;
      p = buffer;
      while (buffer_size > 0)
	{
	  res = smbc_write (backend->smb_context, to_file,
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
	  smbc_close (backend->smb_context, to_file);
  if (from_file)
	  smbc_close (backend->smb_context, from_file);
  return succeeded;
}

static char *
create_etag (struct stat *statbuf)
{
  return g_strdup_printf ("%lu", (long unsigned int)statbuf->st_mtime);
}

static void
do_replace (GVfsBackend *backend,
	    GVfsJobOpenForWrite *job,
	    const char *filename,
	    const char *etag,
	    gboolean make_backup,
	    GFileCreateFlags flags)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat original_stat;
  int res;
  char *uri, *tmp_uri, *backup_uri, *current_etag;
  SMBCFILE *file;
  GError *error = NULL;
  SmbWriteHandle *handle;
  smbc_open_fn smbc_open;
  smbc_stat_fn smbc_stat;

  uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);
  tmp_uri = NULL;
  if (make_backup)
    backup_uri = g_strconcat (uri, "~", NULL);
  else
    backup_uri = NULL;

  smbc_open = smbc_getFunctionOpen (op_backend->smb_context);
  smbc_stat = smbc_getFunctionStat (op_backend->smb_context);
  
  errno = 0;
  file = smbc_open (op_backend->smb_context, uri,
                    O_CREAT|O_RDWR|O_EXCL, 0);
  if (file == NULL && errno != EEXIST)
    {
      int errsv = fixup_open_errno (errno);

      g_set_error_literal (&error, G_IO_ERROR,
			   g_io_error_from_errno (errsv),
			   g_strerror (errsv));
      goto error;
    }
  else if (file == NULL && errno == EEXIST)
    {
      if (etag != NULL)
	{
	  res = smbc_stat (op_backend->smb_context, uri, &original_stat);
	  
	  if (res == 0)
	    {
	      current_etag = create_etag (&original_stat);
	      if (strcmp (etag, current_etag) != 0)
		{
		  g_free (current_etag);
		  g_set_error_literal (&error,
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
		    g_set_error_literal (&error,
					 G_IO_ERROR,
					 G_IO_ERROR_CANCELLED,
					 _("Operation was cancelled"));
		  else
		    g_set_error_literal (&error,
					 G_IO_ERROR,
					 G_IO_ERROR_CANT_CREATE_BACKUP,
					 _("Backup file creation failed"));
		  goto error;
		}
	      g_free (backup_uri);
	      backup_uri = NULL;
	    }
	  
	  errno = 0;
	  file = smbc_open (op_backend->smb_context, uri,
                            O_CREAT|O_RDWR|O_TRUNC, 0);
	  if (file == NULL)
	    {
              int errsv = fixup_open_errno (errno);

	      g_set_error_literal (&error, G_IO_ERROR,
				   g_io_error_from_errno (errsv),
				   g_strerror (errsv));
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
  handle->mode = job->mode;
  
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (job, TRUE);
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
  smbc_write_fn smbc_write;

  smbc_write = smbc_getFunctionWrite (op_backend->smb_context);
  res = smbc_write (op_backend->smb_context, handle->file,
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
  smbc_lseek_fn smbc_lseek;

  if ((whence = gvfs_seek_type_to_lseek (type)) == -1)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Unsupported seek type"));
      return;
    }

  smbc_lseek = smbc_getFunctionLseek (op_backend->smb_context);
  res = smbc_lseek (op_backend->smb_context, handle->file, offset, whence);

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
do_truncate (GVfsBackend *backend,
             GVfsJobTruncate *job,
             GVfsBackendHandle _handle,
	     goffset size)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  SmbWriteHandle *handle = _handle;
  smbc_ftruncate_fn smbc_ftruncate;

  smbc_ftruncate = smbc_getFunctionFtruncate (op_backend->smb_context);
  if (smbc_ftruncate (op_backend->smb_context, handle->file, size) == -1)
    {
      g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
      return;
    }

  if (handle->mode == OPEN_FOR_WRITE_APPEND)
    {
      smbc_lseek_fn smbc_lseek;
      off_t res;

      smbc_lseek = smbc_getFunctionLseek (op_backend->smb_context);
      res = smbc_lseek (op_backend->smb_context, handle->file, size, SEEK_SET);
      if (res == (off_t)-1)
        {
          g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
          return;
        }
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_query_info_on_write (GVfsBackend *backend,
			GVfsJobQueryInfoWrite *job,
			GVfsBackendHandle _handle,
			GFileInfo *info,
			GFileAttributeMatcher *matcher)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat st = {0};
  SmbWriteHandle *handle = _handle;
  int res, saved_errno;
  smbc_fstat_fn smbc_fstat;

  smbc_fstat = smbc_getFunctionFstat (op_backend->smb_context);
  res = smbc_fstat (op_backend->smb_context, handle->file, &st);
  saved_errno = errno;

  if (res == 0)
    {
      set_info_from_stat (op_backend, info, &st, NULL, matcher);
      
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), saved_errno);

}

static void
do_close_write (GVfsBackend *backend,
		GVfsJobCloseWrite *job,
		GVfsBackendHandle _handle)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  SmbWriteHandle *handle = _handle;
  struct stat stat_at_close;
  int stat_res, errsv;
  ssize_t res;
  smbc_fstat_fn smbc_fstat;
  smbc_close_fn smbc_close;
  smbc_unlink_fn smbc_unlink;
  smbc_rename_fn smbc_rename;

  smbc_fstat = smbc_getFunctionFstat (op_backend->smb_context);
  smbc_close = smbc_getFunctionClose (op_backend->smb_context);
  smbc_unlink = smbc_getFunctionUnlink (op_backend->smb_context);
  smbc_rename = smbc_getFunctionRename (op_backend->smb_context);
  
  stat_res = smbc_fstat (op_backend->smb_context, handle->file, &stat_at_close);
  
  res = smbc_close (op_backend->smb_context, handle->file);

  if (res == -1)
    {
      errsv = errno;
      if (handle->tmp_uri)
    	  smbc_unlink (op_backend->smb_context, handle->tmp_uri);
      g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
      goto out;
    }

  if (handle->tmp_uri)
    {
      if (handle->backup_uri)
	{
	  res = smbc_rename (op_backend->smb_context, handle->uri,
						 op_backend->smb_context, handle->backup_uri);
	  if (res ==  -1)
	    {
              errsv = errno;
              smbc_unlink (op_backend->smb_context, handle->tmp_uri);
	      g_vfs_job_failed (G_VFS_JOB (job),
				G_IO_ERROR, G_IO_ERROR_CANT_CREATE_BACKUP,
				_("Backup file creation failed: %s"), g_strerror (errsv));
	      goto out;
	    }
	}
      else
        {
	  res = smbc_unlink (op_backend->smb_context, handle->uri);
	  if (res ==  -1)
	    {
	      errsv = errno;
	      smbc_unlink (op_backend->smb_context, handle->tmp_uri);
	      g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
	      goto out;
	    }
	}
      
      res = smbc_rename (op_backend->smb_context, handle->tmp_uri,
					     op_backend->smb_context, handle->uri);
      if (res ==  -1)
	{
	  errsv = errno;
	  smbc_unlink (op_backend->smb_context, handle->tmp_uri);
	  g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
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
set_info_from_stat (GVfsBackendSmb *backend,
		    GFileInfo *info,
		    struct stat *statbuf,
		    const char *basename,
		    GFileAttributeMatcher *matcher)
{
  GFileType file_type;
  char *display_name;

  if (basename)
    {
      g_file_info_set_name (info, basename);
      if (*basename == '.')
	g_file_info_set_is_hidden (info, TRUE);
    }

  
  if (basename != NULL &&
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME))
    {
      if (strcmp (basename, "/") == 0)
	display_name = g_strdup_printf (_("%s on %s"), backend->share, backend->server);
      else
	display_name = g_filename_display_name (basename);
      
      if (strstr (display_name, "\357\277\275") != NULL)
        {
          char *p = display_name;
          display_name = g_strconcat (display_name, _(" (invalid encoding)"), NULL);
          g_free (p);
        }
      g_file_info_set_display_name (info, display_name);
      g_free (display_name);
    }
  
  if (basename != NULL &&
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME))
    {
      char *edit_name = g_filename_display_name (basename);
      g_file_info_set_edit_name (info, edit_name);
      g_free (edit_name);
    }

  
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
  g_file_info_set_attribute_uint64 (info,
                                    G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE,
                                    statbuf->st_blocks * G_GUINT64_CONSTANT (512));

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, statbuf->st_mtime);
#if defined (HAVE_STRUCT_STAT_ST_MTIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, statbuf->st_mtimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, statbuf->st_mtim.tv_nsec / 1000);
#else
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, 0);
#endif

  if (g_file_attribute_matcher_matches (matcher,
					G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE) ||
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE) ||
      g_file_attribute_matcher_matches (matcher,
					G_FILE_ATTRIBUTE_STANDARD_ICON) ||
      g_file_attribute_matcher_matches (matcher,
					G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON))
    {
      GIcon *icon = NULL;
      GIcon *symbolic_icon = NULL;
      char *content_type = NULL;
      gboolean uncertain_content_type = FALSE;

      if (S_ISDIR(statbuf->st_mode))
	{
	  content_type = g_strdup ("inode/directory");
	  if (basename != NULL && strcmp (basename, "/") == 0)
            {
              icon = g_themed_icon_new ("folder-remote");
              symbolic_icon = g_themed_icon_new ("folder-remote-symbolic");
            }
	  else
            {
              icon = g_content_type_get_icon (content_type);
              symbolic_icon = g_content_type_get_symbolic_icon (content_type);
            }
	}
      else if (basename != NULL)
	{
	  content_type = g_content_type_guess (basename, NULL, 0, &uncertain_content_type);
	  if (content_type)
            {
              icon = g_content_type_get_icon (content_type);
              symbolic_icon = g_content_type_get_symbolic_icon (content_type);
            }
	}
      
      if (content_type)
	{
	  if (!uncertain_content_type)
	    g_file_info_set_content_type (info, content_type);

	  g_file_info_set_attribute_string (info,
					    G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
					    content_type);
	  g_free (content_type);
	}

      if (icon == NULL)
	icon = g_themed_icon_new ("text-x-generic");
      if (symbolic_icon == NULL)
	symbolic_icon = g_themed_icon_new ("text-x-generic-symbolic");

      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      g_file_info_set_symbolic_icon (info, symbolic_icon);
      g_object_unref (symbolic_icon);
    }
  
  /* Don't trust n_link, uid, gid, etc returned from libsmb, its just made up.
     These are ok though: */

  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE, statbuf->st_dev);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE, statbuf->st_ino);

  /* If file is dos-readonly, libsmbclient doesn't set S_IWUSR, we use this to
     trigger ACCESS_WRITE = FALSE. Only set for regular files, see
     https://bugzilla.gnome.org/show_bug.cgi?id=598206   */
  if (S_ISREG (statbuf->st_mode))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, statbuf->st_mode & S_IWUSR);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

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
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *matcher)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat st = {0};
  char *uri;
  int res, saved_errno;
  char *basename;
  smbc_stat_fn smbc_stat;

  uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);
  smbc_stat = smbc_getFunctionStat (op_backend->smb_context);
  res = smbc_stat (op_backend->smb_context, uri, &st);
  saved_errno = errno;
  g_free (uri);

  /* Create dummy stat for root dir where access is denied */
  if (saved_errno == EACCES || saved_errno == EPERM)
    {
      /* Check if the file name is part of the user's mount path */
      if (g_str_equal (filename, "/") ||
          (g_str_has_prefix (op_backend->path, filename) &&
           op_backend->path[strlen (filename)] == '/'))
        {
          st.st_mode = S_IFDIR | 0500;

          res = saved_errno = 0;
        }
    }

  if (res == 0)
    {
      basename = g_path_get_basename (filename);
      set_info_from_stat (op_backend, info, &st, basename, matcher);
      g_free (basename);
      
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), saved_errno);

}

static void
do_query_fs_info (GVfsBackend *backend,
		  GVfsJobQueryFsInfo *job,
		  const char *filename,
		  GFileInfo *info,
		  GFileAttributeMatcher *attribute_matcher)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  smbc_statvfs_fn smbc_statvfs;
  struct statvfs st = {0};
  char *uri;
  int res, saved_errno;

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "cifs");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS);

  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
      g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_FILESYSTEM_FREE) ||
      g_file_attribute_matcher_matches (attribute_matcher,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_USED) ||
      g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_FILESYSTEM_READONLY))
    {
      uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);
      smbc_statvfs = smbc_getFunctionStatVFS (op_backend->smb_context);
      res = smbc_statvfs (op_backend->smb_context, uri, &st);
      saved_errno = errno;
      g_free (uri);

      if (res == 0)
        {
          /* older samba versions ( < 3.0.28) return zero values in struct statvfs */
          if (st.f_blocks > 0)
            {
              /* FIXME: inconsistent return values (libsmbclient-3.4.2)
               *       - for linux samba hosts, f_frsize is zero and f_bsize is a real block size
               *       - for some Windows hosts (XP), f_frsize and f_bsize should be multiplied to get real block size
               */
              guint64 size, free_space;
              size = st.f_bsize * st.f_blocks * ((st.f_frsize == 0) ? 1 : st.f_frsize);
              free_space = st.f_bsize * st.f_bfree * ((st.f_frsize == 0) ? 1 : st.f_frsize);
              g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, size);
              g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, free_space);
              g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USED, size - free_space);
              g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, st.f_flag & SMBC_VFS_FEATURE_RDONLY);
            }
        }
      else
        {
          g_vfs_job_failed_from_errno (G_VFS_JOB (job), saved_errno);
          return;
        }
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}


static gboolean
try_query_settable_attributes (GVfsBackend *backend,
			       GVfsJobQueryAttributes *job,
			       const char *filename)
{
  GFileAttributeInfoList *list;

  list = g_file_attribute_info_list_new ();

  /* TODO: Add all settable attributes here -- bug #559586 */
  /* TODO: xattrs support? */

  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                  G_FILE_ATTRIBUTE_TYPE_UINT64,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);

#if 0
/* FIXME: disabled; despite chmod is supported, it makes no sense on samba shares and
          libsmbclient lacks proper API to read unix file modes.
          The struct stat->st_mode member is used for special Windows attributes. */
  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_UNIX_MODE,
                                  G_FILE_ATTRIBUTE_TYPE_UINT32,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
#endif

  g_vfs_job_query_attributes_set_list (job, list);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  g_file_attribute_info_list_unref (list);
  return TRUE;
}

static void
do_set_attribute (GVfsBackend *backend,
                  GVfsJobSetAttribute *job,
                  const char *filename,
                  const char *attribute,
                  GFileAttributeType type,
                  gpointer value_p,
                  GFileQueryInfoFlags flags)
{
  GVfsBackendSmb *op_backend;
  char *uri;
  int res, errsv;
  struct timeval tbuf[2];
  smbc_utimes_fn smbc_utimes;
#if 0
  smbc_chmod_fn smbc_chmod;
#endif


  op_backend = G_VFS_BACKEND_SMB (backend);

  if (strcmp (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED) != 0
#if 0
      && strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) != 0
#endif
      )
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
      return;
    }

  uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);
  res = -1;

  if (strcmp (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED) == 0)
    {
      if (type == G_FILE_ATTRIBUTE_TYPE_UINT64)
        {
	  smbc_utimes = smbc_getFunctionUtimes (op_backend->smb_context);
	  tbuf[1].tv_sec = (*(guint64 *)value_p);  /* mtime */
	  tbuf[1].tv_usec = 0;
	  /* atime = mtime (atimes are usually disabled on desktop systems) */
	  tbuf[0].tv_sec = tbuf[1].tv_sec;
	  tbuf[0].tv_usec = 0;
	  res = smbc_utimes (op_backend->smb_context, uri, &tbuf[0]);
	}
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "%s",
                            _("Invalid attribute type (uint64 expected)"));
        }
    }
#if 0
  else
  if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0)
    {
      smbc_chmod = smbc_getFunctionChmod (op_backend->smb_context);
      res = smbc_chmod (op_backend->smb_context, uri, (*(guint32 *)value_p) & 0777);
    }
#endif    

  errsv = errno;
  g_free (uri);

  if (res != 0)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      GFileAttributeMatcher *matcher,
	      GFileQueryInfoFlags flags)
{
  GVfsBackendSmb *op_backend = G_VFS_BACKEND_SMB (backend);
  struct stat st = { 0 };
  GError *error;
  SMBCFILE *dir;
  GFileInfo *info;
  GString *uri;
  smbc_opendir_fn smbc_opendir;
  smbc_closedir_fn smbc_closedir;
#ifndef HAVE_SMBC_READDIRPLUS2
  int res;
  char dirents[1024*4];
  struct smbc_dirent *dirp;
  int uri_start_len;
  smbc_getdents_fn smbc_getdents;
  smbc_stat_fn smbc_stat;
#else
  smbc_readdirplus2_fn smbc_readdirplus2;
  const struct libsmb_file_info *exstat;
#endif

  uri = create_smb_uri_string (op_backend->server, op_backend->port, op_backend->share, filename);
  
  smbc_opendir = smbc_getFunctionOpendir (op_backend->smb_context);
#ifndef HAVE_SMBC_READDIRPLUS2
  smbc_getdents = smbc_getFunctionGetdents (op_backend->smb_context);
  smbc_stat = smbc_getFunctionStat (op_backend->smb_context);
#else
  smbc_readdirplus2 = smbc_getFunctionReaddirPlus2 (op_backend->smb_context);
#endif
  smbc_closedir = smbc_getFunctionClosedir (op_backend->smb_context);
  
  dir = smbc_opendir (op_backend->smb_context, uri->str);

  if (dir == NULL)
    {
      int errsv = errno;

      error = NULL;
      g_set_error_literal (&error, G_IO_ERROR,
			   g_io_error_from_errno (errsv),
			   g_strerror (errsv));
      goto error;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  if (uri->str[uri->len - 1] != '/')
    g_string_append_c (uri, '/');

#ifndef HAVE_SMBC_READDIRPLUS2
  uri_start_len = uri->len;

  while (TRUE)
    {
      res = smbc_getdents (op_backend->smb_context, dir, (struct smbc_dirent *)dirents, sizeof (dirents));
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
              g_string_append_uri_escaped (uri, dirp->name, SUB_DELIM_CHARS ":@/", FALSE);

	      if (matcher == NULL ||
		  g_file_attribute_matcher_matches_only (matcher, G_FILE_ATTRIBUTE_STANDARD_NAME))
		{
		  info = g_file_info_new ();
		  g_file_info_set_name (info, dirp->name);
                  g_vfs_job_enumerate_add_info (job, info);
                  g_object_unref (info);
		}
	      else
		{
		  stat_res = smbc_stat (op_backend->smb_context,
							    uri->str, &st);
		  if (stat_res == 0)
		    {
		      info = g_file_info_new ();
		      set_info_from_stat (op_backend, info, &st, dirp->name, matcher);
                      g_vfs_job_enumerate_add_info (job, info);
                      g_object_unref (info);
		    }
		}
	    }
	  
	  dirlen = dirp->dirlen;
	  dirp = (struct smbc_dirent *) (((char *)dirp) + dirlen);
	  res -= dirlen;
	}
   }
#else
  while ((exstat = smbc_readdirplus2 (op_backend->smb_context, dir, &st)) != NULL)
    {
      if ((S_ISREG (st.st_mode) ||
           S_ISDIR (st.st_mode) ||
           S_ISLNK (st.st_mode)) &&
          g_strcmp0 (exstat->name, ".") != 0 &&
          g_strcmp0 (exstat->name, "..") != 0)
        {
          info = g_file_info_new ();
          set_info_from_stat (op_backend, info, &st, exstat->name, matcher);
          g_vfs_job_enumerate_add_info (job, info);
          g_object_unref (info);
        }

      memset (&st, 0, sizeof (struct stat));
    }
#endif

  smbc_closedir (op_backend->smb_context, dir);

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
  g_autofree char *basename = NULL;
  g_autofree char *old_name_case = NULL;
  g_autofree char *new_name_case = NULL;
  char *dirname, *new_path;
  int res, errsv;
  struct stat st;
  smbc_rename_fn smbc_rename;
  smbc_stat_fn smbc_stat;

  dirname = g_path_get_dirname (filename);
  basename = g_path_get_basename (filename);

  /* TODO: display name is in utf8, atm we assume libsmb uris
     are in utf8, but this might not be true if the user changed
     the smb.conf file. Can we check this and convert? */

  new_path = g_build_filename (dirname, display_name, NULL);
  g_free (dirname);
  
  from_uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);
  to_uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, new_path);
  
  /* If we are simply changing the case of an existing file, we don't need to
   * worry about overwriting another file.
   */
  old_name_case = g_utf8_casefold (basename, -1);
  new_name_case = g_utf8_casefold (display_name, -1);
  if (g_strcmp0 (old_name_case, new_name_case) != 0)
    {
      /* We can't rely on libsmbclient reporting EEXIST, let's always stat first.
       * https://bugzilla.gnome.org/show_bug.cgi?id=616645
       */
      smbc_stat = smbc_getFunctionStat (op_backend->smb_context);
      res = smbc_stat (op_backend->smb_context, to_uri, &st);
      if (res == 0)
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR, G_IO_ERROR_EXISTS,
                            _("Can’t rename file, filename already exists"));
          goto out;
        }
    }

  smbc_rename = smbc_getFunctionRename (op_backend->smb_context);
  res = smbc_rename (op_backend->smb_context, from_uri,
                     op_backend->smb_context, to_uri);
  errsv = errno;

  if (res != 0)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
  else
    {
      g_vfs_job_set_display_name_set_new_path (job, new_path);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

 out:
  g_free (from_uri);
  g_free (to_uri);
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
  smbc_stat_fn smbc_stat;
  smbc_rmdir_fn smbc_rmdir;
  smbc_unlink_fn smbc_unlink;


  uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);

  smbc_stat = smbc_getFunctionStat (op_backend->smb_context);
  smbc_rmdir = smbc_getFunctionRmdir (op_backend->smb_context);
  smbc_unlink = smbc_getFunctionUnlink (op_backend->smb_context);

  res = smbc_stat (op_backend->smb_context, uri, &statbuf);
  if (res == -1)
    {
      errsv = errno;

      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR,
			g_io_error_from_errno (errsv),
			_("Error deleting file: %s"),
			g_strerror (errsv));
      g_free (uri);
      return;
    }

  if (S_ISDIR (statbuf.st_mode))
    {
      res = smbc_rmdir (op_backend->smb_context, uri);

      /* We can't rely on libsmbclient reporting ENOTEMPTY, let's verify that
       * the dir has been really removed:
       * https://bugzilla.samba.org/show_bug.cgi?id=13204
       */
      if (res == 0 && smbc_stat (op_backend->smb_context, uri, &statbuf) == 0)
        {
          res = -1;
          errno = ENOTEMPTY;
        }
    }
  else
    res = smbc_unlink (op_backend->smb_context, uri);
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
  smbc_mkdir_fn smbc_mkdir;

  uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, filename);
  smbc_mkdir = smbc_getFunctionMkdir (op_backend->smb_context);
  res = smbc_mkdir (op_backend->smb_context, uri, 0666);
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
  smbc_stat_fn smbc_stat;
  smbc_rename_fn smbc_rename;
  smbc_unlink_fn smbc_unlink;
  goffset size;

  
  source_uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, source);

  smbc_stat = smbc_getFunctionStat (op_backend->smb_context);
  smbc_rename = smbc_getFunctionRename (op_backend->smb_context);
  smbc_unlink = smbc_getFunctionUnlink (op_backend->smb_context);

  res = smbc_stat (op_backend->smb_context, source_uri, &statbuf);
  if (res == -1)
    {
      errsv = errno;

      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR,
			g_io_error_from_errno (errsv),
			_("Error moving file: %s"),
			g_strerror (errsv));
      g_free (source_uri);
      return;
    }

  source_is_dir = S_ISDIR (statbuf.st_mode);
  size = statbuf.st_size;

  dest_uri = create_smb_uri (op_backend->server, op_backend->port, op_backend->share, destination);
  
  destination_exist = FALSE;
  res = smbc_stat (op_backend->smb_context, dest_uri, &statbuf);
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
				G_IO_ERROR_WOULD_MERGE,
				_("Can’t move directory over directory"));
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
      res = smbc_rename (op_backend->smb_context, dest_uri,
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
      res = smbc_unlink (op_backend->smb_context, dest_uri);
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

  
  res = smbc_rename (op_backend->smb_context, source_uri,
					 op_backend->smb_context, dest_uri);
  errsv = errno;
  g_free (source_uri);
  g_free (dest_uri);

  /* Catch moves across device boundaries */
  if (res != 0)
    {
      if (errsv == EXDEV ||
	  /* Unfortunately libsmbclient doesn't correctly return EXDEV, but falls back
	     to EINVAL, so we try to guess when this happens: */
	  (errsv == EINVAL && source_is_dir))
        {
          if (source_is_dir)
            {
              g_vfs_job_failed (G_VFS_JOB (job),
                                G_IO_ERROR,
                                G_IO_ERROR_WOULD_RECURSE,
                                _("Can’t recursively move directory"));
            }
          else
            {
              g_vfs_job_failed (G_VFS_JOB (job),
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_SUPPORTED,
                                _("Operation not supported"));
            }
        }
      else
	g_vfs_job_failed_from_errno (G_VFS_JOB (job), errsv);
    }
  else
    {
      g_vfs_job_progress_callback (size, size, job);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
g_vfs_backend_smb_class_init (GVfsBackendSmbClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_smb_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->unmount = do_unmount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->query_info_on_read = do_query_info_on_read;
  backend_class->close_read = do_close_read;
  backend_class->create = do_create;
  backend_class->append_to = do_append_to;
  backend_class->edit = do_edit;
  backend_class->replace = do_replace;
  backend_class->write = do_write;
  backend_class->seek_on_write = do_seek_on_write;
  backend_class->truncate = do_truncate;
  backend_class->query_info_on_write = do_query_info_on_write;
  backend_class->close_write = do_close_write;
  backend_class->query_info = do_query_info;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->enumerate = do_enumerate;
  backend_class->set_display_name = do_set_display_name;
  backend_class->delete = do_delete;
  backend_class->make_directory = do_make_directory;
  backend_class->move = do_move;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
  backend_class->set_attribute = do_set_attribute;
}

void
g_vfs_smb_daemon_init (void)
{
  g_set_application_name (_("Windows Shares File System Service"));
}
