/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008,2009 Benjamin Otte <otte@gnome.org>
 *               2008,2009 Andreas Henriksson <andreas@fatal.se>
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
 * Author: Benjamin Otte <otte@gnome.org>
 *         Andreas Henriksson <andreas@fatal.se>
 */


#include <config.h>

#include <errno.h> /* for strerror (EAGAIN) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsbackendftp.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsdaemonutils.h"
#include "gvfskeyring.h"

#include "ParseFTPList.h"
#include "gvfsftpconnection.h"
#include "gvfsftpdircache.h"
#include "gvfsftpfile.h"
#include "gvfsftptask.h"

/*** GTK DOC ***/

/**
 * GVfsFtpMethod:
 * @G_VFS_FTP_METHOD_UNKNOWN: method has not yet been determined
 * @G_VFS_FTP_METHOD_EPSV: use EPSV command
 * @G_VFS_FTP_METHOD_PASV: use PASV command
 * @G_VFS_FTP_METHOD_PASV_ADDR: use PASV command, but ignore the returned 
 *                              address and only use it's port
 * @G_VFS_FTP_METHOD_EPRT: use the EPRT command
 * @G_VFS_FTP_METHOD_PORT: use the PORT command
 *
 * Possible methods for creating data connections to the ftp server. If the 
 * method is @G_VFS_FTP_METHOD_UNKNOWN, all possibilities are tried.
 */


/*
 * about filename interpretation in the ftp backend
 *
 * As GVfs composes paths using a slash character, we cannot allow a slash as
 * part of a basename. Other critical characters are \r \n and sometimes the
 * space. We therefore g_uri_escape_string() filenames by default and concatenate
 * paths using slashes. This should make GVfs happy.
 *
 * Luckily, TVFS (see RFC 3xxx for details) is a specification that does exactly
 * what we want. It disallows slashes, \r and \n in filenames, so we can happily
 * use it without the need to escape. We also can operate on full paths as our
 * paths exactly match those of a TVFS-using FTP server.
 */

/** CODE ***/

G_DEFINE_TYPE (GVfsBackendFtp, g_vfs_backend_ftp, G_VFS_TYPE_BACKEND)

static gboolean
gvfs_backend_ftp_determine_features (GVfsFtpTask *task)
{
  const struct {
    const char *        name;        	/* name of feature */
    GVfsFtpFeature      enable;                /* flags to enable with this feature */
  } features[] = {
    { "MDTM", G_VFS_FTP_FEATURE_MDTM },
    { "SIZE", G_VFS_FTP_FEATURE_SIZE },
    { "TVFS", G_VFS_FTP_FEATURE_TVFS },
    { "EPRT", G_VFS_FTP_FEATURE_EPRT },
    { "EPSV", G_VFS_FTP_FEATURE_EPSV },
    { "UTF8", G_VFS_FTP_FEATURE_UTF8 },
    { "AUTH TLS", G_VFS_FTP_FEATURE_AUTH_TLS },
    { "AUTH SSL", G_VFS_FTP_FEATURE_AUTH_SSL },
    { "MFMT", G_VFS_FTP_FEATURE_MFMT },
  };
  guint i, j;
  char **reply;

  if (!g_vfs_ftp_task_send_and_check (task, 0, NULL, NULL, &reply, "FEAT"))
    return FALSE;

  task->backend->features = 0;
  for (i = 1; reply[i]; i++)
    {
      char *feature = reply[i];

      if (feature[0] != ' ')
        continue;
      feature++;

      /* There should just be one space according to RFC2389, but some
       * servers have more so we deal with any number of leading spaces.
       */
      while (g_ascii_isspace (feature[0]))
        feature++;

      for (j = 0; j < G_N_ELEMENTS (features); j++)
        {
          if (g_ascii_strcasecmp (feature, features[j].name) == 0)
            {
              g_debug ("# feature %s supported\n", features[j].name);
              task->backend->features |= 1 << features[j].enable;
            }
        }
    }

  g_strfreev (reply);

  return TRUE;
}

static void
gvfs_backend_ftp_determine_site_features (GVfsFtpTask *task)
{
  const struct {
    const char *        name;        	/* name of feature */
    GVfsFtpFeature      enable;         /* flags to enable with this feature */
  } features[] = {
    { "CHMOD", G_VFS_FTP_FEATURE_CHMOD },
    { "CHGRP", G_VFS_FTP_FEATURE_CHGRP },
  };
  guint i, j;
  char **reply;

  if (g_vfs_ftp_task_is_in_error (task))
    return;

  if (!g_vfs_ftp_task_send_and_check (task, 0, NULL, NULL, &reply, "SITE HELP"))
    {
      g_vfs_ftp_task_clear_error (task);
      return;
    }

  if (g_strv_length (reply) == 1)
    {
      /* vsftpd returns just a single string, so we split it into multiple
       * and then treat it like a regular reply */
      char **split;

      split = g_strsplit (reply[0], " ", -1);
      g_strfreev (reply);
      reply = split;
    }

  for (i = 1; reply[i]; i++)
    {
      char *feature = reply[i];

      while (g_ascii_isspace (feature[0]))
        feature++;

      for (j = 0; j < G_N_ELEMENTS (features); j++)
        {
          if (g_ascii_strcasecmp (feature, features[j].name) == 0)
            {
              g_debug ("# site feature %s supported\n", features[j].name);
              task->backend->features |= 1 << features[j].enable;
            }
        }
    }

  g_strfreev (reply);

  return;
}

static void
gvfs_backend_ftp_determine_system (GVfsFtpTask *task)
{
  static const struct {
    const char *  id;
    GVfsFtpSystem system;
    const char *  debug_name;
  } known_systems[] = {
    /* NB: the first entry that matches is taken, so order matters */
    { "UNIX ", G_VFS_FTP_SYSTEM_UNIX, "Unix"},
    { "WINDOWS_NT ", G_VFS_FTP_SYSTEM_WINDOWS, "Windows NT" }
  };
  guint i;
  char *system_name;
  char **reply;

  if (g_vfs_ftp_task_is_in_error (task))
    return;

  if (!g_vfs_ftp_task_send_and_check (task, 0, NULL, NULL, &reply, "SYST"))
    {
      g_vfs_ftp_task_clear_error (task);
      return;
    }

  system_name = reply[0] + 4;
  for (i = 0; i < G_N_ELEMENTS (known_systems); i++)
    {
      if (g_ascii_strncasecmp (system_name,
                               known_systems[i].id,
                	       strlen (known_systems[i].id)) == 0)
        {
          task->backend->system = known_systems[i].system;
          g_debug ("# system is %s\n", known_systems[i].debug_name);
          break;
        }
    }

  g_strfreev (reply);
}

static void
gvfs_backend_ftp_setup_directory_cache (GVfsBackendFtp *ftp)
{
  if (ftp->system == G_VFS_FTP_SYSTEM_UNIX)
    ftp->dir_funcs = &g_vfs_ftp_dir_cache_funcs_unix;
  else
    ftp->dir_funcs = &g_vfs_ftp_dir_cache_funcs_default;

  ftp->dir_cache = g_vfs_ftp_dir_cache_new (ftp->dir_funcs);
}

/* This parses a file according to RFC 959 Appendix II:
 *
 * the server should return a line of the form:
 *   257<space>"<directory-name>"<space><commentary>
 * That is, the server will tell the user what string to use when
 * referring to the created  directory.  The directory name can
 * contain any character; embedded double-quotes should be escaped by
 * double-quotes (the "quote-doubling" convention).
 *
 * Note that we actually accept (as regexp):
 * ....\s*"<directory-name>".*
 * so we're quite a bit more lax.
 *
 * If parsing this name fails, this function returns NULL.
 */
static GVfsFtpFile *
g_vfs_backend_create_file_from_reply (GVfsBackendFtp *ftp, const char *name)
{
  GVfsFtpFile *file;
  const char *quote;
  GString *unescaped;

  /* strip leading spaces */
  while (g_ascii_isspace (*name))
    name++;

  if (*name++ != '"')
    {
      g_debug ("# filename didn't start with a quote\n");
      return NULL;
    }

  unescaped = g_string_new (NULL);
  while ((quote = strchr (name, '"')))
    {
      g_string_append_len (unescaped, name, quote-name);
      name = quote + 2;
      if (quote[1] == '"')
        g_string_append_c (unescaped, '"');
      else
        break;
    }

  /* NB: The name variable may point into bad space here */
  if (quote == NULL)
    {
      g_debug ("# filename didn't end with a quote\n");
      g_string_free (unescaped, TRUE);
      return NULL;
    }
  else if (!g_ascii_isspace (quote[1]))
    {
      /* issue a warning here, just so we can guess right from debug logs */
      g_debug ("# warning: filename didn't contain space after final quote\n");
    }

  file = g_vfs_ftp_file_new_from_ftp (ftp, unescaped->str);

  g_string_free (unescaped, TRUE);
  return file;
}

static void
gvfs_backend_ftp_determine_default_location (GVfsFtpTask *task)
{
  char **reply;
  GVfsFtpFile *home;

  if (g_vfs_ftp_task_is_in_error (task))
    return;

  if (!g_vfs_ftp_task_send_and_check (task, 0, NULL, NULL, &reply, "PWD"))
    {
      g_vfs_ftp_task_clear_error (task);
      return;
    }

  home = g_vfs_backend_create_file_from_reply (task->backend, reply[0] + 4);
  if (home != NULL)
    {
      g_vfs_backend_set_default_location (G_VFS_BACKEND (task->backend),
                                          g_vfs_ftp_file_get_gvfs_path (home));
      g_vfs_ftp_file_free (home);
    }

  g_strfreev (reply);
}

/*** COMMON FUNCTIONS WITH SPECIAL HANDLING ***/

static gboolean
g_vfs_ftp_task_cd (GVfsFtpTask *task, const GVfsFtpFile *file)
{
  guint response = g_vfs_ftp_task_send (task,
                			G_VFS_FTP_PASS_550,
                			"CWD %s", g_vfs_ftp_file_get_ftp_path (file));
  if (response == 550)
    {
      g_set_error_literal (&task->error,
                           G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                	   _("The file is not a directory"));
      response = 0;
    }

  return response != 0;
}

static gboolean
g_vfs_ftp_task_try_cd (GVfsFtpTask *task, const GVfsFtpFile *file)
{
  if (g_vfs_ftp_task_is_in_error (task))
    return FALSE;

  if (!g_vfs_ftp_task_cd (task, file))
    {
      g_vfs_ftp_task_clear_error (task);
      return FALSE;
    }
 
  return TRUE;
}

/*** default directory reading ***/

static void
g_vfs_backend_ftp_finalize (GObject *object)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (object);

  if (ftp->addr)
    g_object_unref (ftp->addr);

  /* has been cleared on unmount, however it has to be cleared when mount fails */
  if (ftp->queue)
    g_queue_free (ftp->queue);

  g_cond_clear (&ftp->cond);
  g_mutex_clear (&ftp->mutex);

  g_free (ftp->user);
  g_free (ftp->password);

  g_clear_object (&ftp->server_identity);
  g_clear_object (&ftp->certificate);

  if (G_OBJECT_CLASS (g_vfs_backend_ftp_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_ftp_parent_class)->finalize) (object);
}

static void
g_vfs_backend_ftp_init (GVfsBackendFtp *ftp)
{
  g_mutex_init (&ftp->mutex);
  g_cond_init (&ftp->cond);
}

static guint
g_vfs_backend_ftp_default_port (GVfsBackendFtp *ftp)
{
  return (ftp->tls_mode == G_VFS_FTP_TLS_MODE_IMPLICIT) ? 990 : 21;
}

/* If the initial connection has a verification error, display the certificate
 * to the user and ask whether to proceed. */
static gboolean
initial_certificate_cb (GTlsConnection *conn,
                        GTlsCertificate *certificate,
                        GTlsCertificateFlags errors,
                        gpointer user_data)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (user_data);

  /* Save the certificate and result for reconnections. */
  ftp->certificate = g_object_ref (certificate);
  ftp->certificate_errors = errors;

  return gvfs_accept_certificate (ftp->mount_source, certificate, errors);
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  char *prompt = NULL;
  char *username;
  char *password;
  char *display_name;
  gboolean aborted, anonymous, break_on_fail;
  GPasswordSave password_save = G_PASSWORD_SAVE_NEVER;
  GNetworkAddress *addr;
  guint port, default_port;

restart:
  task.conn = g_vfs_ftp_connection_new (ftp->addr, task.cancellable, &task.error);
  /* fail fast here. No need to ask for a password if we know the hostname
   * doesn't exist or the given host/port doesn't have an ftp server running.
   */
  if (task.conn == NULL)
    {
      g_vfs_ftp_task_done (&task);
      return;
    }

  /* Receive the greeting, and secure the initial connection if necessary. This
   * may result in a prompt for the user.
   */
  ftp->mount_source = mount_source;
  if (!g_vfs_ftp_task_initial_handshake (&task, initial_certificate_cb, ftp))
    {
      ftp->mount_source = NULL;
      g_vfs_ftp_task_done (&task);
      return;
    }
  ftp->mount_source = NULL;

  if (!g_vfs_backend_ftp_uses_workaround (ftp, G_VFS_FTP_WORKAROUND_FEAT_AFTER_LOGIN) &&
      !gvfs_backend_ftp_determine_features (&task))
    {
      g_vfs_backend_ftp_use_workaround (ftp, G_VFS_FTP_WORKAROUND_FEAT_AFTER_LOGIN);
      ftp->features = 0;
      g_vfs_ftp_task_clear_error (&task);
      if (!g_vfs_ftp_connection_is_usable (task.conn))
        {
          g_vfs_ftp_connection_free (task.conn);
          goto restart;
        }
    }

  addr = G_NETWORK_ADDRESS (ftp->addr);
  g_object_ref (addr);
  port = g_network_address_get_port (addr);
  default_port = g_vfs_backend_ftp_default_port (ftp);
  username = NULL;
  password = NULL;
  break_on_fail = FALSE;
 
  if (ftp->user != NULL && strcmp (ftp->user, "anonymous") == 0)
    {
      anonymous = TRUE;
      break_on_fail = TRUE;
      goto try_login;
    }

  if (g_vfs_keyring_lookup_password (ftp->user,
                                     g_network_address_get_hostname (addr),
                                     NULL,
                                     "ftp",
                                     NULL,
                                     NULL,
                                     (port == default_port) ? 0 : port,
                                     &username,
                                     NULL,
                                     &password) &&
      username != NULL &&
      password != NULL)
    {
      anonymous = FALSE;
      goto try_login;
    }
  g_free (username);
  g_free (password);
  username = NULL;
  password = NULL;

  while (TRUE)
    {
      GAskPasswordFlags flags;
      if (prompt == NULL)
        {
          if (ftp->has_initial_user)
            /* Translators: the first %s is the username, the second the host name */
            prompt = g_strdup_printf (_("Authentication Required\nEnter password for “%s” on “%s”:"), ftp->user, ftp->host_display_name);
          else
            /* Translators: %s here is the hostname */
            prompt = g_strdup_printf (_("Authentication Required\nEnter user and password for “%s”:"), ftp->host_display_name);
        }
         
      flags = G_ASK_PASSWORD_NEED_PASSWORD;
       
      if (!ftp->has_initial_user)
        flags |= G_ASK_PASSWORD_NEED_USERNAME | G_ASK_PASSWORD_ANONYMOUS_SUPPORTED;
     
      if (g_vfs_keyring_is_available ())
        flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;
     
      if (!g_mount_source_ask_password (
                	mount_source,
                        prompt,
                	ftp->user,
                        NULL,
                        flags,
                        &aborted,
                        &password,
                        &username,
                        NULL,
                	&anonymous,
                        &password_save) ||
          aborted)
        {
          g_set_error_literal (&task.error, G_IO_ERROR,
                               aborted ? G_IO_ERROR_FAILED_HANDLED : G_IO_ERROR_PERMISSION_DENIED,
                	       _("Password dialog cancelled"));
          break;
        }

      /* NEED_USERNAME wasn't set */
      if (ftp->has_initial_user)
        {
          g_free (username);
          username = g_strdup (ftp->user);
        }
     
try_login:
      g_free (ftp->user);
      g_free (ftp->password);
      if (anonymous)
        {
          g_free (username);
          g_free (password);
          ftp->user = g_strdup ("anonymous");
          ftp->password = g_strdup ("");
          if (g_vfs_ftp_task_login (&task, "anonymous", "") != 0)
            break;
          g_free (ftp->user);
          g_free (ftp->password);
          ftp->user = NULL;
          ftp->password = NULL;
        }
      else
        {
          ftp->user = username ? username : g_strdup ("");
          ftp->password = password;
          if (g_vfs_ftp_task_login (&task, ftp->user, ftp->password) != 0)
            break;
        }
     
      if (break_on_fail ||
          !g_error_matches (task.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        break;

      g_vfs_ftp_task_clear_error (&task);
    }
 
  /* send post-login commands */
  if (g_vfs_backend_ftp_uses_workaround (ftp, G_VFS_FTP_WORKAROUND_FEAT_AFTER_LOGIN) &&
      !g_vfs_ftp_task_is_in_error (&task))
    {
      if (!gvfs_backend_ftp_determine_features (&task))
        {
          g_vfs_ftp_task_clear_error (&task);
          ftp->features = G_VFS_FTP_FEATURES_DEFAULT;
        }
    }
  g_vfs_ftp_task_setup_connection (&task);
  gvfs_backend_ftp_determine_system (&task);
  gvfs_backend_ftp_determine_site_features (&task);
  gvfs_backend_ftp_setup_directory_cache (ftp);
  gvfs_backend_ftp_determine_default_location (&task);

  /* Save the address of the current connection, so that for future connections,
   * we are sure to connect to the same machine.
   * The idea here is to avoid using mirrors that have a different state, which
   * might cause Heisenbugs.
   */
  if (!g_vfs_ftp_task_is_in_error (&task))
    {
      g_object_unref (ftp->addr);
      ftp->addr = G_SOCKET_CONNECTABLE (g_vfs_ftp_connection_get_address (task.conn, &task.error));
      if (ftp->addr == NULL)
        {
          g_debug ("# error querying remote address: %s\nUsing original address instead.\n", task.error->message);
          g_vfs_ftp_task_clear_error (&task);
          ftp->addr = G_SOCKET_CONNECTABLE (g_object_ref (addr));
        }
    }

  if (g_vfs_ftp_task_is_in_error (&task))
    {
      g_vfs_ftp_connection_free (task.conn);
      task.conn = NULL;
      g_vfs_ftp_task_done (&task);
      g_object_unref (addr);
      return;
    }

  if (prompt && !anonymous)
    {
      /* a prompt was created, so we have to save the password */
      g_vfs_keyring_save_password (ftp->user,
                                   g_network_address_get_hostname (addr),
                                   NULL,
                                   "ftp",
                                   NULL,
                                   NULL,
                                   (port == default_port) ? 0 : port,
                                   ftp->password,
                                   password_save);
      g_free (prompt);
    }

  if (ftp->tls_mode == G_VFS_FTP_TLS_MODE_EXPLICIT)
    mount_spec = g_mount_spec_new ("ftps");
  else if (ftp->tls_mode == G_VFS_FTP_TLS_MODE_IMPLICIT)
    mount_spec = g_mount_spec_new ("ftpis");
  else
    mount_spec = g_mount_spec_new ("ftp");
  g_mount_spec_set (mount_spec, "host", g_network_address_get_hostname (addr));
  if (port != default_port)
    {
      char *port_str = g_strdup_printf ("%u", port);
      g_mount_spec_set (mount_spec, "port", port_str);
      g_free (port_str);
    }

  if (ftp->has_initial_user)
    g_mount_spec_set (mount_spec, "user", ftp->user);
     
  if (g_str_equal (ftp->user, "anonymous"))
    display_name = g_strdup (ftp->host_display_name);
  else
    {
      /* Translators: the first %s is the username, the second the host name */
      display_name = g_strdup_printf (_("%s on %s"), ftp->user, ftp->host_display_name);
    }
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);

  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  g_vfs_backend_set_icon_name (backend, "folder-remote");
  g_vfs_backend_set_symbolic_icon_name (backend, "folder-remote-symbolic");

  ftp->connections = 1;
  ftp->max_connections = G_MAXUINT;
  ftp->queue = g_queue_new ();
  ftp->root = g_vfs_ftp_file_new_from_ftp (ftp, "/");

  g_object_unref (addr);
  g_vfs_ftp_task_done (&task);
}

static gboolean
try_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  const char *type, *host, *port_str;
  guint port;

  type = g_mount_spec_get_type (mount_spec);
  if (g_strcmp0 (type, "ftps") == 0)
    ftp->tls_mode = G_VFS_FTP_TLS_MODE_EXPLICIT;
  else if (g_strcmp0 (type, "ftpis") == 0)
    ftp->tls_mode = G_VFS_FTP_TLS_MODE_IMPLICIT;
  else
    ftp->tls_mode = G_VFS_FTP_TLS_MODE_NONE;

  host = g_mount_spec_get (mount_spec, "host");
  if (host == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                       G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("No hostname specified"));
      return TRUE;
    }
  port_str = g_mount_spec_get (mount_spec, "port");
  if (port_str == NULL)
    {
      port = g_vfs_backend_ftp_default_port (ftp);
    }
  else
    {
      /* FIXME: error handling? */
      port = strtoul (port_str, NULL, 10);
    }

  ftp->addr = g_network_address_new (host, port);
  ftp->user = g_strdup (g_mount_spec_get (mount_spec, "user"));
  ftp->has_initial_user = ftp->user != NULL;
  if (port == g_vfs_backend_ftp_default_port (ftp))
    ftp->host_display_name = g_strdup (host);
  else
    ftp->host_display_name = g_strdup_printf ("%s:%u", host, port);

  if (ftp->tls_mode != G_VFS_FTP_TLS_MODE_NONE)
    ftp->server_identity = g_object_ref (ftp->addr);

  return FALSE;
}

static void
do_unmount (GVfsBackend *   backend,
            GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpConnection *conn;

  /* Freed early to avoid finalize not being called due to circular references */
  g_clear_pointer (&ftp->root, g_vfs_ftp_file_free);

  g_mutex_lock (&ftp->mutex);
  while ((conn = g_queue_pop_head (ftp->queue)))
    {
      /* FIXME: properly quit */
      g_vfs_ftp_connection_free (conn);
    }
  g_queue_free (ftp->queue);
  ftp->queue = NULL;
  g_cond_broadcast (&ftp->cond);
  g_mutex_unlock (&ftp->mutex);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* NB: sets EPERM if file exists, ENOENT if not - an error will _always_ be set */
static void
error_550_permission_or_not_found (GVfsFtpTask *task, gpointer file)
{
  GFileInfo *info;
  
  info = g_vfs_ftp_dir_cache_lookup_file (task->backend->dir_cache, task, file, FALSE);
  if (info)
    {
      g_object_unref (info);
      g_set_error_literal (&task->error,
                           G_IO_ERROR,
                           G_IO_ERROR_PERMISSION_DENIED,
                           _("Insufficient permissions"));
    }
  else
    {
      /* clear potential error from file lookup above */
      g_vfs_ftp_task_clear_error (task);
      g_set_error_literal (&task->error,
                           G_IO_ERROR,
                           G_IO_ERROR_NOT_FOUND,
                           _("File doesn’t exist"));
    }
}

static void
error_550_exists (GVfsFtpTask *task, gpointer file)
{
  GFileInfo *info;
  
  info = g_vfs_ftp_dir_cache_lookup_file (task->backend->dir_cache, task, file, FALSE);
  if (info)
    {
      g_object_unref (info);
      g_set_error_literal (&task->error,
                           G_IO_ERROR,
                           G_IO_ERROR_EXISTS,
                           _("Target file already exists"));
    }
  else
    {
      /* clear potential error from file lookup above */
      g_vfs_ftp_task_clear_error (task);
    }
}

static void
error_550_is_directory (GVfsFtpTask *task, gpointer file)
{
  GFileInfo *info;
  
  /* need to resolve symlinks here to know if a link is a dir or not */
  info = g_vfs_ftp_dir_cache_lookup_file (task->backend->dir_cache, task, file, TRUE);
  if (info && g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      g_object_unref (info);
      g_set_error_literal (&task->error, G_IO_ERROR,
                           G_IO_ERROR_IS_DIRECTORY,
                           _("File is directory"));
      return;
    }
  else if (info)
    g_object_unref (info);
  
  /* clear potential error from file lookup above */
  g_vfs_ftp_task_clear_error (task);
}

static void
error_550_parent_not_found (GVfsFtpTask *task, gpointer file)
{
  GVfsFtpFile *dir = g_vfs_ftp_file_new_parent (file);

  if (!g_vfs_ftp_file_equal (file, dir) && !g_vfs_ftp_task_try_cd (task, dir))
    {
      /* Yes, this is a weird error for a missing parent directory */
      g_set_error_literal (&task->error, G_IO_ERROR,
                           G_IO_ERROR_NOT_FOUND,
                           _("No such file or directory"));
    }

  g_vfs_ftp_file_free (dir);
}

static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *file;
  static const GVfsFtpErrorFunc open_read_handlers[] = { error_550_is_directory, 
                                                         error_550_permission_or_not_found, 
                                                         NULL };

  g_vfs_ftp_task_setup_data_connection (&task);
  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);

  g_vfs_ftp_task_send_and_check (&task,
                                 G_VFS_FTP_PASS_100 | G_VFS_FTP_FAIL_200,
                                 open_read_handlers,
                                 file,
                                 NULL,
                                 "RETR %s", g_vfs_ftp_file_get_ftp_path (file));
  g_vfs_ftp_file_free (file);

  g_vfs_ftp_task_open_data_connection (&task);

  if (!g_vfs_ftp_task_is_in_error (&task))
    {
      /* don't push the connection back, it's our handle now */
      GVfsFtpConnection *conn = g_vfs_ftp_task_take_connection (&task);

      g_vfs_job_open_for_read_set_handle (job, conn);
      g_vfs_job_open_for_read_set_can_seek (job, FALSE);
    }

  g_vfs_ftp_task_done (&task);
}

static void
do_close_read (GVfsBackend *     backend,
               GVfsJobCloseRead *job,
               GVfsBackendHandle handle)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpConnection *conn = handle;

  g_vfs_ftp_task_give_connection (&task, conn);
  g_vfs_ftp_task_close_data_connection (&task);
  g_vfs_ftp_task_receive (&task, 0, NULL);

  g_vfs_ftp_task_done (&task);
}

static void
do_read (GVfsBackend *     backend,
         GVfsJobRead *     job,
         GVfsBackendHandle handle,
         char *            buffer,
         gsize             bytes_requested)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpConnection *conn = handle;
  GInputStream *input;
  gssize n_bytes;

  input = g_io_stream_get_input_stream (g_vfs_ftp_connection_get_data_stream (conn));
  n_bytes = g_input_stream_read (input,
                                 buffer,
                                 bytes_requested,
                                 task.cancellable,
                                 &task.error);

  if (n_bytes >= 0)
    g_vfs_job_read_set_size (job, n_bytes);

  g_vfs_ftp_task_done (&task);
}

static void
do_start_write (GVfsFtpTask *task,
                GFileCreateFlags flags,
                const char *format,
                ...) G_GNUC_PRINTF (3, 4);
static void
do_start_write (GVfsFtpTask *task,
                GFileCreateFlags flags,
                const char *format,
                ...)
{
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (task->job);
  va_list varargs;

  /* FIXME: can we honour the flags? */

  g_vfs_ftp_task_setup_data_connection (task);

  va_start (varargs, format);
  g_vfs_ftp_task_sendv (task,
                        G_VFS_FTP_PASS_100 | G_VFS_FTP_FAIL_200,
                        NULL,
                        format,
                        varargs);
  va_end (varargs);

  g_vfs_ftp_task_open_data_connection (task);

  if (!g_vfs_ftp_task_is_in_error (task))
    {
      GIOStream *stream;

      /* don't push the connection back, it's our handle now */
      GVfsFtpConnection *conn = g_vfs_ftp_task_take_connection (task);
      g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (task->job), conn);
      g_vfs_job_open_for_write_set_can_seek (G_VFS_JOB_OPEN_FOR_WRITE (task->job), FALSE);

      stream = g_vfs_ftp_connection_get_data_stream (conn);
      g_object_set_data_full (G_OBJECT (stream), "g-vfs-backend-ftp-filename",
                              g_strdup (job->filename), g_free);
    }
}

static void
do_create (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GFileInfo *info;
  GVfsFtpFile *file;

  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);
  info = g_vfs_ftp_dir_cache_lookup_file (ftp->dir_cache, &task, file, FALSE);
  if (info)
    {
      g_object_unref (info);
      g_set_error_literal (&task.error,
                           G_IO_ERROR,
                	   G_IO_ERROR_EXISTS,
                	   _("Target file already exists"));
      g_vfs_ftp_file_free (file);
      g_vfs_ftp_task_done (&task);
      return;
    }
  do_start_write (&task, flags, "STOR %s", g_vfs_ftp_file_get_ftp_path (file));
  g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
  g_vfs_ftp_file_free (file);

  g_vfs_ftp_task_done (&task);
}

static void
do_append (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *file;

  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);
  do_start_write (&task, flags, "APPE %s", g_vfs_ftp_file_get_ftp_path (file));
  g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
  g_vfs_ftp_file_free (file);

  g_vfs_ftp_task_done (&task);
}

static void
do_replace (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            const char *etag,
            gboolean make_backup,
            GFileCreateFlags flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *file, *backupfile = NULL;
  static const GVfsFtpErrorFunc rnfr_handlers[] = { error_550_permission_or_not_found,
                                                    NULL };

  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);

  if (make_backup)
    {
      GFileInfo *info;
      char *backup_path = g_strconcat (filename, "~", NULL);
      backupfile = g_vfs_ftp_file_new_from_gvfs (ftp, backup_path);
      g_free (backup_path);

      info = g_vfs_ftp_dir_cache_lookup_file (ftp->dir_cache, &task, file, FALSE);

      if (info)
        {
          guint ret;

          g_object_unref (info);

          ret = g_vfs_ftp_task_send (&task,
                                     G_VFS_FTP_PASS_550,
                                     "DELE %s", g_vfs_ftp_file_get_ftp_path (backupfile));
          if (!ret)
            goto err_backup;
          g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, backupfile);

          ret = g_vfs_ftp_task_send_and_check (&task,
                                               G_VFS_FTP_PASS_300 | G_VFS_FTP_FAIL_200,
                                               rnfr_handlers,
                                               file,
                                               NULL,
                                               "RNFR %s", g_vfs_ftp_file_get_ftp_path (file));
          if (!ret)
            goto err_backup;

          ret = g_vfs_ftp_task_send (&task,
                                     0,
                                     "RNTO %s", g_vfs_ftp_file_get_ftp_path (backupfile));
          if (!ret)
            goto err_backup;

          g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
        }
      g_vfs_ftp_file_free (backupfile);
    }

  do_start_write (&task, flags, "STOR %s", g_vfs_ftp_file_get_ftp_path (file));
  g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
  g_vfs_ftp_file_free (file);

  g_vfs_ftp_task_done (&task);

  return;

err_backup:
  g_vfs_ftp_file_free (file);
  g_vfs_ftp_file_free (backupfile);
  g_vfs_ftp_task_clear_error (&task);
  g_set_error_literal (&task.error,
                       G_IO_ERROR,
                       G_IO_ERROR_CANT_CREATE_BACKUP,
                       _("Backup file creation failed"));
  g_vfs_ftp_task_done (&task);
}

static void
do_close_write (GVfsBackend *backend,
                GVfsJobCloseWrite *job,
                GVfsBackendHandle handle)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpConnection *conn = handle;
  GIOStream *stream;
  const gchar *filename;
  GVfsFtpFile *file;

  stream = g_vfs_ftp_connection_get_data_stream (conn);
  filename = g_object_get_data (G_OBJECT (stream), "g-vfs-backend-ftp-filename");
  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);

  g_vfs_ftp_task_give_connection (&task, handle);
  g_vfs_ftp_task_close_data_connection (&task);
  g_vfs_ftp_task_receive (&task, 0, NULL);

  g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
  g_vfs_ftp_file_free (file);

  g_vfs_ftp_task_done (&task);
}

static void
do_write (GVfsBackend *backend,
          GVfsJobWrite *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize buffer_size)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpConnection *conn = handle;
  GOutputStream *output;
  gssize n_bytes;

  output = g_io_stream_get_output_stream (g_vfs_ftp_connection_get_data_stream (conn));

  /* FIXME: use write_all here? */
  n_bytes = g_output_stream_write (output,
                                   buffer,
                                   buffer_size,
                                   task.cancellable,
                                   &task.error);
           
  if (n_bytes >= 0)
    g_vfs_job_write_set_written_size (job, n_bytes);

  g_vfs_ftp_task_done (&task);
}

static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *job,
               const char *filename,
               GFileQueryInfoFlags query_flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *file;
  GFileInfo *real;
 
  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);
  real = g_vfs_ftp_dir_cache_lookup_file (ftp->dir_cache,
                                          &task,
                                          file,
                                          query_flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS ? FALSE : TRUE);

  if (real)
    {
      g_file_info_copy_into (real, info);
      g_object_unref (real);
    }
  else if (!g_vfs_ftp_task_is_in_error (&task))
    {
      g_set_error_literal (&task.error,
                           G_IO_ERROR,
                           G_IO_ERROR_NOT_FOUND,
                           _("File doesn’t exist"));
    }

  g_vfs_ftp_task_done (&task);
  g_vfs_ftp_file_free (file);
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
			       GVfsJobQueryAttributes *job,
			       const char *filename)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GFileAttributeInfoList *list;

  list = g_file_attribute_info_list_new ();

  if (g_vfs_backend_ftp_has_feature (ftp, G_VFS_FTP_FEATURE_CHMOD))
    {
      g_file_attribute_info_list_add (list,
                                      G_FILE_ATTRIBUTE_UNIX_MODE,
                                      G_FILE_ATTRIBUTE_TYPE_UINT32,
                                      G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                      G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
    }

  if (g_vfs_backend_ftp_has_feature (ftp, G_VFS_FTP_FEATURE_MFMT))
    {
      g_file_attribute_info_list_add (list,
                                      G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                      G_FILE_ATTRIBUTE_TYPE_UINT64,
                                      G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                      G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
    }

  if (list->n_infos == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));

      g_file_attribute_info_list_unref (list);

      return TRUE;
    }

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
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *file;

  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);

  if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0 &&
      g_vfs_backend_ftp_has_feature (ftp, G_VFS_FTP_FEATURE_CHMOD))
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT32) 
        {
          g_set_error_literal (&task.error,
                               G_IO_ERROR,
                               G_IO_ERROR_INVALID_ARGUMENT,
                               _("Invalid attribute type (uint32 expected)"));
        }
      else
        {
          guint mode = *(guint32 *)value_p;

          if (g_vfs_ftp_task_send (&task,
                                   0,
                                   "SITE CHMOD %04o %s",
                                   mode & (S_IRWXU | S_IRWXG | S_IRWXO),
                                   g_vfs_ftp_file_get_ftp_path (file)))
            {
              g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
            }
        }
    }
  else if (g_strcmp0 (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED) == 0 &&
           g_vfs_backend_ftp_has_feature (ftp, G_VFS_FTP_FEATURE_MFMT))
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT64)
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            _("Invalid attribute type (uint64 expected)"));
        }
      else
        {
          GDateTime *dt;
          gchar *value;

          dt = g_date_time_new_from_unix_utc (*(guint64 *)value_p);
          value = g_date_time_format (dt, "%Y%m%d%H%M%S");

          if (g_vfs_ftp_task_send (&task,
                                   0,
                                   "MFMT %s %s",
                                   value,
                                   g_vfs_ftp_file_get_ftp_path (file)))
            {
              g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
            }

          g_date_time_unref (dt);
          g_free (value);
        }
    }
  else
    {
      g_set_error_literal (&task.error,
                           G_IO_ERROR,
                           G_IO_ERROR_NOT_SUPPORTED,
                           _("Operation not supported"));
    }

  g_vfs_ftp_task_done (&task);
  g_vfs_ftp_file_free (file);
}

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *dirname,
              GFileAttributeMatcher *matcher,
              GFileQueryInfoFlags query_flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *dir;
  GList *list, *walk;

  dir = g_vfs_ftp_file_new_from_gvfs (ftp, dirname);
  list = g_vfs_ftp_dir_cache_lookup_dir (ftp->dir_cache,
                                         &task,
                                         dir,
                                         TRUE,
                                         query_flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS ? FALSE : TRUE);
  g_vfs_ftp_file_free (dir);
  if (g_vfs_ftp_task_is_in_error (&task))
    {
      g_assert (list == NULL);
      g_vfs_ftp_task_done (&task);
      return;
    }

  g_vfs_ftp_task_done (&task);

  for (walk = list; walk; walk = walk->next)
    {
      GFileInfo *matched_info = g_file_info_new ();

      /* copy into a new GFileInfo as g_vfs_job_enumerate_add_info()
       * modifies the given GFileInfo */
      g_file_info_copy_into (walk->data, matched_info);
      g_vfs_job_enumerate_add_info (job, matched_info);
      g_object_unref (matched_info);
      g_object_unref (walk->data);
    }
  g_vfs_job_enumerate_done (job);

  g_list_free (list);
}

static void
do_set_display_name (GVfsBackend *backend,
                     GVfsJobSetDisplayName *job,
                     const char *filename,
                     const char *display_name)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *original, *dir, *now;

  original = g_vfs_ftp_file_new_from_gvfs (ftp, filename);
  dir = g_vfs_ftp_file_new_parent (original);
  now = g_vfs_ftp_file_new_child (dir, display_name, &task.error);

  /* Rename a directory that has been "opened" by CWD may fail, so cd to root first */
  g_vfs_ftp_task_try_cd (&task, ftp->root);
  g_vfs_ftp_task_send (&task,
                       G_VFS_FTP_PASS_300 | G_VFS_FTP_FAIL_200,
                       "RNFR %s", g_vfs_ftp_file_get_ftp_path (original));
  g_vfs_ftp_task_send (&task,
                       0,
                       "RNTO %s", g_vfs_ftp_file_get_ftp_path (now));

  /* FIXME: parse result of RNTO here? */
  g_vfs_job_set_display_name_set_new_path (job, g_vfs_ftp_file_get_gvfs_path (now));
  g_vfs_ftp_dir_cache_purge_dir (ftp->dir_cache, dir);
  g_vfs_ftp_file_free (now);
  g_vfs_ftp_file_free (dir);
  g_vfs_ftp_file_free (original);

  g_vfs_ftp_task_done (&task);
}

static void
do_delete (GVfsBackend *backend,
           GVfsJobDelete *job,
           const char *filename)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *file;
  guint response;

  /* We try file deletion first. If that fails, we try directory deletion.
   * The file-first-then-directory order has been decided by coin-toss. */
  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);
  response = g_vfs_ftp_task_send (&task,
                		  G_VFS_FTP_PASS_500,
                		  "DELE %s", g_vfs_ftp_file_get_ftp_path (file));
  if (G_VFS_FTP_RESPONSE_GROUP (response) == 5)
    {
      response = g_vfs_ftp_task_send (&task,
                		      G_VFS_FTP_PASS_550,
                		      "RMD %s", g_vfs_ftp_file_get_ftp_path (file));
      if (response == 550)
        {
          GList *list = g_vfs_ftp_dir_cache_lookup_dir (ftp->dir_cache,
                                                        &task,
                                                        file,
                                                        FALSE,
                                                        FALSE);
          if (list)
            {
              g_set_error_literal (&task.error,
                		   G_IO_ERROR,
                		   G_IO_ERROR_NOT_EMPTY,
                		   g_strerror (ENOTEMPTY));
              g_list_free_full (list, g_object_unref);
            }
          else
            {
              g_vfs_ftp_task_clear_error (&task);
              g_vfs_ftp_task_set_error_from_response (&task, response);
            }
        }
    }

  g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
  g_vfs_ftp_file_free (file);

  g_vfs_ftp_task_done (&task);
}

static void
do_make_directory (GVfsBackend *backend,
                   GVfsJobMakeDirectory *job,
                   const char *filename)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *file;
  static const GVfsFtpErrorFunc make_directory_handlers[] = { error_550_exists, error_550_parent_not_found, NULL };

  file = g_vfs_ftp_file_new_from_gvfs (ftp, filename);
  g_vfs_ftp_task_send_and_check (&task,
                                 0,
                                 make_directory_handlers,
                                 file,
                                 NULL,
                                 "MKD %s", g_vfs_ftp_file_get_ftp_path (file));

  /* FIXME: Compare created file with name from server result to be sure
   * it's correct and otherwise fail. */
  g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, file);
  g_vfs_ftp_file_free (file);

  g_vfs_ftp_task_done (&task);
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
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *srcfile, *destfile;
  static const GVfsFtpErrorFunc rnfr_handlers[] = { error_550_permission_or_not_found,
                                                    NULL };

  /* FIXME: what about G_FILE_COPY_NOFOLLOW_SYMLINKS and G_FILE_COPY_ALL_METADATA? */

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* FIXME: implement? */

      if (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE)
        {
          g_set_error_literal (&task.error,
                               G_IO_ERROR,
                               G_IO_ERROR_CANT_CREATE_BACKUP,
                               _("Backups not supported"));
        }
      else
        {
          /* Return G_IO_ERROR_NOT_SUPPORTED instead of G_IO_ERROR_CANT_CREATE_BACKUP
           * to be proceeded with copy and delete fallback (see g_file_move). */
          g_set_error_literal (&task.error,
                               G_IO_ERROR,
                               G_IO_ERROR_NOT_SUPPORTED,
                               "Operation not supported");
        }

      g_vfs_ftp_task_done (&task);
      return;
    }

  srcfile = g_vfs_ftp_file_new_from_gvfs (ftp, source);
  destfile = g_vfs_ftp_file_new_from_gvfs (ftp, destination);
  if (g_vfs_ftp_task_try_cd (&task, destfile))
    {
      char *basename = g_path_get_basename (source);
      GVfsFtpFile *real = g_vfs_ftp_file_new_child (destfile, basename, &task.error);

      g_free (basename);
      if (real == NULL)
        {
          goto out;
        }
      else
        {
          g_vfs_ftp_file_free (destfile);
          destfile = real;
        }
    }

  if (!(flags & G_FILE_COPY_OVERWRITE))
    {
      GFileInfo *info = g_vfs_ftp_dir_cache_lookup_file (ftp->dir_cache,
                                                         &task,
                                                         destfile,
                                                         FALSE);

      if (info)
        {
          g_object_unref (info);
          g_set_error_literal (&task.error,
                	       G_IO_ERROR,
                               G_IO_ERROR_EXISTS,
                	       _("Target file already exists"));
          goto out;
        }
    }

  /* Rename a directory that has been "opened" by CWD may fail, so cd to root first */
  g_vfs_ftp_task_try_cd (&task, ftp->root);
  if (!g_vfs_ftp_task_send_and_check (&task,
                                 G_VFS_FTP_PASS_300 | G_VFS_FTP_FAIL_200,
                                 rnfr_handlers,
                                 srcfile,
                                 NULL,
                                 "RNFR %s", g_vfs_ftp_file_get_ftp_path (srcfile)))
    goto out;

  g_vfs_ftp_task_send (&task,
                       0,
                       "RNTO %s", g_vfs_ftp_file_get_ftp_path (destfile));

  g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, srcfile);
  g_vfs_ftp_dir_cache_purge_file (ftp->dir_cache, destfile);
out:
  g_vfs_ftp_file_free (srcfile);
  g_vfs_ftp_file_free (destfile);

  g_vfs_ftp_task_done (&task);
}

static void
do_pull_improve_error_message (GVfsFtpTask *task,
		               GFile       *dest,
                               gboolean     overwrite)
{
  GFileInfo *info;
  GFileType file_type;
  
  /* There was an error opening the source, try to set a good error for it.
   * Code taken from glib's gio/gfile.c:open_source_for_copy() function */

  if (g_vfs_ftp_task_error_matches (task, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
    {
      /* The source is a directory, don't fail with WOULD_RECURSE immediately, 
       * as that is less useful to the app. Better check for errors on the 
       * target instead. 
       */
      g_vfs_ftp_task_clear_error (task);
      
      info = g_file_query_info (dest, G_FILE_ATTRIBUTE_STANDARD_TYPE,
				G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				task->cancellable, &task->error);
      if (info != NULL)
	{
	  file_type = g_file_info_get_file_type (info);
	  g_object_unref (info);
	  
	  if (overwrite)
	    {
	      if (file_type == G_FILE_TYPE_DIRECTORY)
		{
		  g_set_error_literal (&task->error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                       _("Can’t copy directory over directory"));
		  return;
		}
	      /* continue to would_recurse error */
	    }
	  else
	    {
	      g_set_error_literal (&task->error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                   _("Target file already exists"));
	      return;
	    }
	}
      else
	{
	  /* Error getting info from target, return that error 
           * (except for NOT_FOUND, which is no error here) 
           */
	  if (!g_vfs_ftp_task_error_matches (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            return;
          g_vfs_ftp_task_clear_error (task);
	}
      
      g_set_error_literal (&task->error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                           _("Can’t recursively copy directory"));
    }
}

static void
do_pull (GVfsBackend *         backend,
         GVfsJobPull *         job,
         const char *          source,
         const char *          local_path,
         GFileCopyFlags        flags,
         gboolean              remove_source,
         GFileProgressCallback progress_callback,
         gpointer              progress_callback_data)
{
  static const GVfsFtpErrorFunc open_read_handlers[] = { error_550_is_directory,
                                                         error_550_permission_or_not_found, 
                                                         NULL };
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GVfsFtpTask task = G_VFS_FTP_TASK_INIT (ftp, G_VFS_JOB (job));
  GVfsFtpFile *src;
  GFile *dest;
  GInputStream *input;
  GOutputStream *output;
  goffset total_size = 0;
  guint64 mtime = 0;
  
  src = g_vfs_ftp_file_new_from_gvfs (ftp, source);
  dest = g_file_new_for_path (local_path);

  if (remove_source && (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE))
    {
      g_set_error_literal (&task.error,
                           G_IO_ERROR,
                           G_IO_ERROR_NOT_SUPPORTED,
                           _("Operation not supported"));
      goto out;
    }

  /* If the source is a symlink, then it needs to be handled specially. */
  if (flags & G_FILE_COPY_NOFOLLOW_SYMLINKS)
    {
      GFileInfo *info = g_vfs_ftp_dir_cache_lookup_file (ftp->dir_cache,
                                                         &task,
                                                         src,
                                                         FALSE);
      if (!info)
        goto out;

      if (g_file_info_get_is_symlink (info))
        {
          /* Fall back to the default implementation to copy the symlink.
           * Because of the cache, this doesn't require any extra I/O
           * operations. */
          g_set_error_literal (&task.error,
                               G_IO_ERROR,
                               G_IO_ERROR_NOT_SUPPORTED,
                               "Operation not supported");
          g_object_unref (info);
          goto out;
        }
      g_object_unref (info);
    }

  if (progress_callback)
    {
      GFileInfo *info = g_vfs_ftp_dir_cache_lookup_file (ftp->dir_cache, &task, src, TRUE);
      if (info)
        {
          total_size = g_file_info_get_size (info);
          mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
          g_object_unref (info);
        }
    }

  g_vfs_ftp_task_setup_data_connection (&task);
  g_vfs_ftp_task_send_and_check (&task,
                                 G_VFS_FTP_PASS_100 | G_VFS_FTP_FAIL_200,
                                 open_read_handlers,
                                 src,
                                 NULL,
                                 "RETR %s", g_vfs_ftp_file_get_ftp_path (src));
  g_vfs_ftp_task_open_data_connection (&task);
  if (g_vfs_ftp_task_is_in_error (&task))
    {
      do_pull_improve_error_message (&task, dest, flags & G_FILE_COPY_OVERWRITE);
      goto out;
    }

  if (flags & G_FILE_COPY_OVERWRITE)
    output = G_OUTPUT_STREAM (g_file_replace (dest,
                                              NULL,
                                              flags & G_FILE_COPY_BACKUP ? TRUE : FALSE,
                                              G_FILE_CREATE_REPLACE_DESTINATION,
                                              task.cancellable,
                                              &task.error));
  else
    output = G_OUTPUT_STREAM (g_file_create (dest,
                                             0,
                                             task.cancellable,
                                             &task.error));
  if (output == NULL)
    {
      g_vfs_ftp_task_close_data_connection (&task);
      g_vfs_ftp_task_receive (&task, 0, NULL);
      goto out;
    }

  input = g_io_stream_get_input_stream (g_vfs_ftp_connection_get_data_stream (task.conn));
  gvfs_output_stream_splice (output,
                             input,
                             G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                             total_size,
                             progress_callback,
                             progress_callback_data,
                             task.cancellable,
                             &task.error);
  g_vfs_ftp_task_close_data_connection (&task);
  g_vfs_ftp_task_receive (&task, 0, NULL);
  g_object_unref (output);

  /* Ignore errors here. Failure to copy metadata is not a hard error */
  if (!g_vfs_ftp_task_is_in_error (&task) && mtime)
    {
      g_file_set_attribute_uint64 (dest,
                                   G_FILE_ATTRIBUTE_TIME_MODIFIED, mtime,
                                   G_FILE_QUERY_INFO_NONE,
                                   task.cancellable, NULL);
    }

  if (!g_vfs_ftp_task_is_in_error (&task) && remove_source)
    {
      g_vfs_ftp_task_send (&task,
                	   G_VFS_FTP_PASS_500,
                           "DELE %s", g_vfs_ftp_file_get_ftp_path (src));
    }

out:
  g_object_unref (dest);
  g_vfs_ftp_file_free (src);
  g_vfs_ftp_task_done (&task);
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "ftp");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static void
g_vfs_backend_ftp_class_init (GVfsBackendFtpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
 
  gobject_class->finalize = g_vfs_backend_ftp_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->unmount = do_unmount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->close_read = do_close_read;
  backend_class->read = do_read;
  backend_class->create = do_create;
  backend_class->append_to = do_append;
  backend_class->replace = do_replace;
  backend_class->close_write = do_close_write;
  backend_class->write = do_write;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->set_display_name = do_set_display_name;
  backend_class->delete = do_delete;
  backend_class->make_directory = do_make_directory;
  backend_class->move = do_move;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
  backend_class->try_query_fs_info = try_query_fs_info;
  backend_class->set_attribute = do_set_attribute;
  backend_class->pull = do_pull;
}

/*** PUBLIC API ***/

/**
 * g_vfs_backend_ftp_has_feature:
 * @ftp: the backend
 * @feature: feature to check
 *
 * Checks if the FTP server supports a given @feature. Features are determined
 * once during the mount phase and are not queried again.
 *
 * Returns: %TRUE if @feature is supported.
 **/
gboolean
g_vfs_backend_ftp_has_feature (GVfsBackendFtp *ftp,
                               GVfsFtpFeature  feature)
{
  g_return_val_if_fail (G_VFS_IS_BACKEND_FTP (ftp), FALSE);
  g_return_val_if_fail (feature < 32, FALSE);

  return (ftp->features & (1 << feature)) != 0;
}

/**
 * g_vfs_backend_ftp_uses_workaround:
 * @ftp: the backend
 * @workaround: workaround to check
 *
 * Checks if the given @workaround was enabled previously using
 * g_vfs_backend_ftp_use_workaround(). See that function for a discussion
 * of the purpose of workarounds.
 *
 * Returns: %TRUE if the workaround is enabled
 **/
gboolean
g_vfs_backend_ftp_uses_workaround (GVfsBackendFtp *  ftp,
                                  GVfsFtpWorkaround workaround)
{
  g_return_val_if_fail (G_VFS_IS_BACKEND_FTP (ftp), FALSE);
  g_return_val_if_fail (workaround < 32, FALSE);

  return (g_atomic_int_get (&ftp->workarounds) & (1 << workaround)) != 0;
}

/**
 * g_vfs_backend_ftp_use_workaround:
 * @ftp: the backend
 * @workaround: workaround to set
 *
 * Sets the given @workaround to be used on the backend. Workarounds are flags
 * set on the backend to ensure a special behavior in the client to work
 * around problems with servers. See the existing workarounds for examples.
 **/
void
g_vfs_backend_ftp_use_workaround (GVfsBackendFtp *  ftp,
                                  GVfsFtpWorkaround workaround)
{
  int cur, set;

  g_return_if_fail (G_VFS_IS_BACKEND_FTP (ftp));
  g_return_if_fail (workaround < 32);

  set = 1 << workaround;
  while (((cur = g_atomic_int_get (&ftp->workarounds)) & set) == 0 &&
         !g_atomic_int_compare_and_exchange (&ftp->workarounds, cur, cur | set));
   
}

