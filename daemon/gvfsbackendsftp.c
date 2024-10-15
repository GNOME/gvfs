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

#include <stdlib.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include "gvfsicon.h"

#include "gvfsbackendsftp.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobopeniconforread.h"
#include "gvfsjobmount.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobtruncate.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryinforead.h"
#include "gvfsjobqueryinfowrite.h"
#include "gvfsjobmove.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobmakedirectory.h"
#include "gvfsjobprogress.h"
#include "gvfsjobpush.h"
#include "gvfsjobpull.h"
#include "gvfsjobsetattribute.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsutils.h"
#include "gvfskeyring.h"
#include <gvfsutils.h>
#include "sftp.h"
#include "pty_open.h"

/* TODO for sftp:
 * Implement can_delete & can_rename
 * fstat
 */

#if defined(HAVE_GRANTPT) || defined(HAVE_OPENPTY)
/* We only use this on systems with unix98 or BSD ptys */
#define USE_PTY 1
#endif

#define SFTP_READ_TIMEOUT 40   /* seconds */

/*
 * All servers SHOULD support packets of at least 34000 bytes (where the packet
 * size refers to the full length, including the header above). This should
 * allow for reads and writes of at most 32768 bytes. For more details, see
 * draft-ietf-secsh-filexfer-02.txt.
 */
#define MAX_BUFFER_SIZE 32768

static GQuark id_q;

typedef enum {
  SFTP_EXT_OPENSSH_STATVFS,
} SFTPServerExtensions;

typedef enum {
  SFTP_VENDOR_INVALID = 0,
  SFTP_VENDOR_OPENSSH,
  SFTP_VENDOR_SSH
} SFTPClientVendor;

typedef struct _MultiReply MultiReply;

typedef void (*ReplyCallback) (GVfsBackendSftp *backend,
                               int reply_type,
                               GDataInputStream *reply,
                               guint32 len,
                               GVfsJob *job,
                               gpointer user_data);

typedef void (*MultiReplyCallback) (GVfsBackendSftp *backend,
                                    MultiReply *replies,
                                    int n_replies,
                                    GVfsJob *job,
                                    gpointer user_data);


typedef struct {
  MultiReply *replies;
  int n_replies;
  int n_outstanding;
  gpointer user_data;
  MultiReplyCallback callback;
} MultiRequest;

struct _MultiReply {
  int type;
  GDataInputStream *data;
  guint32 data_len;

  MultiRequest *request;
};



typedef struct {
  guchar *data;
  gsize size;
} DataBuffer;

typedef struct {
  DataBuffer *raw_handle;
  goffset offset;
  char *filename;
  char *tempname;
  guint32 permissions;
  gboolean set_permissions;
  gboolean make_backup;
} SftpHandle;


typedef struct {
  ReplyCallback callback;
  GVfsJob *job;
  gpointer user_data;
} ExpectedReply;

typedef struct {
  GVfsBackendSftp *op_backend;

  GOutputStream *command_stream;
  GInputStream *reply_stream;
  GDataInputStream *error_stream;

  GCancellable *reply_stream_cancellable;

  /* Output Queue */

  gsize command_bytes_written;
  GList *command_queue;

  /* Reply reading: */
  GHashTable *expected_replies;
  guint32 reply_size;
  guint32 reply_size_read;
  guint8 *reply;
} Connection;

struct _GVfsBackendSftp
{
  GVfsBackend parent_instance;

  SFTPClientVendor client_vendor;
  char *host;
  int port;
  gboolean user_specified;
  gboolean user_specified_in_uri;
  char *user;
  char *tmp_password;
  GPasswordSave password_save;

  guint32 my_uid;
  guint32 my_gid;
  
  int protocol_version;
  SFTPServerExtensions extensions;

  guint32 current_id;

  Connection command_connection;
  Connection data_connection;

  gboolean force_unmounted;
};

static void parse_attributes (GVfsBackendSftp *backend,
                              GFileInfo *info,
                              const char *basename,
                              GDataInputStream *reply,
                              GFileAttributeMatcher *attribute_matcher);

G_DEFINE_TYPE (GVfsBackendSftp, g_vfs_backend_sftp, G_VFS_TYPE_BACKEND)

static void
data_buffer_free (DataBuffer *buffer)
{
  if (buffer)
    {
      g_free (buffer->data);
      g_slice_free (DataBuffer, buffer);
    }
}

static void
make_fd_nonblocking (int fd)
{
  fcntl (fd, F_SETFL, O_NONBLOCK | fcntl (fd, F_GETFL));
}

static SFTPClientVendor
get_sftp_client_vendor (void)
{
  char *ssh_stderr;
  char *args[3];
  gint ssh_exitcode;
  SFTPClientVendor res = SFTP_VENDOR_INVALID;
  
  args[0] = g_strdup (SSH_PROGRAM);
  args[1] = g_strdup ("-V");
  args[2] = NULL;
  if (g_spawn_sync (NULL, args, NULL,
		    G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
		    NULL, NULL,
		    NULL, &ssh_stderr,
		    &ssh_exitcode, NULL))
    {
      if (ssh_stderr == NULL)
	res = SFTP_VENDOR_INVALID;
      else if ((strstr (ssh_stderr, "OpenSSH") != NULL) ||
	       (strstr (ssh_stderr, "Sun_SSH") != NULL))
	res = SFTP_VENDOR_OPENSSH;
      else if (strstr (ssh_stderr, "SSH Secure Shell") != NULL)
	res = SFTP_VENDOR_SSH;
      else
	res = SFTP_VENDOR_INVALID;
    }
  
  g_free (ssh_stderr);
  g_free (args[0]);
  g_free (args[1]);
  
  return res;
}

static gboolean
has_extension (GVfsBackendSftp *backend, SFTPServerExtensions extension)
{
  return (backend->extensions & (1 << extension)) != 0;
}

static void
destroy_connection (Connection *conn)
{
  if (conn->expected_replies)
    {
      g_hash_table_destroy (conn->expected_replies);
      conn->expected_replies = NULL;
    }

  g_clear_object (&conn->command_stream);
  g_clear_object (&conn->reply_stream_cancellable);
  g_clear_object (&conn->reply_stream);
  g_clear_object (&conn->error_stream);
}

static gboolean
connection_is_usable (Connection *conn)
{
  return conn->command_stream != NULL;
}

static void
g_vfs_backend_sftp_finalize (GObject *object)
{
  GVfsBackendSftp *backend;

  backend = G_VFS_BACKEND_SFTP (object);
  destroy_connection (&backend->command_connection);
  destroy_connection (&backend->data_connection);

  if (G_OBJECT_CLASS (g_vfs_backend_sftp_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_sftp_parent_class)->finalize) (object);
}

static void
expected_reply_free (ExpectedReply *reply)
{
  g_object_unref (reply->job);
  g_slice_free (ExpectedReply, reply);
}

static void
g_vfs_backend_sftp_init (GVfsBackendSftp *backend)
{
}

static void
look_for_stderr_errors (Connection *conn, GError **error)
{
  char *line;

  while (1)
    {
      line = g_data_input_stream_read_line (conn->error_stream, NULL, NULL, NULL);
      
      if (line == NULL)
        {
          /* Error (real or WOULDBLOCK) or EOF */
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Connection failed"));
          return;
        }

      g_debug ("stderr: %s\n", line);
      if (strstr (line, "Permission denied") != NULL)
        {
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        	               _("Permission denied"));
          return;
        }
      else if (strstr (line, "Name or service not known") != NULL)
        {
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND,
        	               _("Hostname not known"));
          return;
        }
      else if (strstr (line, "No route to host") != NULL)
        {
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND,
        	               _("No route to host"));
          return;
        }
      else if (strstr (line, "Connection refused") != NULL ||
               strstr (line, "subsystem request failed") != NULL)
        {
          g_set_error_literal (error,
                               G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
        	               _("Connection refused by server"));
          return;
        }
      else if (strstr (line, "Host key verification failed") != NULL) 
        {
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_FAILED,
        	               _("Host key verification failed"));
          return;
        }
      else if (strstr (line, "Too many authentication failures") != NULL)
        {
          g_set_error_literal (error,
                               G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Too many authentication failures"));
          return;
        }
      
      g_free (line);
    }
}

static gchar*
read_dbus_string_dict_value (GVariant *args, const gchar *key)
{
  GVariant *a;
  GVariantIter iter;
  const gchar *str, *val;
  gchar *res;
  
  if (! g_variant_is_of_type (args, G_VARIANT_TYPE ("(a{ss})")))
    return NULL;
  
  g_variant_get (args, "(@a{ss})", &a);
  
  res = NULL;
  g_variant_iter_init (&iter, a);
  while (g_variant_iter_next (&iter, "{&s&s}", &str, &val))
    {
      if (g_strcmp0 (str, key) == 0)
        {
          res = g_strdup (val);
          break;
        }
    }
  
  g_variant_unref (a);
  
  return res;
}

static void
setup_ssh_environment (void)
{
  GDBusConnection *conn;
  GError *error;
  GVariant *iter;
  gchar *env;
  
  error = NULL;
  conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (! conn)
    {
      g_warning ("Failed to setup SSH evironment: %s (%s, %d)",
                 error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      return;
    }

  iter = g_dbus_connection_call_sync (conn,
                                      "org.gnome.keyring",
                                      "/org/gnome/keyring/daemon",
                                      "org.gnome.keyring.Daemon",
                                      "GetEnvironment",
                                      NULL,
                                      NULL,
                                      G_DBUS_CALL_FLAGS_NONE,
                                      -1,
                                      NULL,
                                      &error);
  if (! iter)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Failed to setup SSH evironment: %s (%s, %d)",
                 error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
  else
    {
      env = read_dbus_string_dict_value (iter, "SSH_AUTH_SOCK");
      if (env && env[0])
        g_setenv ("SSH_AUTH_SOCK", env, TRUE);
      g_free (env);
      g_variant_unref (iter);
    }
  
  g_object_unref (conn);
}

static char **
setup_ssh_commandline (GVfsBackend *backend, const gchar *control_path)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  guint last_arg;
  gchar **args;

  args = g_new0 (gchar *, 20); /* 20 is enought for now, bump size if code below changes */

  /* Fill in the first few args */
  last_arg = 0;
  args[last_arg++] = g_strdup (SSH_PROGRAM);

  if (op_backend->client_vendor == SFTP_VENDOR_OPENSSH)
    {
      args[last_arg++] = g_strdup ("-oForwardX11 no");
      args[last_arg++] = g_strdup ("-oForwardAgent no");
      args[last_arg++] = g_strdup ("-oPermitLocalCommand no");
      args[last_arg++] = g_strdup ("-oClearAllForwardings yes");
      args[last_arg++] = g_strdup ("-oProtocol 2");
      args[last_arg++] = g_strdup ("-oNoHostAuthenticationForLocalhost yes");
#ifndef USE_PTY
      args[last_arg++] = g_strdup ("-oBatchMode yes");
#endif
      args[last_arg++] = g_strdup ("-oControlMaster auto");
      args[last_arg++] = g_strdup_printf ("-oControlPath=%s/%%C", control_path);
    }
  else if (op_backend->client_vendor == SFTP_VENDOR_SSH)
    args[last_arg++] = g_strdup ("-x");

  if (op_backend->port != -1)
    {
      args[last_arg++] = g_strdup ("-p");
      args[last_arg++] = g_strdup_printf ("%d", op_backend->port);
    }
    

  if (op_backend->user_specified)
    {
      args[last_arg++] = g_strdup ("-l");
      args[last_arg++] = g_strdup (op_backend->user);
    }

  args[last_arg++] = g_strdup ("-s");

  if (op_backend->client_vendor == SFTP_VENDOR_SSH)
    {
      args[last_arg++] = g_strdup ("sftp");
      args[last_arg++] = g_strdup (op_backend->host);
    }
  else
    {
      args[last_arg++] = g_strdup (op_backend->host);
      args[last_arg++] = g_strdup ("sftp");
    }

  args[last_arg++] = NULL;

  return args;
}

static gboolean
spawn_ssh (GVfsBackend *backend,
           char *args[],
           pid_t *pid,
           int *tty_fd,
           int *stdin_fd,
           int *stdout_fd,
           int *stderr_fd,
           int *slave_fd,
           GError **error)
{
  if (gvfs_get_debug ())
    {
      const char **arg;
      GString *cmd;

      cmd = g_string_new (NULL);
      for (arg = (const char **)args; *arg != NULL; arg++)
        {
          g_string_append (cmd, *arg);
          g_string_append (cmd, " ");
        }

      g_debug ("spawn_ssh: %s\n", cmd->str);
      g_string_free (cmd, TRUE);
    }

#ifdef USE_PTY
  *tty_fd = pty_open(pid, PTY_REAP_CHILD, NULL,
		     args[0], args, NULL,
		     300, 300, 
		     stdin_fd, stdout_fd, stderr_fd, slave_fd);
  if (*tty_fd == -1)
    {
      g_set_error_literal (error,
			   G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Unable to spawn SSH program"));
      return FALSE;
    }
#else
  GError *my_error;
  GPid gpid;
  
  *tty_fd = -1;

  my_error = NULL;
  if (!g_spawn_async_with_pipes (NULL, args, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
				 &gpid,
				 stdin_fd, stdout_fd, stderr_fd, &my_error))
    {
      g_set_error (error,
                   G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Unable to spawn SSH program: %s"), my_error->message);
      g_error_free (my_error);
      return FALSE;
    }
  *pid = gpid;
#endif
  
  return TRUE;
}

static guint32
get_new_id (GVfsBackendSftp *backend)
{
  return backend->current_id++;
}

static GDataOutputStream *
new_command_stream (GVfsBackendSftp *backend, int type)
{
  GOutputStream *mem_stream;
  GDataOutputStream *data_stream;
  guint32 id;

  mem_stream = g_memory_output_stream_new (NULL, 0, (GReallocFunc)g_realloc, NULL);
  data_stream = g_data_output_stream_new (mem_stream);
  g_object_unref (mem_stream);

  g_data_output_stream_put_int32 (data_stream, 0, NULL, NULL); /* LEN */
  g_data_output_stream_put_byte (data_stream, type, NULL, NULL);
  if (type != SSH_FXP_INIT)
    {
      id = get_new_id (backend);
      g_data_output_stream_put_uint32 (data_stream, id, NULL, NULL);
      g_object_set_qdata (G_OBJECT (data_stream), id_q, GUINT_TO_POINTER (id));
    }
  
  return data_stream;
}

static gpointer
get_data_from_command_stream (GDataOutputStream *command_stream, gsize *len)
{
  GOutputStream *mem_stream;
  gpointer data;
  guint32 *len_ptr;
  
  mem_stream = g_filter_output_stream_get_base_stream (G_FILTER_OUTPUT_STREAM (command_stream));
  *len = g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (mem_stream));
  data = g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (mem_stream));

  len_ptr = (guint32 *)data;
  *len_ptr = GUINT32_TO_BE (*len - 4);
  
  return data;
}

static gboolean
send_command_sync_and_unref_command (Connection *conn,
                                     GDataOutputStream *command_stream,
                                     GCancellable *cancellable,
                                     GError **error)
{
  gpointer data;
  gsize len;
  gsize bytes_written;
  gboolean res;
  
  data = get_data_from_command_stream (command_stream, &len);

  res = g_output_stream_write_all (conn->command_stream,
                                   data, len,
                                   &bytes_written,
                                   cancellable, error);
  
  if (error == NULL && !res)
    g_warning ("Ignored send_command error\n");

  g_free (data);
  g_object_unref (command_stream);

  return res;
}

static gboolean
wait_for_reply (GVfsBackend *backend, int stdout_fd, GError **error)
{
  fd_set ifds;
  struct timeval tv;
  int ret;
  
  FD_ZERO (&ifds);
  FD_SET (stdout_fd, &ifds);
  
  tv.tv_sec = SFTP_READ_TIMEOUT;
  tv.tv_usec = 0;
      
  ret = select (stdout_fd+1, &ifds, NULL, NULL, &tv);

  if (ret <= 0)
    {
      g_set_error_literal (error,
			   G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
			   _("Timed out when logging in"));
      return FALSE;
    }
  return TRUE;
}

static GDataInputStream *
make_reply_stream (guint8 *data, gsize len)
{
  GInputStream *mem_stream;
  GDataInputStream *data_stream;
  
  mem_stream = g_memory_input_stream_new_from_data (data, len, g_free);
  data_stream = g_data_input_stream_new (mem_stream);
  g_object_unref (mem_stream);
  
  return data_stream;
}

static GDataInputStream *
read_reply_sync (Connection *conn, gsize *len_out, GError **error)
{
  guint32 len;
  gsize bytes_read;
  GByteArray *array;
  guint8 *data;
  
  if (!g_input_stream_read_all (conn->reply_stream,
				&len, 4,
				&bytes_read, NULL, error))
    return NULL;

  /* Make sure we handle SSH exiting early, e.g. if no further
     authentication methods */
  if (bytes_read == 0)
    {
      g_set_error_literal (error,
			   G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Connection failed"));
      return NULL;
    }
  
  len = GUINT32_FROM_BE (len);
  
  array = g_byte_array_sized_new (len);

  if (!g_input_stream_read_all (conn->reply_stream,
				array->data, len,
				&bytes_read, NULL, error))
    {
      g_byte_array_free (array, TRUE);
      return NULL;
    }

  if (len_out)
    *len_out = len;

  data = array->data;
  g_byte_array_free (array, FALSE);
  
  return make_reply_stream (data, len);
}

static void
put_string (GDataOutputStream *stream, const char *str)
{
  g_data_output_stream_put_uint32 (stream, strlen (str), NULL, NULL);
  g_data_output_stream_put_string (stream, str, NULL, NULL);
}

static void
put_data_buffer (GDataOutputStream *stream, DataBuffer *buffer)
{
  g_data_output_stream_put_uint32 (stream, buffer->size, NULL, NULL);
  g_output_stream_write_all (G_OUTPUT_STREAM (stream),
                             buffer->data, buffer->size,
                             NULL,
                             NULL, NULL);
}

static char *
read_string (GDataInputStream *stream, gsize *len_out)
{
  guint32 len;
  char *data;
  GError *error;

  error = NULL;
  len = g_data_input_stream_read_uint32 (stream, NULL, &error);
  if (error)
    {
      g_error_free (error);
      return NULL;
    }
  
  data = g_malloc (len + 1);

  if (!g_input_stream_read_all (G_INPUT_STREAM (stream), data, len, NULL, NULL, NULL))
    {
      g_free (data);
      return NULL;
    }
  
  data[len] = 0;

  if (len_out)
    *len_out = len;
  
  return data;
}

static DataBuffer *
read_data_buffer (GDataInputStream *stream)
{
  DataBuffer *buffer;

  buffer = g_slice_new (DataBuffer);
  buffer->data = (guchar *)read_string (stream, &buffer->size);
  
  return buffer;
}

static gboolean
get_hostname_from_line (const gchar *buffer,
                        gchar **hostname_out)
{
  gchar *startpos;
  gchar *endpos;

  /* Parse a line that looks like: "username@hostname's password:". */

  startpos = strchr (buffer, '@');
  if (!startpos)
    return FALSE;

  endpos = strchr (buffer, '\'');
  if (!endpos)
    return FALSE;

  *hostname_out = g_strndup (startpos + 1, endpos - startpos - 1);

  return TRUE;
}

static gboolean
get_hostname_and_fingerprint_from_line (const gchar *buffer,
                                        gchar      **hostname_out,
                                        gchar      **fingerprint_out)
{
  gchar *pos;
  gchar *startpos;
  gchar *endpos;
  gchar *hostname = NULL;
  gchar *fingerprint = NULL;
  
  if (g_str_has_prefix (buffer, "The authenticity of host '"))
    {
      /* OpenSSH */
      pos = strchr (&buffer[26], '\'');
      if (pos == NULL)
        return FALSE;

      hostname = g_strndup (&buffer[26], pos - (&buffer[26]));

      startpos = strstr (pos, " key fingerprint is ");
      if (startpos == NULL)
        {
          g_free (hostname);
          return FALSE;
        }

      startpos = startpos + 20;
      endpos = strchr (startpos, '.');
      if (endpos == NULL)
        {
          g_free (hostname);
          return FALSE;
        }
      
      fingerprint = g_strndup (startpos, endpos - startpos);
    }
  else if (strstr (buffer, "Key fingerprint:") != NULL)
    {
      /* SSH.com*/
      startpos = strstr (buffer, "Key fingerprint:");
      if (startpos == NULL)
        {
          g_free (hostname);
          return FALSE;
        }
      
      startpos = startpos + 18;
      endpos = strchr (startpos, '\r');
      fingerprint = g_strndup (startpos, endpos - startpos);
    }

  *hostname_out = hostname;
  *fingerprint_out = fingerprint;

  return TRUE;
}

static gboolean
get_hostname_and_ip_address (const gchar *buffer,
                             gchar      **hostname_out,
                             gchar      **ip_address_out)
{
  char *startpos, *endpos, *hostname;

  /* Parse a line that looks like:
   * Warning: the ECDSA/RSA host key for 'hostname' differs from the key for the IP address '...'
   * First get the hostname.
   */
  startpos = strchr (buffer, '\'');
  if (!startpos)
    return FALSE;
  startpos++;

  endpos = strchr (startpos, '\'');
  if (!endpos)
    return FALSE;

  hostname = g_strndup (startpos, endpos - startpos);

  /* Then get the ip address. */
  startpos = strchr (endpos + 1, '\'');
  if (!startpos)
    {
      g_free (hostname);
      return FALSE;
    }
  startpos++;

  endpos = strchr (startpos, '\'');
  if (!endpos)
    {
      g_free (hostname);
      return FALSE;
    }

  *hostname_out = hostname;
  *ip_address_out = g_strndup (startpos, endpos - startpos);

  return TRUE;
}

static gboolean
login_answer_yes_no (GMountSource *mount_source,
                     char *message,
                     GOutputStream *reply_stream,
                     GError **error)
{
  const char *choices[] = {_("Log In Anyway"), _("Cancel Login"), NULL};
  const char *choice_string;
  int choice;
  gboolean aborted = FALSE;
  gsize bytes_written;

  if (!g_mount_source_ask_question (mount_source,
                                    message,
                                    choices,
                                    &aborted,
                                    &choice) ||
      aborted)
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Login dialog cancelled"));
      g_free (message);
      return FALSE;
    }
  g_free (message);

  choice_string = (choice == 0) ? "yes" : "no";
  if (!g_output_stream_write_all (reply_stream,
                                  choice_string,
                                  strlen (choice_string),
                                  &bytes_written,
                                  NULL, NULL) ||
      !g_output_stream_write_all (reply_stream,
                                  "\n", 1,
                                  &bytes_written,
                                  NULL, NULL))
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Can’t send host identity confirmation"));
      return FALSE;
    }

  return TRUE;
}

static const gchar *
get_authtype_from_password_line (const char *password_line)
{
  return g_str_has_prefix (password_line, "Enter passphrase for key") ?
	  "publickey" : "password";
}

static char *
get_object_from_password_line (const char *password_line)
{
  char *chr, *ptr, *object = NULL;

  if (g_str_has_prefix (password_line, "Enter passphrase for key"))
    {
      ptr = strchr (password_line, '\'');
      if (ptr != NULL)
        {
	  ptr += 1;
	  chr = strchr (ptr, '\'');
	  if (chr != NULL)
	    {
	      object = g_strndup (ptr, chr - ptr);
	    }
	  else
	    {
	      object = g_strdup (ptr);
	    }
	}
    }
  return object;
}

static gboolean
handle_login (GVfsBackend *backend,
              GMountSource *mount_source,
              int tty_fd, int stdout_fd, int stderr_fd,
              gboolean initial_connection,
              GError **error)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GInputStream *prompt_stream;
  GOutputStream *reply_stream;
  int ret;
  int prompt_fd;
  struct pollfd fds[2];
  char buffer[1024];
  gsize len;
  gboolean ret_val;
  char *new_password = NULL;
  char *new_user = NULL;
  gboolean password_in_keyring = FALSE;
  const gchar *authtype = NULL;
  gchar *object = NULL;
  char *prompt;
  int attempts = 0;
  static int i = 0;

  i++;
  g_debug ("handle_login #%d initial_connection = %d - user: %s, host: %s, port: %d\n",
           i, initial_connection, op_backend->user, op_backend->host, op_backend->port);

  if (op_backend->client_vendor == SFTP_VENDOR_SSH) 
    prompt_fd = stderr_fd;
  else
    prompt_fd = tty_fd;

  prompt_stream = g_unix_input_stream_new (prompt_fd, FALSE);
  reply_stream = g_unix_output_stream_new (tty_fd, FALSE);

  ret_val = TRUE;
  while (1)
    {
      fds[0].fd = stdout_fd;
      fds[0].events = POLLIN;
      fds[1].fd = prompt_fd;
      fds[1].events = POLLIN;
      
      ret = poll(fds, 2, SFTP_READ_TIMEOUT * 1000);
      
      if (ret <= 0)
        {
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
        	               _("Timed out when logging in"));
          ret_val = FALSE;
          break;
        }
      
      if (fds[0].revents)
        break; /* Got reply to initial INIT request */
      
      if (!(fds[1].revents & POLLIN))
        continue;
      
      len = g_input_stream_read (prompt_stream,
                                 buffer, sizeof (buffer) - 1,
                                 NULL, error);
      
      if (len == -1)
        {
          ret_val = FALSE;
          break;
        }
      
      buffer[len] = 0;
      g_strchug (buffer);

      g_debug ("handle_login #%d - prompt: \"%s\"\n", i, buffer);

      /*
       * If logging in on a second connection (e.g. the data connection), use
       * the user and password stored in the backend and don't retry if it
       * fails.
       *
       * If the input URI contains a username
       *     if the input URI contains a password, we attempt one login and return GNOME_VFS_ERROR_ACCESS_DENIED on failure.
       *     if the input URI contains no password, we query the user until he provides a correct one, or he cancels.
       *
       * If the input URI contains no username
       *     (a) the user is queried for a user name and a password, with the default login being his
       *     local login name.
       *
       *     (b) if the user decides to change his remote login name, we go to tty_retry because we need a
       *     new SSH session, attempting one login with his provided credentials, and if that fails proceed
       *     with (a), but use his desired remote login name as default.
       *
       * The "password" variable is only used for the very first login attempt,
       * or for the first re-login attempt when the user decided to change his name.
       * Otherwise, we "new_password" and "new_user_name" is used, as output variable
       * for user and keyring input.
       */
      if (g_str_has_suffix (buffer, "password: ") ||
          g_str_has_suffix (buffer, "Password: ") ||
          g_str_has_suffix (buffer, "Password:")  ||
          strstr (buffer, "Password for ") ||
          strstr (buffer, "Enter Kerberos password") ||
          strstr (buffer, "Enter passphrase for key") ||
          strstr (buffer, "Enter PASSCODE"))
        {
          gboolean aborted = FALSE;
          gsize bytes_written;

          attempts++;
	  authtype = get_authtype_from_password_line (buffer);
	  object = get_object_from_password_line (buffer);

          if (!initial_connection && attempts > 1)
            {
	      g_set_error_literal (error,
				   G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                   _("Permission denied"));
	      ret_val = FALSE;
	      break;
            }

          /* If password is in keyring at this point is because it failed */
	  if (!op_backend->tmp_password && (password_in_keyring ||
              !g_vfs_keyring_lookup_password (op_backend->user,
                                              op_backend->host,
                                              NULL,
                                              "sftp",
					      object,
					      authtype,
					      op_backend->port != -1 ?
					      op_backend->port
					      :
					      0,
                                              &new_user,
                                              NULL,
                                              &new_password)))
            {
              GAskPasswordFlags flags = G_ASK_PASSWORD_NEED_PASSWORD;
              gchar *hostname = NULL;
              
              g_debug ("handle_login #%d - asking for password...\n", i);

              if (g_vfs_keyring_is_available ())
                flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;
	      if (strcmp (authtype, "password") == 0 &&
		  !op_backend->user_specified)
	        flags |= G_ASK_PASSWORD_NEED_USERNAME;

              g_free (new_password);
              
              get_hostname_from_line (buffer, &hostname);

              if (op_backend->user_specified)
                if (strcmp (authtype, "publickey") == 0)
                  /* Translators: the first %s is the username, the second the host name */
                  prompt = g_strdup_printf (_("Authentication Required\nEnter passphrase for secure key for “%s” on “%s”:"), op_backend->user, op_backend->host);
                else
                  /* Translators: the first %s is the username, the second the host name */
                  prompt = g_strdup_printf (_("Authentication Required\nEnter password for “%s” on “%s”:"), op_backend->user, hostname ? hostname : op_backend->host);
              else
                if (strcmp (authtype, "publickey") == 0)
                  /* Translators: %s is the hostname */
                  prompt = g_strdup_printf (_("Authentication Required\nEnter passphrase for secure key for “%s”:"), op_backend->host);
                else
                  /* Translators: %s is the hostname */
                  prompt = g_strdup_printf (_("Authentication Required\nEnter user and password for “%s”:"), hostname ? hostname : op_backend->host);

              if (!g_mount_source_ask_password (mount_source,
                                                prompt,
                                                op_backend->user,
                                                NULL,
                                                flags,
                                                &aborted,
                                                &new_password,
                                                &new_user,
                                                NULL,
						NULL,
                                                &op_backend->password_save) ||
                  aborted)
                {
                  g_set_error_literal (error, G_IO_ERROR,
                                       aborted ? G_IO_ERROR_FAILED_HANDLED : G_IO_ERROR_PERMISSION_DENIED,
        	                       _("Password dialog cancelled"));
                  ret_val = FALSE;
                  break;
                }
                g_free (prompt);
            }
	  else if (op_backend->tmp_password)
	    {
              /* I already have a password of a previous login attempt
               * (either because this is a second connection or because the
               * user provided a new user name).
	       */
	      new_password = op_backend->tmp_password;
	      op_backend->tmp_password = NULL;

              g_debug ("handle_login #%d - using credentials from previous login attempt...\n", i);
	    }
          else
            {
              password_in_keyring = TRUE;

              g_debug ("handle_login #%d - using credentials from keyring...\n", i);
            }

	  if (new_user &&
	      (op_backend->user == NULL ||
	       strcmp (new_user, op_backend->user) != 0))
	    {
              g_debug ("handle_login #%d - new_user: %s\n", i, new_user);

	      g_free (op_backend->user);
	      op_backend->user = new_user;

	      op_backend->user_specified = TRUE;
	      
	      g_free (op_backend->tmp_password);
	      op_backend->tmp_password = new_password;
	      new_password = NULL;
	      
	      g_set_error_literal (error,
				   G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				   "Invalid user name");
	      ret_val = FALSE;
	      break;
	    }
	  else if (new_user)
	    {
	      g_free (new_user);
	    }

	  if (new_password == NULL)
	    {
	      /* This really should not happen, but was seen as bug #569203
	       * avoid crash and ask for info
	       */
	      g_warning ("Got NULL password but no error in sftp login request. "
			 "This should not happen, if you can reproduce this, please "
			 "add information to http://bugzilla.gnome.org/show_bug.cgi?id=569203");
	      new_password = g_strdup ("");
	    }
	  
          if (!g_output_stream_write_all (reply_stream,
                                          new_password, strlen (new_password),
                                          &bytes_written,
                                          NULL, NULL) ||
              !g_output_stream_write_all (reply_stream,
                                          "\n", 1,
                                          &bytes_written,
                                          NULL, NULL))
            {
              g_set_error_literal (error,
	                           G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        	                   _("Can’t send password"));
              ret_val = FALSE;
              break;
            }
        }
      else if (strstr (buffer, "Verification code") ||
               strstr (buffer, "One-time password"))
        {
          gchar *verification_code = NULL;
          gboolean aborted = FALSE;

          g_debug ("handle_login #%d - asking for verification code...\n", i);

          if (op_backend->user_specified)
            /* Translators: the first %s is the username, the second the host name */
            prompt = g_strdup_printf (_("Enter verification code for %s on %s"),
                                      op_backend->user, op_backend->host);
          else
            /* Translators: %s is the hostname */
            prompt = g_strdup_printf (_("Enter verification code for %s"),
                                      op_backend->host);

          if (!g_mount_source_ask_password (mount_source, prompt,
                                            op_backend->user, NULL,
                                            G_ASK_PASSWORD_NEED_PASSWORD,
                                            &aborted, &verification_code,
                                            NULL, NULL, NULL, NULL) ||
              aborted)
            {
              g_set_error_literal (error, G_IO_ERROR, aborted ?
                                   G_IO_ERROR_FAILED_HANDLED :
                                   G_IO_ERROR_PERMISSION_DENIED,
                                   _("Password dialog cancelled"));
              ret_val = FALSE;
              break;
            }
          g_free (prompt);

          if (!g_output_stream_write_all (reply_stream, verification_code,
                                          strlen (verification_code), NULL,
                                          NULL, NULL) ||
              !g_output_stream_write_all (reply_stream, "\n", 1, NULL, NULL,
                                          NULL))
            {
              g_free (verification_code);
              g_set_error_literal (error,
                                   G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                   _("Can’t send password"));
              ret_val = FALSE;
              break;
            }

          g_free (verification_code);
        }
      else if (g_str_has_prefix (buffer, "The authenticity of host '") ||
               strstr (buffer, "Key fingerprint:") != NULL)
        {
	  gchar *hostname = NULL;
	  gchar *fingerprint = NULL;
	  gchar *message;

          g_debug ("handle_login #%d - confirming authenticity of host...\n", i);

	  get_hostname_and_fingerprint_from_line (buffer, &hostname, &fingerprint);

          /* Translators: the first %s is the hostname, the second the key fingerprint */
	  message = g_strdup_printf (_("Identity Verification Failed\n"
				       "Verifying the identity of “%s” failed, this happens when "
				       "you log in to a computer the first time.\n\n"
				       "The identity sent by the remote computer is “%s”. "
				       "If you want to be absolutely sure it is safe to continue, "
				       "contact the system administrator."),
                                     hostname ? hostname : op_backend->host,
                                     fingerprint ? fingerprint : "???");

	  g_free (hostname);
	  g_free (fingerprint);

          if (!login_answer_yes_no (mount_source, message, reply_stream, error))
            {
              ret_val = FALSE;
              break;
            }
	}
      else if (strstr (buffer, "differs from the key for the IP address"))
        {
          gchar *hostname = NULL;
          gchar *ip_address = NULL;
          gchar *message;

          g_debug ("handle_login #%d - host key / IP mismatch ...\n", i);

          get_hostname_and_ip_address (buffer, &hostname, &ip_address);

          /* Translators: the first %s is the hostname, the second is an ip address */
          message = g_strdup_printf (_("Identity Verification Failed\n"
                                       "The host key for “%s” differs from the key for the IP address “%s”\n"
                                       "If you want to be absolutely sure it is safe to continue, "
                                       "contact the system administrator."),
                                     hostname ? hostname : op_backend->host,
                                     ip_address ? ip_address : "???");

          g_free (hostname);
          g_free (ip_address);

          if (!login_answer_yes_no (mount_source, message, reply_stream, error))
            {
              ret_val = FALSE;
              break;
            }
        }
    }
  
  if (ret_val && initial_connection)
    {
      g_debug ("handle_login #%d - password_save: %d\n", i, op_backend->password_save);

      /* Login succeed, save password in keyring */
      g_vfs_keyring_save_password (op_backend->user,
                                   op_backend->host,
                                   NULL,
                                   "sftp",
				   object,
				   authtype,
				   op_backend->port != -1 ?
				   op_backend->port
				   :
				   0, 
                                   new_password,
                                   op_backend->password_save);

      /* Keep the successful password for subsequent connections. */
      op_backend->tmp_password = new_password;
      new_password = NULL;
    }

  g_debug ("handle_login #%d - ret_val: %d\n", i, ret_val);

  g_free (object);
  g_free (new_password);
  g_object_unref (prompt_stream);
  g_object_unref (reply_stream);
  return ret_val;
}

static void
fail_jobs (Connection *conn, GError *error)
{
  GHashTableIter iter;
  gpointer key, value;

  if (!connection_is_usable (conn))
    return;

  g_hash_table_iter_init (&iter, conn->expected_replies);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      ExpectedReply *expected_reply = (ExpectedReply *) value;
      g_vfs_job_failed_from_error (expected_reply->job, error);
    }
}

static void
fail_jobs_and_unmount (GVfsBackendSftp *backend, GError *error)
{
  if (backend->force_unmounted)
    return;

  backend->force_unmounted = TRUE;

  fail_jobs (&backend->command_connection, error);
  fail_jobs (&backend->data_connection, error);

  g_error_free (error);

  g_vfs_backend_force_unmount ((GVfsBackend*)backend);
}

static int
check_input_stream_read_result (Connection *conn, gssize res, GError *error)
{
  if (G_UNLIKELY (res <= 0))
    {
      if (res == 0 || error == NULL)
        {
          g_clear_error (&error);
          g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       res == 0 ? _("The connection is closed (the underlying SSH process exited)")
                                : _("Internal error: Unknown Error"));
        }

      fail_jobs_and_unmount (conn->op_backend, error);
      return -1;
    }

  return 0;
}

static void read_reply_async (Connection *conn);

static void
read_reply_async_got_data  (GObject *source_object,
                            GAsyncResult *result,
                            gpointer user_data)
{
  Connection *conn = user_data;
  gssize res;
  GDataInputStream *reply;
  ExpectedReply *expected_reply;
  guint32 id;
  int type;
  GError *error;

  error = NULL;
  res = g_input_stream_read_finish (G_INPUT_STREAM (source_object), result, &error);

  /* If we got an error, we've already called force_unmount so don't do
   * anything further. */
  if (check_input_stream_read_result (conn, res, error) == -1)
    return;

  conn->reply_size_read += res;

  if (conn->reply_size_read < conn->reply_size)
    {
      g_input_stream_read_async (conn->reply_stream,
				 conn->reply + conn->reply_size_read, conn->reply_size - conn->reply_size_read,
				 0, NULL, read_reply_async_got_data, conn);
      return;
    }

  reply = make_reply_stream (conn->reply, conn->reply_size);
  conn->reply = NULL;

  type = g_data_input_stream_read_byte (reply, NULL, NULL);
  id = g_data_input_stream_read_uint32 (reply, NULL, NULL);

  expected_reply = g_hash_table_lookup (conn->expected_replies, GINT_TO_POINTER (id));
  if (expected_reply)
    {
      if (expected_reply->callback != NULL)
        (expected_reply->callback) (conn->op_backend, type, reply, conn->reply_size,
                                    expected_reply->job, expected_reply->user_data);
      g_hash_table_remove (conn->expected_replies, GINT_TO_POINTER (id));
    }
  else
    g_warning ("Got unhandled reply of size %"G_GUINT32_FORMAT" for id %"G_GUINT32_FORMAT"\n", conn->reply_size, id);

  g_object_unref (reply);

  read_reply_async (conn);
  
}

static void
read_reply_async_got_len  (GObject *source_object,
                           GAsyncResult *result,
                           gpointer user_data)
{
  Connection *conn = user_data;
  gssize res;
  GError *error;

  error = NULL;
  res = g_input_stream_read_finish (G_INPUT_STREAM (source_object), result, &error);

  /* Bail out if cancelled */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_error_free (error);
      g_object_unref (conn->op_backend);
      return;
    }

  /* If we got an error, we've already called force_unmount so don't do
   * anything further. */
  if (check_input_stream_read_result (conn, res, error) == -1)
    return;

  conn->reply_size_read += res;

  if (conn->reply_size_read < 4)
    {
      g_input_stream_read_async (conn->reply_stream,
				 (char *)&conn->reply_size + conn->reply_size_read, 4 - conn->reply_size_read,
				 0, conn->reply_stream_cancellable, read_reply_async_got_len,
				 conn);
      return;
    }
  conn->reply_size = GUINT32_FROM_BE (conn->reply_size);

  conn->reply_size_read = 0;
  conn->reply = g_malloc (conn->reply_size);
  g_input_stream_read_async (conn->reply_stream,
			     conn->reply, conn->reply_size,
			     0, NULL, read_reply_async_got_data, conn);
}

static void
read_reply_async (Connection *conn)
{
  conn->reply_size_read = 0;
  g_input_stream_read_async (conn->reply_stream,
                             &conn->reply_size, 4,
                             0, conn->reply_stream_cancellable,
                             read_reply_async_got_len,
                             conn);
}

static void send_command (Connection *conn);

static void
send_command_data (GObject *source_object,
                   GAsyncResult *result,
                   gpointer user_data)
{
  Connection *conn = user_data;
  gssize res;
  DataBuffer *buffer;

  res = g_output_stream_write_finish (G_OUTPUT_STREAM (source_object), result, NULL);

  if (res <= 0)
    {
      g_warning ("Error sending command");
      fail_jobs_and_unmount (conn->op_backend, NULL);
      return;
    }

  buffer = conn->command_queue->data;
  
  conn->command_bytes_written += res;

  if (conn->command_bytes_written < buffer->size)
    {
      g_output_stream_write_async (conn->command_stream,
                                   buffer->data + conn->command_bytes_written,
                                   buffer->size - conn->command_bytes_written,
                                   0,
                                   NULL,
                                   send_command_data,
                                   conn);
      return;
    }

  data_buffer_free (buffer);

  conn->command_queue = g_list_delete_link (conn->command_queue, conn->command_queue);

  if (conn->command_queue != NULL)
    send_command (conn);
}

static void
send_command (Connection *conn)
{
  DataBuffer *buffer;

  buffer = conn->command_queue->data;
  
  conn->command_bytes_written = 0;
  g_output_stream_write_async (conn->command_stream,
                               buffer->data,
                               buffer->size,
                               0,
                               NULL,
                               send_command_data,
                               conn);
}

static void
expect_reply (Connection *conn,
              guint32 id,
              ReplyCallback callback,
              GVfsJob *job,
              gpointer user_data)
{
  ExpectedReply *expected;

  expected = g_slice_new (ExpectedReply);
  expected->callback = callback;
  expected->job = g_object_ref (job);
  expected->user_data = user_data;

  g_hash_table_replace (conn->expected_replies, GINT_TO_POINTER (id), expected);
}

static DataBuffer *
data_buffer_new (guchar *data, gsize len)
{
  DataBuffer *buffer;
  
  buffer = g_slice_new (DataBuffer);
  buffer->data = data;
  buffer->size = len;

  return buffer;
}

static void
queue_command_buffer (Connection *conn,
                      DataBuffer *buffer)
{
  gboolean first;
  
  first = conn->command_queue == NULL;

  conn->command_queue = g_list_append (conn->command_queue, buffer);
  
  if (first)
    send_command (conn);
}

static void
queue_command_stream_and_free (Connection *conn,
                               GDataOutputStream *command_stream,
                               ReplyCallback callback,
                               GVfsJob *job,
                               gpointer user_data)
{
  gpointer data;
  gsize len;
  DataBuffer *buffer;
  guint32 id;

  id = GPOINTER_TO_UINT (g_object_get_qdata (G_OBJECT (command_stream), id_q));
  data = get_data_from_command_stream (command_stream, &len);
  
  buffer = data_buffer_new (data, len);
  g_object_unref (command_stream);

  expect_reply (conn, id, callback, job, user_data);
  queue_command_buffer (conn, buffer);
}


static void
multi_request_cb (GVfsBackendSftp *backend,
                  int reply_type,
                  GDataInputStream *reply_stream,
                  guint32 len,
                  GVfsJob *job,
                  gpointer user_data)
{
  MultiReply *reply;
  MultiRequest *request;
  int i;

  reply = user_data;
  request = reply->request;

  reply->type = reply_type;
  reply->data = g_object_ref (reply_stream);
  reply->data_len = len;

  if (--request->n_outstanding == 0)
    {
      /* Call callback */
      if (request->callback != NULL)
        (request->callback) (backend,
                             request->replies,
                             request->n_replies,
                             job,
                             request->user_data);

      /* Free request data */
      
      for (i = 0; i < request->n_replies; i++)
        {
          reply = &request->replies[i];
          if (reply->data)
            g_object_unref (reply->data);
        }
      g_free (request->replies);
      
      g_free (request);
      
    }
}

typedef struct
{
  Connection *connection;
  GDataOutputStream *cmd;
} Command;

static void
queue_command_streams_and_free (Command *commands,
                                int n_commands,
                                MultiReplyCallback callback,
                                GVfsJob *job,
                                gpointer user_data)
{
  MultiRequest *data;
  MultiReply *reply;
  
  int i;

  data = g_new0 (MultiRequest, 1);

  data->user_data = user_data;
  data->n_replies = n_commands;
  data->n_outstanding = n_commands;
  data->replies = g_new0 (MultiReply, n_commands);
  data->callback = callback;

  for (i = 0; i < n_commands; i++)
    {
      reply = &data->replies[i];
      reply->request = data;
      queue_command_stream_and_free (commands[i].connection,
                                     commands[i].cmd,
                                     multi_request_cb,
                                     job,
                                     reply);
    }
}

static gboolean
get_uid_sync (GVfsBackendSftp *backend)
{
  GDataOutputStream *command;
  GDataInputStream *reply;
  int type;
  
  command = new_command_stream (backend, SSH_FXP_STAT);
  put_string (command, ".");
  send_command_sync_and_unref_command (&backend->command_connection,
                                       command,
                                       NULL, NULL);

  reply = read_reply_sync (&backend->command_connection, NULL, NULL);
  if (reply == NULL)
    return FALSE;
  
  type = g_data_input_stream_read_byte (reply, NULL, NULL);
  /*id =*/ (void) g_data_input_stream_read_uint32 (reply, NULL, NULL);

  /* On error, set uid to -1 and ignore */
  backend->my_uid = (guint32)-1;
  backend->my_gid = (guint32)-1;
  if (type == SSH_FXP_ATTRS)
    {
      GFileInfo *info;

      info = g_file_info_new ();
      parse_attributes (backend, info, NULL, reply, NULL);
      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_UID))
        {
          /* Both are always set if set */
          backend->my_uid = g_file_info_get_attribute_uint32 (info,
                                                              G_FILE_ATTRIBUTE_UNIX_UID);
          backend->my_gid = g_file_info_get_attribute_uint32 (info,
                                                              G_FILE_ATTRIBUTE_UNIX_GID);
        }

      g_object_unref (info);
    }

  g_object_unref (reply);

  return TRUE;
}

static gboolean
get_home_sync (GVfsBackendSftp *backend)
{
  GDataOutputStream *command;
  GDataInputStream *reply;
  char *home_path;
  int type;

  command = new_command_stream (backend, SSH_FXP_REALPATH);
  put_string (command, ".");
  send_command_sync_and_unref_command (&backend->command_connection,
                                       command,
                                       NULL, NULL);

  reply = read_reply_sync (&backend->command_connection, NULL, NULL);
  if (reply == NULL)
    return FALSE;

  type = g_data_input_stream_read_byte (reply, NULL, NULL);
  /*id =*/ (void) g_data_input_stream_read_uint32 (reply, NULL, NULL);

  /* On error, set home to NULL and ignore */
  if (type == SSH_FXP_NAME)
    {
    /* count = */ (void) g_data_input_stream_read_uint32 (reply, NULL, NULL);

      home_path = read_string (reply, NULL);
      g_vfs_backend_set_default_location (G_VFS_BACKEND (backend), home_path);
      g_free (home_path);
    }

  g_object_unref (reply);

  return TRUE;
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount);

static gboolean
setup_connection (GVfsBackend *backend,
                  GVfsJobMount *job,
                  GMountSpec *mount_spec,
                  GMountSource *mount_source,
                  gboolean is_automount,
                  Connection *connection,
                  gboolean initial_connection,
                  GError **error)
{
  const struct {
    const char *name;               /* extension_name field */
    const char *data;               /* extension_data field */
    SFTPServerExtensions enable;    /* flag to enable this extension */
  } extensions[] = {
    { "statvfs@openssh.com", "2", SFTP_EXT_OPENSSH_STATVFS },
  };

  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  gchar **args;
  pid_t pid;
  int tty_fd, stdout_fd, stdin_fd, stderr_fd, slave_fd;
  GInputStream *is;
  GDataOutputStream *command;
  GDataInputStream *reply;
  gboolean res;
  char *extension_name, *extension_data;
  int i;
  gchar *control_path = NULL;

  control_path = g_build_filename (g_get_user_runtime_dir (), "gvfsd-sftp", NULL);
  g_mkdir (control_path, 0700);

  args = setup_ssh_commandline (backend, control_path);
  g_free (control_path);

  if (!spawn_ssh (backend,
		  args, &pid,
		  &tty_fd, &stdin_fd, &stdout_fd, &stderr_fd, &slave_fd,
		  error))
    {
      g_strfreev (args);
      return FALSE;
    }

  g_strfreev (args);

  connection->op_backend = op_backend;
  connection->command_stream = g_unix_output_stream_new (stdin_fd, TRUE);
  connection->expected_replies = g_hash_table_new_full (NULL, NULL, NULL,
                                                        (GDestroyNotify)expected_reply_free);

  command = new_command_stream (op_backend, SSH_FXP_INIT);
  g_data_output_stream_put_int32 (command,
                                  SSH_FILEXFER_VERSION, NULL, NULL);
  send_command_sync_and_unref_command (connection, command, NULL, NULL);

  if (tty_fd == -1)
    res = wait_for_reply (backend, stdout_fd, error);
  else
    {
      res = handle_login (backend,
                          mount_source,
                          tty_fd, stdout_fd, stderr_fd,
                          initial_connection,
                          error);
      close (slave_fd);
    }

  if (!res)
    {
      if (error && (*error)->code == G_IO_ERROR_INVALID_ARGUMENT)
        {
	  /* New username provided by the user,
	   * we need to re-spawn the ssh command
	   */
	  g_clear_error (error);
	  do_mount (backend, job, mount_spec, mount_source, is_automount);
	}

      return FALSE;
    }

  connection->reply_stream = g_unix_input_stream_new (stdout_fd, TRUE);
  connection->reply_stream_cancellable = g_cancellable_new ();

  make_fd_nonblocking (stderr_fd);
  is = g_unix_input_stream_new (stderr_fd, TRUE);
  connection->error_stream = g_data_input_stream_new (is);
  g_object_unref (is);

  reply = read_reply_sync (connection, NULL, NULL);
  if (reply == NULL)
    {
      look_for_stderr_errors (connection, error);
      return FALSE;
    }

  if (g_data_input_stream_read_byte (reply, NULL, NULL) != SSH_FXP_VERSION)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Protocol error"));
      return FALSE;
    }

  op_backend->protocol_version = g_data_input_stream_read_uint32 (reply, NULL, NULL);

  while ((extension_name = read_string (reply, NULL)) != NULL)
    {
      extension_data = read_string (reply, NULL);
      if (extension_data)
        {
          for (i = 0; i < G_N_ELEMENTS (extensions); i++)
            {
              if (!strcmp (extension_name, extensions[i].name) &&
                  !strcmp (extension_data, extensions[i].data))
                op_backend->extensions |= 1 << extensions[i].enable;
            }
        }
      g_free (extension_name);
      g_free (extension_data);
    }

  g_object_unref (reply);

  if (initial_connection &&
      (!get_uid_sync (op_backend) || !get_home_sync (op_backend)))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Protocol error"));
      return FALSE;
    }

  g_object_ref (op_backend);
  read_reply_async (connection);

  return TRUE;
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GError *error = NULL;
  GMountSpec *sftp_mount_spec;
  char *display_name;

  if (!setup_connection (backend,
                         job,
                         mount_spec,
                         mount_source,
                         is_automount,
                         &op_backend->command_connection,
                         TRUE,
                         &error))
    {
      if (error)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
        }
      /* When a new user is specified, do_mount is called recursively so we
       * need to return here without finishing the job. */
      return;
    }

  if (!setup_connection (backend,
                         job,
                         mount_spec,
                         mount_source,
                         is_automount,
                         &op_backend->data_connection,
                         FALSE,
                         NULL))
    {
      g_warning ("Setting up data connection failed\n");
      destroy_connection (&op_backend->data_connection);
    }

  g_clear_pointer (&op_backend->tmp_password, g_free);

  sftp_mount_spec = g_mount_spec_new ("sftp");
  if (op_backend->user_specified_in_uri)
    g_mount_spec_set (sftp_mount_spec, "user", op_backend->user);
  g_mount_spec_set (sftp_mount_spec, "host", op_backend->host);
  if (op_backend->port != -1)
    {
      char *v;
      v = g_strdup_printf ("%d", op_backend->port);
      g_mount_spec_set (sftp_mount_spec, "port", v);
      g_free (v);
    }

  g_vfs_backend_set_mount_spec (backend, sftp_mount_spec);
  g_mount_spec_unref (sftp_mount_spec);

  if (op_backend->user_specified_in_uri)
    /* Translators: This is the name of an SFTP share, like "<user> on <hostname>" */
    display_name = g_strdup_printf (_("%s on %s"), op_backend->user, op_backend->host);
  else
    display_name = g_strdup (op_backend->host);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  g_vfs_backend_set_icon_name (G_VFS_BACKEND (backend), "folder-remote");
  g_vfs_backend_set_symbolic_icon_name (G_VFS_BACKEND (backend), "folder-remote-symbolic");
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
real_do_mount (GVfsBackend *backend,
	       GVfsJobMount *job,
	       GMountSpec *mount_spec,
	       GMountSource *mount_source,
	       gboolean is_automount)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  setup_ssh_environment ();

  op_backend->password_save = G_PASSWORD_SAVE_NEVER;
  do_mount (backend, job, mount_spec, mount_source, is_automount);
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  const char *user, *host, *port;

  op_backend->client_vendor = get_sftp_client_vendor ();

  if (op_backend->client_vendor == SFTP_VENDOR_INVALID)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Unable to find supported SSH command"));
      return TRUE;
    }
  
  host = g_mount_spec_get (mount_spec, "host");

  if (host == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("No hostname specified"));
      return TRUE;
    }

  port = g_mount_spec_get (mount_spec, "port");
  op_backend->port = -1;
  if (port != NULL)
    {
      int p = atoi (port);
      if (p != 22)
        op_backend->port = p;
    }
  
  user = g_mount_spec_get (mount_spec, "user");

  op_backend->host = g_strdup (host);
  op_backend->user = g_strdup (user);
  if (op_backend->user)
    {
      op_backend->user_specified = TRUE;
      op_backend->user_specified_in_uri = TRUE;
    }
      

  return FALSE;
}

static gboolean
try_unmount (GVfsBackend *backend,
             GVfsJobUnmount *job,
             GMountUnmountFlags flags,
             GMountSource *mount_source)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  if (op_backend->command_connection.reply_stream &&
      op_backend->command_connection.reply_stream_cancellable)
    g_cancellable_cancel (op_backend->command_connection.reply_stream_cancellable);
  if (op_backend->data_connection.reply_stream &&
      op_backend->data_connection.reply_stream_cancellable)
    g_cancellable_cancel (op_backend->data_connection.reply_stream_cancellable);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static int
io_error_code_for_sftp_error (guint32 code, int failure_error)
{
  int error_code;
  
  error_code = G_IO_ERROR_FAILED;
  
  switch (code)
    {
    default:
    case SSH_FX_EOF:
    case SSH_FX_BAD_MESSAGE:
    case SSH_FX_NO_CONNECTION:
    case SSH_FX_CONNECTION_LOST:
      break;
      
    case SSH_FX_FAILURE:
      error_code = failure_error;
      break;
      
    case SSH_FX_NO_SUCH_FILE:
      error_code = G_IO_ERROR_NOT_FOUND;
      break;
      
    case SSH_FX_PERMISSION_DENIED:
      error_code = G_IO_ERROR_PERMISSION_DENIED;
      break;
      
    case SSH_FX_OP_UNSUPPORTED:
      error_code = G_IO_ERROR_NOT_SUPPORTED;
      break;
    }
  return error_code;
}

static char *
error_message (GIOErrorEnum error)
{
  switch (error)
    {
    case G_IO_ERROR_NOT_EMPTY:
      return _("Directory not empty");
    case G_IO_ERROR_EXISTS:
      return _("Target file exists");
    case G_IO_ERROR_NOT_SUPPORTED:
      return _("Operation not supported");
    case G_IO_ERROR_PERMISSION_DENIED:
      return _("Permission denied");
    case G_IO_ERROR_NOT_FOUND:
      return _("No such file or directory");
    default:
      return _("Unknown reason");
    }
}

static gboolean
error_from_status_code (GVfsJob *job,
			guint32 code,
			int failure_error,
			int allowed_sftp_error,
			GError **error)
{
  gint error_code;
  
  if (failure_error == -1)
    failure_error = G_IO_ERROR_FAILED;
  
  if (code == SSH_FX_OK ||
      (allowed_sftp_error != -1 &&
       code == allowed_sftp_error))
    return TRUE;

  if (error)
    {
      error_code = io_error_code_for_sftp_error (code, failure_error);
      *error = g_error_new_literal (G_IO_ERROR, error_code,
				    error_message (error_code));
    }
  
  return FALSE;
}

static gboolean
failure_from_status_code (GVfsJob *job,
			  guint32 code,
			  int failure_error,
			  int allowed_sftp_error)
{
  GError *error;

  error = NULL;
  if (error_from_status_code (job, code, failure_error, allowed_sftp_error, &error))
    return TRUE;
  else
    {
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
    }
  return FALSE;
}

static gboolean
result_from_status_code (GVfsJob *job,
			 guint32 code,
			 int failure_error,
			 int allowed_sftp_error)
{
  gboolean res;

  res = failure_from_status_code (job, code, 
				  failure_error,
				  allowed_sftp_error);
  if (res)
    g_vfs_job_succeeded (job);
  
  return res;
}

static guint32
read_status_code (GDataInputStream *status_reply)
{
  return g_data_input_stream_read_uint32 (status_reply, NULL, NULL);
}

static gboolean
error_from_status (GVfsJob *job,
                   GDataInputStream *reply,
                   int failure_error,
                   int allowed_sftp_error,
                   GError **error)
{
  guint32 code;

  code = read_status_code (reply);

  return error_from_status_code (job, code,
				 failure_error, allowed_sftp_error,
				 error);
}

static gboolean
failure_from_status (GVfsJob *job,
                     GDataInputStream *reply,
                     int failure_error,
                     int allowed_sftp_error)
{
  GError *error;

  error = NULL;
  if (error_from_status (job, reply, failure_error, allowed_sftp_error, &error))
    return TRUE;
  else
    {
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
    }
  return FALSE;
}

static gboolean
result_from_status (GVfsJob *job,
                    GDataInputStream *reply,
                    int failure_error,
                    int allowed_sftp_error)
{
  gboolean res;

  res = failure_from_status (job, reply, 
                             failure_error,
                             allowed_sftp_error);
  if (res)
    g_vfs_job_succeeded (job);
  
  return res;
}



typedef struct _ErrorFromStatData ErrorFromStatData;

typedef void (*ErrorFromStatCallback) (GVfsBackendSftp *backend,
				       GVfsJob *job,
				       gint original_error,
				       gint stat_error,
				       GFileInfo *info,
				       gpointer user_data);
				       

struct _ErrorFromStatData {
  gint original_error;
  ErrorFromStatCallback callback;
  gpointer user_data;
};

static void
error_from_lstat_reply (GVfsBackendSftp *backend,
			int reply_type,
			GDataInputStream *reply,
			guint32 len,
			GVfsJob *job,
			gpointer user_data)
{
  ErrorFromStatData *data = user_data;
  GFileInfo *info;
  gint stat_error;

  stat_error = 0;
  info = NULL;
  if (reply_type == SSH_FXP_STATUS)
    stat_error = read_status_code (reply);
  else if (reply_type == SSH_FXP_ATTRS)
    {
      info = g_file_info_new ();
      parse_attributes (backend,
			info,
			NULL,
			reply,
			NULL);
    }
  else
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      goto out;
    }

  data->callback (backend, job,
		  data->original_error,
		  stat_error,
		  info,
		  data->user_data);

 out:
  g_slice_free (ErrorFromStatData, data);
}

static void
error_from_lstat (GVfsBackendSftp *backend,
		  GVfsJob *job,
		  guint32 original_error,
		  const char *path,
		  ErrorFromStatCallback callback,
		  gpointer user_data)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  ErrorFromStatData *data;
  
  command = new_command_stream (op_backend, SSH_FXP_LSTAT);
  put_string (command, path);

  data = g_slice_new (ErrorFromStatData);
  data->original_error = original_error;
  data->callback = callback;
  data->user_data = user_data;
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 error_from_lstat_reply,
				 G_VFS_JOB (job), data);
}

static void
set_access_attributes_trusted (GFileInfo *info,
			       guint32 perm)
{
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
				     perm & 0x4);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
				     perm & 0x2);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				     perm & 0x1);
}

/* For files we don't own we can't trust a negative response to this check, as
   something else could allow us to do the operation, for instance an ACL
   or some sticky bit thing */
static void
set_access_attributes (GFileInfo *info,
                       guint32 perm)
{
  if (perm & 0x4)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
				       TRUE);
  if (perm & 0x2)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
				       TRUE);
  if (perm & 0x1)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				       TRUE);
}


static void
parse_attributes (GVfsBackendSftp *backend,
                  GFileInfo *info,
                  const char *basename,
                  GDataInputStream *reply,
                  GFileAttributeMatcher *matcher)
{
  guint32 flags;
  GFileType type;
  guint32 uid, gid;
  guint32 mode;
  gboolean has_uid, free_mimetype;
  char *mimetype;
  gboolean uncertain_content_type;
  gboolean is_symlink;
  
  flags = g_data_input_stream_read_uint32 (reply, NULL, NULL);

  if (basename != NULL && basename[0] == '.')
    g_file_info_set_is_hidden (info, TRUE);

  if (basename != NULL)
    g_file_info_set_name (info, basename);
  else
    g_file_info_set_name (info, "/");
  
  if (basename != NULL && basename[strlen (basename) -1] == '~')
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP, TRUE);

  if (flags & SSH_FILEXFER_ATTR_SIZE)
    {
      guint64 size = g_data_input_stream_read_uint64 (reply, NULL, NULL);
      g_file_info_set_size (info, size);
    }

  has_uid = FALSE;
  uid = gid = 0; /* Avoid warnings */
  if (flags & SSH_FILEXFER_ATTR_UIDGID)
    {
      has_uid = TRUE;
      uid = g_data_input_stream_read_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, uid);
      gid = g_data_input_stream_read_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, gid);
    }

  type = G_FILE_TYPE_UNKNOWN;

  if (flags & SSH_FILEXFER_ATTR_PERMISSIONS)
    {
      mode = g_data_input_stream_read_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, mode);

      mimetype = NULL;
      uncertain_content_type = FALSE;
      is_symlink = FALSE;
      if (S_ISREG (mode))
        type = G_FILE_TYPE_REGULAR;
      else if (S_ISDIR (mode))
        {
          type = G_FILE_TYPE_DIRECTORY;
          mimetype = "inode/directory";
        }
      else if (S_ISFIFO (mode))
        {
          type = G_FILE_TYPE_SPECIAL;
          mimetype = "inode/fifo";
        }
      else if (S_ISSOCK (mode))
        {
          type = G_FILE_TYPE_SPECIAL;
          mimetype = "inode/socket";
        }
      else if (S_ISCHR (mode))
        {
          type = G_FILE_TYPE_SPECIAL;
          mimetype = "inode/chardevice";
        }
      else if (S_ISBLK (mode))
        {
          type = G_FILE_TYPE_SPECIAL;
          mimetype = "inode/blockdevice";
        }
      else if (S_ISLNK (mode))
        {
          type = G_FILE_TYPE_SYMBOLIC_LINK;
          is_symlink = TRUE;
          mimetype = "inode/symlink";
        }

      free_mimetype = FALSE;
      if (mimetype == NULL)
        {
          if (basename)
            {
              mimetype = g_content_type_guess (basename, NULL, 0, &uncertain_content_type);
              free_mimetype = TRUE;
            }
          else
            mimetype = "application/octet-stream";
        }

      if (!uncertain_content_type)
        g_file_info_set_content_type (info, mimetype);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, mimetype);
      g_file_info_set_is_symlink (info, is_symlink);
      
      if (g_file_attribute_matcher_matches (matcher,
                                            G_FILE_ATTRIBUTE_STANDARD_ICON)
          || g_file_attribute_matcher_matches (matcher,
                                               G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON))
        {
          GIcon *icon = NULL;
          GIcon *symbolic_icon = NULL;

          icon = g_content_type_get_icon (mimetype);
          symbolic_icon = g_content_type_get_symbolic_icon (mimetype);

          if (icon == NULL)
            icon = g_themed_icon_new ("text-x-generic");
          if (symbolic_icon == NULL)
            symbolic_icon = g_themed_icon_new ("text-x-generic-symbolic");

          g_file_info_set_icon (info, icon);
          g_file_info_set_symbolic_icon (info, symbolic_icon);
          g_object_unref (icon);
          g_object_unref (symbolic_icon);
        }


      if (free_mimetype)
        g_free (mimetype);
      
      if (has_uid && backend->my_uid != (guint32)-1)
        {
          if (uid == backend->my_uid)
            set_access_attributes_trusted (info, (mode >> 6) & 0x7);
          else if (gid == backend->my_gid)
            set_access_attributes (info, (mode >> 3) & 0x7);
          else
            set_access_attributes (info, (mode >> 0) & 0x7);
        }

    }

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

  g_file_info_set_file_type (info, type);
  
  if (flags & SSH_FILEXFER_ATTR_ACMODTIME)
    {
      guint32 v;
      char *etag;
      
      v = g_data_input_stream_read_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, v);
      v = g_data_input_stream_read_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, v);

      etag = g_strdup_printf ("%lu", (long unsigned int)v);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
      g_free (etag);
    }
  
  if (flags & SSH_FILEXFER_ATTR_EXTENDED)
    {
      guint32 count, i;
      char *name, *val;
      count = g_data_input_stream_read_uint32 (reply, NULL, NULL);
      for (i = 0; i < count; i++)
        {
          name = read_string (reply, NULL);
          val = read_string (reply, NULL);

          g_free (name);
          g_free (val);
        }
    }

  /* We use the same setting as for local files. Can't really
   * do better, since there is no way in this version of sftp to find out
   * the remote charset encoding
   */
  if (g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME))
    {
      if (basename != NULL)
        {
          char *display_name = g_filename_display_name (basename);
          
          if (strstr (display_name, "\357\277\275") != NULL)
            {
              char *p = display_name;
              display_name = g_strconcat (display_name, _(" (invalid encoding)"), NULL);
              g_free (p);
            }
          g_file_info_set_display_name (info, display_name);
          g_free (display_name);
        }
      else
        {
          char *name;

          
          /* Translators: This is the name of the root of an SFTP share, like "/ on <hostname>" */
          name = g_strdup_printf (_("/ on %s"), G_VFS_BACKEND_SFTP (backend)->host);
          g_file_info_set_display_name (info, name);
          g_free (name);
        }
    }
  
  if (basename != NULL &&
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME))
    {
      char *edit_name = g_filename_display_name (basename);
      g_file_info_set_edit_name (info, edit_name);
      g_free (edit_name);
    }
}

static SftpHandle *
sftp_handle_new (GDataInputStream *reply)
{
  SftpHandle *handle;

  handle = g_slice_new0 (SftpHandle);
  handle->raw_handle = read_data_buffer (reply);
  handle->offset = 0;

  return handle;
}

static void
sftp_handle_free (SftpHandle *handle)
{
  data_buffer_free (handle->raw_handle);
  g_free (handle->filename);
  g_free (handle->tempname);
  g_slice_free (SftpHandle, handle);
}

static void
open_stat_reply (GVfsBackendSftp *backend,
                 int reply_type,
                 GDataInputStream *reply,
                 guint32 len,
                 GVfsJob *job,
                 gpointer user_data)
{
  if (g_vfs_job_is_finished (job))
    {
      /* Handled in stat reply */
      return;
    }
  
  if (reply_type == SSH_FXP_ATTRS)
    {
      GFileType type;
      GFileInfo *info = g_file_info_new ();
      
      parse_attributes (backend, info, NULL,
                        reply, NULL);
      type = g_file_info_get_file_type (info);
      g_object_unref (info);
      
      if (type == G_FILE_TYPE_DIRECTORY)
        {
          g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                            _("File is directory"));
          return;
        }
    }

  if (GPOINTER_TO_INT (G_VFS_JOB(job)->backend_data) == 1)
    {
      /* We ran the read_reply and it was a generic failure */
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Failure"));
    }
}

static void
open_for_read_reply (GVfsBackendSftp *backend,
                     int reply_type,
                     GDataInputStream *reply,
                     guint32 len,
                     GVfsJob *job,
                     gpointer user_data)
{
  SftpHandle *handle;

  if (g_vfs_job_is_finished (job))
    {
      /* Handled in stat reply */

      /* Normally this should not happen as we
         sent an is_dir error. But i guess we can
         race */
      if (reply_type == SSH_FXP_HANDLE)
        {
          GDataOutputStream *command;
          DataBuffer *bhandle;

          bhandle = read_data_buffer (reply);
          
          command = new_command_stream (backend, SSH_FXP_CLOSE);
          put_data_buffer (command, bhandle);
          queue_command_stream_and_free (&backend->command_connection, command,
                                         NULL,
                                         G_VFS_JOB (job), NULL);

          data_buffer_free (bhandle);
        }
      
      return;
    }
  
  if (reply_type == SSH_FXP_STATUS)
    {
      if (failure_from_status (job,
                               reply,
                               -1,
                               SSH_FX_FAILURE))
        {
          /* Unknown failure type, mark that we got this and
             return result from stat result */
          G_VFS_JOB(job)->backend_data = GINT_TO_POINTER (1);
        }
      
      return;
    }

  if (reply_type != SSH_FXP_HANDLE)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }

  handle = sftp_handle_new (reply);
  
  g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ (job), handle);
  g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), TRUE);
  g_vfs_job_succeeded (job);
}

static gboolean
try_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  G_VFS_JOB(job)->backend_data = GINT_TO_POINTER (0);
  
  command = new_command_stream (op_backend,
                                SSH_FXP_STAT);
  put_string (command, filename);
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 open_stat_reply,
                                 G_VFS_JOB (job), NULL);

  command = new_command_stream (op_backend,
                                SSH_FXP_OPEN);
  put_string (command, filename);
  g_data_output_stream_put_uint32 (command, SSH_FXF_READ, NULL, NULL); /* open flags */
  g_data_output_stream_put_uint32 (command, 0, NULL, NULL); /* Attr flags */
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 open_for_read_reply,
                                 G_VFS_JOB (job), NULL);

  return TRUE;
}

static void
read_reply (GVfsBackendSftp *backend,
            int reply_type,
            GDataInputStream *reply,
            guint32 len,
            GVfsJob *job,
            gpointer user_data)
{
  SftpHandle *handle;
  guint32 count;
  
  handle = user_data;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      result_from_status (job, reply, -1, SSH_FX_EOF);
      return;
    }

  if (reply_type != SSH_FXP_DATA)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }
  
  count = g_data_input_stream_read_uint32 (reply, NULL, NULL);

  if (!g_input_stream_read_all (G_INPUT_STREAM (reply),
                                G_VFS_JOB_READ (job)->buffer, count,
                                NULL, NULL, NULL))
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }
  
  handle->offset += count;

  g_vfs_job_read_set_size (G_VFS_JOB_READ (job), count);
  g_vfs_job_succeeded (job);
}

static gboolean
try_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle _handle,
          char *buffer,
          gsize bytes_requested)
{
  SftpHandle *handle = _handle;
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend,
                                SSH_FXP_READ);
  put_data_buffer (command, handle->raw_handle);
  g_data_output_stream_put_uint64 (command, handle->offset, NULL, NULL);
  g_data_output_stream_put_uint32 (command, bytes_requested, NULL, NULL);
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 read_reply,
                                 G_VFS_JOB (job), handle);

  return TRUE;
}

static void
seek_read_fstat_reply (GVfsBackendSftp *backend,
                       int reply_type,
                       GDataInputStream *reply,
                       guint32 len,
                       GVfsJob *job,
                       gpointer user_data)
{
  SftpHandle *handle;
  GFileInfo *info;
  goffset file_size;
  GVfsJobSeekRead *op_job;
  
  handle = user_data;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      result_from_status (job, reply, -1, -1);
      return;
    }

  if (reply_type != SSH_FXP_ATTRS)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }

  info = g_file_info_new ();
  parse_attributes (backend, info, NULL,
                    reply, NULL);
  file_size = g_file_info_get_size (info);
  g_object_unref (info);

  op_job = G_VFS_JOB_SEEK_READ (job);

  handle->offset = file_size + op_job->requested_offset;

  if (handle->offset < 0)
    handle->offset = 0;

  g_vfs_job_seek_read_set_offset (op_job, handle->offset);
  g_vfs_job_succeeded (job);
}

static gboolean
try_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle _handle,
                  goffset    offset,
                  GSeekType  type)
{
  SftpHandle *handle = _handle;
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  switch (job->seek_type)
    {
    case G_SEEK_CUR:
      handle->offset += job->requested_offset;
      break;
    case G_SEEK_SET:
      handle->offset = job->requested_offset;
      break;
    case G_SEEK_END:
      command = new_command_stream (op_backend,
                                    SSH_FXP_FSTAT);
      put_data_buffer (command, handle->raw_handle);
      queue_command_stream_and_free (&op_backend->command_connection, command,
                                     seek_read_fstat_reply,
                                     G_VFS_JOB (job), handle);
      return TRUE;
    }

  if (handle->offset < 0)
    handle->offset = 0;

  g_vfs_job_seek_read_set_offset (job, handle->offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
delete_temp_file (GVfsBackendSftp *backend,
                  SftpHandle *handle,
                  GVfsJob *job)
{
  GDataOutputStream *command;
  
  if (handle->tempname)
    {
      command = new_command_stream (backend,
                                    SSH_FXP_REMOVE);
      put_string (command, handle->tempname);
      queue_command_stream_and_free (&backend->command_connection, command,
                                     NULL,
                                     job, NULL);
    }
}

static void
close_moved_tempfile (GVfsBackendSftp *backend,
                      int reply_type,
                      GDataInputStream *reply,
                      guint32 len,
                      GVfsJob *job,
                      gpointer user_data)
{
  SftpHandle *handle;
  
  handle = user_data;

  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));

  /* On failure, don't remove tempfile, since we removed the new original file */
  sftp_handle_free (handle);
}
  
static void
close_restore_permissions (GVfsBackendSftp *backend,
                           int reply_type,
                           GDataInputStream *reply,
                           guint32 len,
                           GVfsJob *job,
                           gpointer user_data)
{
  GDataOutputStream *command;
  SftpHandle *handle;

  handle = user_data;
  
  /* Here we don't really care whether or not setting the permissions succeeded
     or not. We just take the last step and rename the temp file to the
     actual file */
  command = new_command_stream (backend,
                                SSH_FXP_RENAME);
  put_string (command, handle->tempname);
  put_string (command, handle->filename);
  queue_command_stream_and_free (&backend->command_connection, command,
                                 close_moved_tempfile,
                                 G_VFS_JOB (job), handle);
}

static void
close_deleted_file (GVfsBackendSftp *backend,
                    int reply_type,
                    GDataInputStream *reply,
                    guint32 len,
                    GVfsJob *job,
                    gpointer user_data)
{
  GDataOutputStream *command;
  GError *error;
  gboolean res;
  SftpHandle *handle;

  handle = user_data;

  error = NULL;
  res = FALSE;
  if (reply_type == SSH_FXP_STATUS)
    res = error_from_status (job, reply, -1, -1, &error);
  else
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
	                 _("Invalid reply received"));

  if (res)
    {
      /* Removed original file, now first try to restore permissions */
      if (handle->set_permissions)
        {
          command = new_command_stream (backend, SSH_FXP_SETSTAT);
          put_string (command, handle->tempname);
          g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_PERMISSIONS, NULL, NULL);
          g_data_output_stream_put_uint32 (command, handle->permissions, NULL, NULL);
          queue_command_stream_and_free (&backend->command_connection, command,
                                         close_restore_permissions,
                                         G_VFS_JOB (job), handle);
        }
      else
        {
          /* Skip restoring permissions by calling the callback directly */
          close_restore_permissions(backend, 0, NULL, 0, job, user_data);
        }
    }
  else
    {
      /* The delete failed, remove any temporary files */
      delete_temp_file (backend,
                        handle,
                        G_VFS_JOB (job));
      
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      sftp_handle_free (handle);
    }
}

static void
close_moved_file (GVfsBackendSftp *backend,
                  int reply_type,
                  GDataInputStream *reply,
                  guint32 len,
                  GVfsJob *job,
                  gpointer user_data)
{
  GDataOutputStream *command;
  GError *error;
  gboolean res;
  SftpHandle *handle;

  handle = user_data;

  error = NULL;
  res = FALSE;
  if (reply_type == SSH_FXP_STATUS)
    res = error_from_status (job, reply, -1, -1, &error);
  else
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
	                 _("Invalid reply received"));

  if (res)
    {
      /* moved original file to backup, now move new file in place */

      command = new_command_stream (backend,
                                    SSH_FXP_RENAME);
      put_string (command, handle->tempname);
      put_string (command, handle->filename);
      queue_command_stream_and_free (&backend->command_connection, command,
                                     close_moved_tempfile,
                                     G_VFS_JOB (job), handle);
    }
  else
    {
      /* Move original file to backup name failed, remove any temporary files */
      delete_temp_file (backend,
                        handle,
                        G_VFS_JOB (job));
      
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Error creating backup file: %s"), error->message);
      g_error_free (error);
      sftp_handle_free (handle);
    }
}

static void
close_deleted_backup (GVfsBackendSftp *backend,
                      int reply_type,
                      GDataInputStream *reply,
                      guint32 len,
                      GVfsJob *job,
                      gpointer user_data)
{
  SftpHandle *handle;
  GDataOutputStream *command;
  char *backup_name;

  /* Ignore result here, if it failed we'll just get a new error when moving over it
   * This is simpler than ignoring NOEXIST errors
   */
  
  handle = user_data;
  
  command = new_command_stream (backend,
                                SSH_FXP_RENAME);
  backup_name = g_strconcat (handle->filename, "~", NULL);
  put_string (command, handle->filename);
  put_string (command, backup_name);
  g_free (backup_name);
  queue_command_stream_and_free (&backend->command_connection, command,
                                 close_moved_file,
                                 G_VFS_JOB (job), handle);
}

static void
close_write_reply (GVfsBackendSftp *backend,
                   int reply_type,
                   GDataInputStream *reply,
                   guint32 len,
                   GVfsJob *job,
                   gpointer user_data)
{
  GDataOutputStream *command;
  GError *error;
  gboolean res;
  char *backup_name;
  SftpHandle *handle;

  handle = user_data;

  error = NULL;
  res = FALSE;
  if (reply_type == SSH_FXP_STATUS)
    res = error_from_status (job, reply, -1, -1, &error);
  else
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
	                 _("Invalid reply received"));

  if (res)
    {
      if (handle->tempname)
        {
          if (handle->make_backup)
            {
              command = new_command_stream (backend,
                                            SSH_FXP_REMOVE);
              backup_name = g_strconcat (handle->filename, "~", NULL);
              put_string (command, backup_name);
              g_free (backup_name);
              queue_command_stream_and_free (&backend->command_connection,
                                             command,
                                             close_deleted_backup,
                                             G_VFS_JOB (job), handle);
            }
          else
            {
              command = new_command_stream (backend,
                                            SSH_FXP_REMOVE);
              put_string (command, handle->filename);
              queue_command_stream_and_free (&backend->command_connection,
                                             command,
                                             close_deleted_file,
                                             G_VFS_JOB (job), handle);
            }
        }
      else
        {
          g_vfs_job_succeeded (job);
          sftp_handle_free (handle);
        }
    }
  else
    {
      /* The close failed, remove any temporary files */
      delete_temp_file (backend,
                        handle,
                        G_VFS_JOB (job));
      
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      
      sftp_handle_free (handle);
    }
}

static void
close_write_fstat_reply (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job,
                        gpointer user_data)
{
  SftpHandle *handle = user_data;
  GDataOutputStream *command;
  GFileInfo *info;
  const char *etag;
  
  if (reply_type == SSH_FXP_ATTRS)
    {
      info = g_file_info_new ();
      parse_attributes (backend, info, NULL,
                        reply, NULL);
      etag = g_file_info_get_etag (info);
      if (etag)
        g_vfs_job_close_write_set_etag (G_VFS_JOB_CLOSE_WRITE (job), etag);
      g_object_unref (info);
    }
  
  command = new_command_stream (backend, SSH_FXP_CLOSE);
  put_data_buffer (command, handle->raw_handle);

  queue_command_stream_and_free (&backend->command_connection, command,
                                 close_write_reply,
                                 G_VFS_JOB (job), handle);
}

static gboolean
try_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle _handle)
{
  SftpHandle *handle = _handle;
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend, SSH_FXP_FSTAT);
  put_data_buffer (command, handle->raw_handle);

  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 close_write_fstat_reply,
                                 G_VFS_JOB (job), handle);

  return TRUE;
}


static void
close_read_reply (GVfsBackendSftp *backend,
                  int reply_type,
                  GDataInputStream *reply,
                  guint32 len,
                  GVfsJob *job,
                  gpointer user_data)
{
  SftpHandle *handle;

  handle = user_data;

  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
  sftp_handle_free (handle);
}

static gboolean
try_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle _handle)
{
  SftpHandle *handle = _handle;
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend, SSH_FXP_CLOSE);
  put_data_buffer (command, handle->raw_handle);

  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 close_read_reply,
                                 G_VFS_JOB (job), handle);

  return TRUE;
}

static void
put_mode (GDataOutputStream *command, GFileCreateFlags flags)
{
  if (flags & G_FILE_CREATE_PRIVATE)
    {
      g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_PERMISSIONS, NULL, NULL);
      g_data_output_stream_put_uint32 (command, 0600, NULL, NULL);
    }
  else
    {
      g_data_output_stream_put_uint32 (command, 0, NULL, NULL);
    }
}

static void not_dir_or_not_exist_error (GVfsBackendSftp *backend,
					GVfsJob *job,
					char *filename);

static void
not_dir_or_not_exist_error_cb (GVfsBackendSftp *backend,
			       GVfsJob *job,
			       gint original_error,
			       gint stat_error,
			       GFileInfo *info,
			       gpointer user_data)
{
  char *path;

  path = user_data;
  if (info != NULL)
    {
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
	/* Parent is a directory, so must not have found child */
	g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			  _("No such file or directory"));
      else /* Some path element was not a directory */
	g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
			  _("Not a directory"));
    }
  else if (stat_error == SSH_FX_NO_SUCH_FILE)
    {
      not_dir_or_not_exist_error (backend, job, path);
    }
  else
    {
      /* Some other weird error, lets say "not found" */
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			_("No such file or directory"));
    }
  
  g_free (path);
}

static void
not_dir_or_not_exist_error (GVfsBackendSftp *backend,
			    GVfsJob *job,
			    char *filename)
{
  char *parent;

  parent = g_path_get_dirname (filename);
  if (strcmp (parent, ".") == 0)
    {
      g_free (parent);
      /* Root not found? Weird, but at least not
	 NOT_DIRECTORY, so lets report not found */
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			_("No such file or directory"));
      return;
    }
    
  error_from_lstat (backend,
		    job,
		    SSH_FX_NO_SUCH_FILE,
		    filename,
		    not_dir_or_not_exist_error_cb,
		    parent);
}

static void
open_for_write_error (GVfsBackendSftp *backend,
                      GVfsJob *job,
                      gint original_error,
                      gint stat_error,
                      GFileInfo *info,
                      gpointer user_data)
{
  if (info != NULL &&
      g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                        _("File is directory"));
      return;
    }

  result_from_status_code (job, original_error, -1, -1);
}

static void
open_for_write_reply (GVfsBackendSftp *backend,
                      int reply_type,
                      GDataInputStream *reply,
                      guint32 len,
                      GVfsJob *job,
                      gpointer user_data)
{
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  SftpHandle *handle;
  guint32 code;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      code = read_status_code (reply);

      if (code == SSH_FX_NO_SUCH_FILE)
	{
	  /* openssh sftp returns NO_SUCH_FILE for both ENOTDIR and ENOENT,
	     we need to stat-walk the hierarchy to see what the error was */
	  not_dir_or_not_exist_error (backend, job,
				      G_VFS_JOB_OPEN_FOR_WRITE (job)->filename);
	  return;
	}
      
      if (code == SSH_FX_FAILURE && op_job->mode != OPEN_FOR_WRITE_CREATE)
        {
          error_from_lstat (backend, job, code, op_job->filename,
                            open_for_write_error, NULL);
          return;
        }

      result_from_status_code (job, code, G_IO_ERROR_EXISTS, -1);
      return;
    }

  if (reply_type != SSH_FXP_HANDLE)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }

  handle = sftp_handle_new (reply);
  
  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), handle);
  g_vfs_job_open_for_write_set_can_seek (G_VFS_JOB_OPEN_FOR_WRITE (job), TRUE);
  g_vfs_job_open_for_write_set_can_truncate (G_VFS_JOB_OPEN_FOR_WRITE (job), TRUE);
  g_vfs_job_succeeded (job);
}

static void
open_for_write (GVfsBackend *backend,
                GVfsJobOpenForWrite *job,
                const char *filename,
                GFileCreateFlags flags,
                int open_flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend,
                                SSH_FXP_OPEN);
  put_string (command, filename);
  g_data_output_stream_put_uint32 (command, open_flags, NULL, NULL); /* open flags */
  put_mode (command, flags);
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 open_for_write_reply,
                                 G_VFS_JOB (job), NULL);
}

static gboolean
try_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  open_for_write (backend,
                  job,
                  filename,
                  flags,
                  SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_EXCL);

  return TRUE;
}

static gboolean
try_append_to (GVfsBackend *backend,
               GVfsJobOpenForWrite *job,
               const char *filename,
               GFileCreateFlags flags)
{
  open_for_write (backend,
                  job,
                  filename,
                  flags,
                  SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_APPEND);

  return TRUE;
}

static gboolean
try_edit (GVfsBackend *backend,
          GVfsJobOpenForWrite *job,
          const char *filename,
          GFileCreateFlags flags)
{
  open_for_write (backend,
                  job,
                  filename,
                  flags,
                  SSH_FXF_WRITE|SSH_FXF_CREAT);

  return TRUE;
}

typedef struct {
  guint32 permissions;
  gboolean set_permissions;
  guint32 uid;
  guint32 gid;
  gboolean set_ownership;
  char *tempname;
  int temp_count;
} ReplaceData;

static void
replace_data_free (ReplaceData *data)
{
  g_free (data->tempname);
  g_slice_free (ReplaceData, data);
}

static void replace_create_temp (GVfsBackendSftp *backend,
                                 GVfsJobOpenForWrite *job);

static void
replace_truncate_original_reply (GVfsBackendSftp *backend,
                                 int reply_type,
                                 GDataInputStream *reply,
                                 guint32 len,
                                 GVfsJob *job,
                                 gpointer user_data)
{
  GVfsJobOpenForWrite *op_job;
  SftpHandle *handle;
  ReplaceData *data;
  GError *error = NULL;

  op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  data = G_VFS_JOB (job)->backend_data;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      error = NULL;
      if (error_from_status (job, reply, G_IO_ERROR_EXISTS, -1, &error))
        /* Open should not return OK */
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Invalid reply received"));
      else
        {
          g_vfs_job_failed_from_error (job, error);
          g_error_free (error);
        }
      return;
    }

  if (reply_type != SSH_FXP_HANDLE)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }

  handle = sftp_handle_new (reply);
  handle->filename = g_strdup (op_job->filename);
  handle->tempname = NULL;
  handle->permissions = data->permissions;
  handle->set_permissions = data->set_permissions;
  handle->make_backup = op_job->make_backup;
  
  g_vfs_job_open_for_write_set_handle (op_job, handle);
  g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
  
  g_vfs_job_succeeded (job);
}

static void
replace_truncate_original (GVfsBackendSftp *backend,
                           GVfsJob *job)
{
  GVfsJobOpenForWrite *op_job;
  GDataOutputStream *command;

  op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  
  command = new_command_stream (backend,
                                SSH_FXP_OPEN);
  put_string (command, op_job->filename);
  g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_TRUNC,  NULL, NULL); /* open flags */
  put_mode (command, op_job->flags);
  
  queue_command_stream_and_free (&backend->command_connection, command,
                                 replace_truncate_original_reply,
                                 job, NULL);
}

static void
close_temp_file (GVfsBackendSftp *backend,
                 SftpHandle *handle,
                 GVfsJob *job)
{
  GDataOutputStream *command;

  command = new_command_stream (backend,
                                SSH_FXP_CLOSE);
  put_data_buffer (command, handle->raw_handle);
  queue_command_stream_and_free (&backend->command_connection, command,
                                 NULL,
                                 G_VFS_JOB (job), NULL);
}

static void
replace_create_temp_fsetstat_reply (GVfsBackendSftp *backend,
                                    int reply_type,
                                    GDataInputStream *reply,
                                    guint32 len,
                                    GVfsJob *job,
                                    gpointer user_data)
{
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  ReplaceData *data = job->backend_data;
  SftpHandle *handle = user_data;
  GError *error = NULL;

  if (reply_type == SSH_FXP_STATUS)
    error_from_status (job, reply, -1, -1, &error);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));

  if (error != NULL)
    {
      if (data->set_ownership &&
          error->code == G_IO_ERROR_PERMISSION_DENIED)
        {
          g_error_free (error);

          close_temp_file (backend, handle, job);
          delete_temp_file (backend, handle, job);
          sftp_handle_free (handle);

          /* This was probably due to the fact that the ownership could not be
             set properly. In this case we change our strategy altogether and
             simply open/truncate the original file. This is not as secure
             as the atomit tempfile/move approach, but at least ownership
             doesn't change */
          if (!op_job->make_backup)
            replace_truncate_original (backend, job);
          else
            {
              /* We only do this if make_backup is FALSE, as this version breaks
                 the backup code. Would like to handle the backup case too by
                 backing up before truncating, but for now error out... */
              g_vfs_job_failed (job, G_IO_ERROR,
                                G_IO_ERROR_CANT_CREATE_BACKUP,
                                _("Backups not supported"));
            }
        }
      else
        {
          g_vfs_job_failed_from_error (job, error);
          g_error_free (error);
        }
      return;
    }

  g_vfs_job_open_for_write_set_handle (op_job, handle);
  g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
  g_vfs_job_succeeded (job);
}

static void
replace_create_temp_fstat_reply (GVfsBackendSftp *backend,
                                 int reply_type,
                                 GDataInputStream *reply,
                                 guint32 len,
                                 GVfsJob *job,
                                 gpointer user_data)
{
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  ReplaceData *data = job->backend_data;
  SftpHandle *handle = user_data;
  GFileInfo *info;
  guint32 uid = -1;
  guint32 gid = -1;

  if (reply_type == SSH_FXP_STATUS ||
      reply_type != SSH_FXP_ATTRS)
    {
      close_temp_file (backend, handle, job);
      delete_temp_file (backend, handle, job);
      sftp_handle_free (handle);

      if (reply_type == SSH_FXP_STATUS)
        result_from_status (job, reply, -1, -1);
      else
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Invalid reply received"));
      return;
    }

  info = g_file_info_new ();
  parse_attributes (backend, info, NULL, reply, NULL);

  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_UID) &&
      g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_GID))
    {
      uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);
      gid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID);
    }

  g_object_unref (info);

  if (uid != data->uid || gid != data->gid)
    {
      GDataOutputStream *command;

      command = new_command_stream (backend, SSH_FXP_FSETSTAT);
      put_data_buffer (command, handle->raw_handle);
      g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_UIDGID, NULL, NULL);
      g_data_output_stream_put_uint32 (command, data->uid, NULL, NULL);
      g_data_output_stream_put_uint32 (command, data->gid, NULL, NULL);
      queue_command_stream_and_free (&backend->command_connection, command,
                                     replace_create_temp_fsetstat_reply, job, handle);
      return;
    }

  g_vfs_job_open_for_write_set_handle (op_job, handle);
  g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
  g_vfs_job_succeeded (job);
}

static void
replace_create_temp_reply (GVfsBackendSftp *backend,
                           int reply_type,
                           GDataInputStream *reply,
                           guint32 len,
                           GVfsJob *job,
                           gpointer user_data)
{
  GVfsJobOpenForWrite *op_job;
  SftpHandle *handle;
  ReplaceData *data;
  GError *error;

  op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  data = G_VFS_JOB (job)->backend_data;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      error = NULL;
      if (error_from_status (job, reply, G_IO_ERROR_EXISTS, -1, &error))
        /* Open should not return OK */
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Invalid reply received"));
      else if (error->code == G_IO_ERROR_EXISTS)
        {
          /* It was *probably* the EXCL flag failing. I wish we had
             an actual real error code for that, grumble */
          g_error_free (error);

          replace_create_temp (backend, op_job);
        }
      else
        {
          g_vfs_job_failed_from_error (job, error);
          g_error_free (error);
        }
      return;
    }

  if (reply_type != SSH_FXP_HANDLE)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }

  handle = sftp_handle_new (reply);
  handle->filename = g_strdup (op_job->filename);
  handle->tempname = g_strdup (data->tempname);
  handle->permissions = data->permissions;
  handle->set_permissions = data->set_permissions;
  handle->make_backup = op_job->make_backup;

  if (data->set_ownership)
    {
      GDataOutputStream *command;

      command = new_command_stream (backend, SSH_FXP_FSTAT);
      put_data_buffer (command, handle->raw_handle);
      queue_command_stream_and_free (&backend->command_connection, command,
                                     replace_create_temp_fstat_reply, job, handle);
      return;
    }

  g_vfs_job_open_for_write_set_handle (op_job, handle);
  g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
  
  g_vfs_job_succeeded (job);
}

static void
replace_create_temp (GVfsBackendSftp *backend,
                     GVfsJobOpenForWrite *job)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  char *dirname;
  ReplaceData *data;
  char basename[] = ".giosaveXXXXXX";

  data = G_VFS_JOB (job)->backend_data;

  data->temp_count++;

  if (data->temp_count == 100)
    {
      g_vfs_job_failed (G_VFS_JOB (job), 
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Unable to create temporary file"));
      return;
    }
  
  g_free (data->tempname);
  
  dirname = g_path_get_dirname (job->filename);
  gvfs_randomize_string (basename + 8, 6);
  data->tempname = g_build_filename (dirname, basename, NULL);
  g_free (dirname);

  command = new_command_stream (op_backend,
                                SSH_FXP_OPEN);
  put_string (command, data->tempname);
  g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_EXCL,  NULL, NULL); /* open flags */
  g_data_output_stream_put_uint32 (command, (data->set_permissions ? SSH_FILEXFER_ATTR_PERMISSIONS : 0), NULL, NULL);

  if (data->set_permissions)
    g_data_output_stream_put_uint32 (command, data->permissions, NULL, NULL);
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 replace_create_temp_reply,
                                 G_VFS_JOB (job), NULL);
}

static void
replace_stat_reply (GVfsBackendSftp *backend,
                    int reply_type,
                    GDataInputStream *reply,
                    guint32 len,
                    GVfsJob *job,
                    gpointer user_data)
{
  GFileInfo *info;
  GVfsJobOpenForWrite *op_job;
  const char *current_etag;
  guint32 permissions;
  guint32 uid;
  guint32 gid;
  gboolean set_permissions;
  gboolean set_ownership = FALSE;
  gboolean is_regular = FALSE;
  ReplaceData *data;

  op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);

  set_permissions = op_job->flags & G_FILE_CREATE_PRIVATE ? TRUE : FALSE;
  permissions = 0600;
  
  if (reply_type == SSH_FXP_ATTRS)
    {
      info = g_file_info_new ();
      parse_attributes (backend, info, NULL,
                        reply, NULL);

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
	{
	  g_object_unref (info);
          g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                            _("File is directory"));
          return;
	}
      else if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR)
        is_regular = TRUE;
      
      if (op_job->etag != NULL)
        {
          current_etag = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE);

          if (current_etag == NULL ||
              strcmp (op_job->etag, current_etag) != 0)
            {
              g_vfs_job_failed (job, 
                                G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                                _("The file was externally modified"));
              g_object_unref (info);
              return;
            }
        }

      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_MODE) &&
          !(op_job->flags & G_FILE_CREATE_REPLACE_DESTINATION))
        {
          set_permissions = TRUE;
          permissions = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE) & 0777;
        }
      
      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_UID) &&
          g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_GID) &&
          !(op_job->flags & G_FILE_CREATE_REPLACE_DESTINATION))
      {
        set_ownership = TRUE;
        uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);
        gid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID);
      }
    }

  data = g_slice_new0 (ReplaceData);
  data->set_permissions = set_permissions;
  if (set_permissions)
    data->permissions = permissions;

  data->set_ownership = set_ownership;
  if (set_ownership)
  {
    data->uid = uid;
    data->gid = gid;
  }
  
  g_vfs_job_set_backend_data (job, data, (GDestroyNotify)replace_data_free);

  /* If G_FILE_CREATE_REPLACE_DESTINATION is specified or the destination
   * is a regular file, replace by writing to a temp file and renaming rather
   * than truncating. */
  if (op_job->flags & G_FILE_CREATE_REPLACE_DESTINATION || is_regular)
    replace_create_temp (backend, op_job);
  else
    {
      if (op_job->make_backup)
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_CANT_CREATE_BACKUP,
                          _("Backups not supported"));
      else
        replace_truncate_original (backend, job);
    }
}

static void
replace_exclusive_reply (GVfsBackendSftp *backend,
                         int reply_type,
                         GDataInputStream *reply,
                         guint32 len,
                         GVfsJob *job,
                         gpointer user_data)
{
  GVfsJobOpenForWrite *op_job;
  GDataOutputStream *command;
  SftpHandle *handle;
  GError *error;

  op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  if (reply_type == SSH_FXP_STATUS)
    {
      error = NULL;
      if (error_from_status (job, reply, G_IO_ERROR_EXISTS, -1, &error))
        /* Open should not return OK */
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Invalid reply received"));
      else if (error->code == G_IO_ERROR_NOT_FOUND)
	{
          g_error_free (error);
	  /* openssh sftp returns NO_SUCH_FILE for ENOTDIR */
	  not_dir_or_not_exist_error (backend, job,
				      G_VFS_JOB_OPEN_FOR_WRITE (job)->filename);
	}
      else if (error->code == G_IO_ERROR_EXISTS)
        {
          /* It was *probably* the EXCL flag failing. I wish we had
             an actual real error code for that, grumble */
          g_error_free (error);
          
          /* Replace existing file code: */
          
          command = new_command_stream (backend,
                                        SSH_FXP_LSTAT);
          put_string (command, op_job->filename);
          queue_command_stream_and_free (&backend->command_connection, command,
                                         replace_stat_reply,
                                         G_VFS_JOB (job), NULL);
        }
      else
        {
          g_vfs_job_failed_from_error (job, error);
          g_error_free (error);
        }
      return;
    }

  if (reply_type != SSH_FXP_HANDLE)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }
  
  handle = sftp_handle_new (reply);
  
  g_vfs_job_open_for_write_set_handle (op_job, handle);
  g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
  
  g_vfs_job_succeeded (job);
}

static gboolean
try_replace (GVfsBackend *backend,
             GVfsJobOpenForWrite *job,
             const char *filename,
             const char *etag,
             gboolean make_backup,
             GFileCreateFlags flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend,
                                SSH_FXP_OPEN);
  put_string (command, filename);
  g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_EXCL,  NULL, NULL); /* open flags */
  put_mode (command, flags);
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 replace_exclusive_reply,
                                 G_VFS_JOB (job), NULL);

  return TRUE;
}

static void
write_reply (GVfsBackendSftp *backend,
             int reply_type,
             GDataInputStream *reply,
             guint32 len,
             GVfsJob *job,
             gpointer user_data)
{
  SftpHandle *handle;
  
  handle = user_data;

  if (reply_type == SSH_FXP_STATUS)
    {
      if (result_from_status (job, reply, -1, -1))
        {
          handle->offset += G_VFS_JOB_WRITE (job)->written_size;
        }
    }
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static gboolean
try_write (GVfsBackend *backend,
           GVfsJobWrite *job,
           GVfsBackendHandle _handle,
           char *buffer,
           gsize buffer_size)
{
  SftpHandle *handle = _handle;
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  gsize size;

  size = MIN (buffer_size, MAX_BUFFER_SIZE);

  command = new_command_stream (op_backend,
                                SSH_FXP_WRITE);
  put_data_buffer (command, handle->raw_handle);
  g_data_output_stream_put_uint64 (command, handle->offset, NULL, NULL);
  g_data_output_stream_put_uint32 (command, size, NULL, NULL);
  /* Ideally we shouldn't do this copy, but doing the writes as multiple writes
     caused problems on the read side in openssh */
  g_output_stream_write_all (G_OUTPUT_STREAM (command),
                             buffer, size,
                             NULL, NULL, NULL);
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 write_reply,
                                 G_VFS_JOB (job), handle);

  /* We always write the full size (on success) */
  g_vfs_job_write_set_written_size (job, size);

  return TRUE;
}

static void
seek_write_fstat_reply (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job,
                        gpointer user_data)
{
  SftpHandle *handle;
  GFileInfo *info;
  goffset file_size;
  GVfsJobSeekWrite *op_job;
  
  handle = user_data;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      result_from_status (job, reply, -1, -1);
      return;
    }

  if (reply_type != SSH_FXP_ATTRS)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }

  info = g_file_info_new ();
  parse_attributes (backend, info, NULL,
                    reply, NULL);
  file_size = g_file_info_get_size (info);
  g_object_unref (info);

  op_job = G_VFS_JOB_SEEK_WRITE (job);

  handle->offset = file_size + op_job->requested_offset;

  if (handle->offset < 0)
    handle->offset = 0;

  g_vfs_job_seek_write_set_offset (op_job, handle->offset);
  g_vfs_job_succeeded (job);
}

static gboolean
try_seek_on_write (GVfsBackend *backend,
                   GVfsJobSeekWrite *job,
                   GVfsBackendHandle _handle,
                   goffset    offset,
                   GSeekType  type)
{
  SftpHandle *handle = _handle;
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  switch (job->seek_type)
    {
    case G_SEEK_CUR:
      handle->offset += job->requested_offset;
      break;
    case G_SEEK_SET:
      handle->offset = job->requested_offset;
      break;
    case G_SEEK_END:
      command = new_command_stream (op_backend,
                                    SSH_FXP_FSTAT);
      put_data_buffer (command, handle->raw_handle);
      queue_command_stream_and_free (&op_backend->command_connection, command,
                                     seek_write_fstat_reply,
                                     G_VFS_JOB (job), handle);
      return TRUE;
    }

  if (handle->offset < 0)
    handle->offset = 0;

  g_vfs_job_seek_write_set_offset (job, handle->offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
truncate_reply (GVfsBackendSftp *backend,
                int reply_type,
                GDataInputStream *reply,
                guint32 len,
                GVfsJob *job,
                gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static gboolean
try_truncate (GVfsBackend *backend,
              GVfsJobTruncate *job,
              GVfsBackendHandle _handle,
              goffset size)
{
  SftpHandle *handle = _handle;
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend, SSH_FXP_FSETSTAT);
  put_data_buffer (command, handle->raw_handle);
  g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_SIZE, NULL, NULL);
  g_data_output_stream_put_uint64 (command, size, NULL, NULL);
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 truncate_reply,
                                 G_VFS_JOB (job), NULL);

  return TRUE;
}

typedef struct {
  DataBuffer *handle;
  int outstanding_requests;
} ReadDirData;

static
void
read_dir_data_free (ReadDirData *data)
{
  data_buffer_free (data->handle);
  g_slice_free (ReadDirData, data);
}

static void
read_dir_readlink_reply (GVfsBackendSftp *backend,
                         int reply_type,
                         GDataInputStream *reply,
                         guint32 len,
                         GVfsJob *job,
                         gpointer user_data)
{
  ReadDirData *data;
  GFileInfo *info = user_data;
  char *target;

  data = job->backend_data;

  if (reply_type == SSH_FXP_NAME)
    {
      /* count = */ (void) g_data_input_stream_read_uint32 (reply, NULL, NULL);
      
      target = read_string (reply, NULL);
      if (target)
        {
          g_file_info_set_symlink_target (info, target);
          g_free (target);
        }
    }

  g_vfs_job_enumerate_add_info (G_VFS_JOB_ENUMERATE (job), info);
  g_object_unref (info);
  
  if (--data->outstanding_requests == 0)
    g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
}

static void
read_dir_got_stat_info (GVfsBackendSftp *backend,
                        GVfsJob *job,
                        GFileInfo *info)
{
  GVfsJobEnumerate *enum_job;
  GDataOutputStream *command;
  ReadDirData *data;
  char *abs_name;
  
  data = job->backend_data;
  
  enum_job = G_VFS_JOB_ENUMERATE (job);

  if (g_file_attribute_matcher_matches (enum_job->attribute_matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
    {
      data->outstanding_requests++;
      command = new_command_stream (backend,
                                    SSH_FXP_READLINK);
      abs_name = g_build_filename (enum_job->filename, g_file_info_get_name (info), NULL);
      put_string (command, abs_name);
      g_free (abs_name);
      queue_command_stream_and_free (&backend->command_connection, command,
                                     read_dir_readlink_reply,
                                     G_VFS_JOB (job), g_object_ref (info));
    }
  else
    g_vfs_job_enumerate_add_info (enum_job, info);
}


static void
read_dir_symlink_reply (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job,
                        gpointer user_data)
{
  const char *name;
  GFileInfo *info;
  GFileInfo *lstat_info;
  ReadDirData *data;

  lstat_info = user_data;
  name = g_file_info_get_name (lstat_info);
  data = job->backend_data;
  
  if (reply_type == SSH_FXP_ATTRS)
    {
      info = g_file_info_new ();
      g_file_info_set_name (info, name);
      g_file_info_set_is_symlink (info, TRUE);
      
      parse_attributes (backend, info, name, reply, G_VFS_JOB_ENUMERATE (job)->attribute_matcher);

      read_dir_got_stat_info (backend, job, info);
      
      g_object_unref (info);
    }
  else
    read_dir_got_stat_info (backend, job, lstat_info);

  g_object_unref (lstat_info);
  
  if (--data->outstanding_requests == 0)
    g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
}

static void
read_dir_reply (GVfsBackendSftp *backend,
                int reply_type,
                GDataInputStream *reply,
                guint32 len,
                GVfsJob *job,
                gpointer user_data)
{
  GVfsJobEnumerate *enum_job;
  guint32 count;
  int i;
  GDataOutputStream *command;
  ReadDirData *data;

  data = job->backend_data;
  enum_job = G_VFS_JOB_ENUMERATE (job);

  if (reply_type != SSH_FXP_NAME)
    {
      /* Ignore all error, including the expected END OF FILE.
       * Real errors are expected in open_dir anyway */

      /* Close handle */

      command = new_command_stream (backend,
                                    SSH_FXP_CLOSE);
      put_data_buffer (command, data->handle);
      queue_command_stream_and_free (&backend->command_connection, command,
                                     NULL,
                                     G_VFS_JOB (job), NULL);
  
      if (--data->outstanding_requests == 0)
        g_vfs_job_enumerate_done (enum_job);
      
      return;
    }

  count = g_data_input_stream_read_uint32 (reply, NULL, NULL);
  for (i = 0; i < count; i++)
    {
      GFileInfo *info;
      char *name;
      char *longname;
      char *abs_name;

      info = g_file_info_new ();
      name = read_string (reply, NULL);
      g_file_info_set_name (info, name);
      
      longname = read_string (reply, NULL);
      g_free (longname);
      
      parse_attributes (backend, info, name, reply, enum_job->attribute_matcher);
      
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK &&
          ! (enum_job->flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS))
        {
          /* Default (at least for openssh) is for readdir to not follow symlinks.
             This was a symlink, and follow links was requested, so we need to manually follow it */
          command = new_command_stream (backend,
                                        SSH_FXP_STAT);
          abs_name = g_build_filename (enum_job->filename, name, NULL);
          put_string (command, abs_name);
          g_free (abs_name);
          
          queue_command_stream_and_free (&backend->command_connection, command,
                                         read_dir_symlink_reply,
                                         G_VFS_JOB (job), g_object_ref (info));
          data->outstanding_requests ++;
        }
      else if (strcmp (".", name) != 0 &&
               strcmp ("..", name) != 0)
        read_dir_got_stat_info (backend, job, info);
        
      g_object_unref (info);
      g_free (name);
    }

  command = new_command_stream (backend,
                                SSH_FXP_READDIR);
  put_data_buffer (command, data->handle);
  queue_command_stream_and_free (&backend->command_connection, command,
                                 read_dir_reply,
                                 G_VFS_JOB (job), NULL);
}

static void
open_dir_error (GVfsBackendSftp *backend,
		GVfsJob *job,
		gint original_error,
		gint stat_error,
		GFileInfo *info,
		gpointer user_data)
{
  if ((original_error == SSH_FX_FAILURE ||
       original_error == SSH_FX_NO_SUCH_FILE) &&
      info != NULL &&
      g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
		        G_IO_ERROR,
			G_IO_ERROR_NOT_DIRECTORY,
			_("The file is not a directory"));
      return;
    }

  result_from_status_code (job, original_error, -1, -1);
}

static void
open_dir_reply (GVfsBackendSftp *backend,
                int reply_type,
                GDataInputStream *reply,
                guint32 len,
                GVfsJob *job,
                gpointer user_data)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  ReadDirData *data;

  data = job->backend_data;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      error_from_lstat (backend, job, read_status_code (reply),
			G_VFS_JOB_ENUMERATE (job)->filename,
			open_dir_error,
			NULL);
      return;
    }

  if (reply_type != SSH_FXP_HANDLE)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  data->handle = read_data_buffer (reply);
  
  command = new_command_stream (op_backend,
                                SSH_FXP_READDIR);
  put_data_buffer (command, data->handle);

  data->outstanding_requests = 1;
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 read_dir_reply,
                                 G_VFS_JOB (job), NULL);
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  ReadDirData *data;

  data = g_slice_new0 (ReadDirData);

  g_vfs_job_set_backend_data (G_VFS_JOB (job), data, (GDestroyNotify)read_dir_data_free);
  command = new_command_stream (op_backend,
                                SSH_FXP_OPENDIR);
  put_string (command, filename);
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 open_dir_reply,
                                 G_VFS_JOB (job), NULL);

  return TRUE;
}

static void
query_info_reply (GVfsBackendSftp *backend,
                  MultiReply *replies,
                  int n_replies,
                  GVfsJob *job,
                  gpointer user_data)
{
  char *basename;
  int i;
  MultiReply *lstat_reply, *reply;
  GFileInfo *lstat_info;
  GVfsJobQueryInfo *op_job;

  op_job = G_VFS_JOB_QUERY_INFO (job);
  
  i = 0;
  lstat_reply = &replies[i++];

  if (lstat_reply->type == SSH_FXP_STATUS)
    {
      result_from_status (job, lstat_reply->data, -1, -1);
      return;
    }
  else if (lstat_reply->type != SSH_FXP_ATTRS)
    {
      g_vfs_job_failed (job,
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        "%s", _("Invalid reply received"));
      return;
    }

  basename = NULL;
  if (strcmp (op_job->filename, "/") != 0)
    basename = g_path_get_basename (op_job->filename);

  if (op_job->flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
    {
      parse_attributes (backend, op_job->file_info, basename,
                        lstat_reply->data, op_job->attribute_matcher);
    }
  else
    {
      /* Look at stat results */
      reply = &replies[i++];

      if (reply->type == SSH_FXP_ATTRS)
        {
          parse_attributes (backend, op_job->file_info, basename,
                            reply->data, op_job->attribute_matcher);

          
          lstat_info = g_file_info_new ();
          parse_attributes (backend, lstat_info, basename,
                            lstat_reply->data, op_job->attribute_matcher);
          if (g_file_info_get_is_symlink (lstat_info))
            g_file_info_set_is_symlink (op_job->file_info, TRUE);
          g_object_unref (lstat_info);
        }
      else
        {
          /* Broken symlink, use lstat data */
          parse_attributes (backend, op_job->file_info, basename,
                            lstat_reply->data, op_job->attribute_matcher);
        }
      
    }
    
  g_free (basename);

  if (g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
    {
      /* Look at readlink results */
      reply = &replies[i++];

      if (reply->type == SSH_FXP_NAME)
        {
          char *symlink_target;
          
          /* Skip count (always 1 for replies to SSH_FXP_READLINK) */
          g_data_input_stream_read_uint32 (reply->data, NULL, NULL);
          symlink_target = read_string (reply->data, NULL);
          g_file_info_set_symlink_target (op_job->file_info, symlink_target);
          g_free (symlink_target);
        }
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  Command commands[3];
  GDataOutputStream *command;
  int n_commands;

  n_commands = 0;
  
  commands[n_commands].connection = &op_backend->command_connection;
  command = commands[n_commands++].cmd =
    new_command_stream (op_backend,
                        SSH_FXP_LSTAT);
  put_string (command, filename);
  
  if (! (job->flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS))
    {
      commands[n_commands].connection = &op_backend->command_connection;
      command = commands[n_commands++].cmd =
        new_command_stream (op_backend,
                            SSH_FXP_STAT);
      put_string (command, filename);
    }

  if (g_file_attribute_matcher_matches (job->attribute_matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
    {
      commands[n_commands].connection = &op_backend->command_connection;
      command = commands[n_commands++].cmd =
        new_command_stream (op_backend,
                            SSH_FXP_READLINK);
      put_string (command, filename);
    }

  queue_command_streams_and_free (commands, n_commands,
                                  query_info_reply,
                                  G_VFS_JOB (job), NULL);
  
  return TRUE;
}

static void
query_fs_info_reply (GVfsBackendSftp *backend,
                     int reply_type,
                     GDataInputStream *reply,
                     guint32 len,
                     GVfsJob *job,
                     gpointer user_data)
{
  GFileInfo *info = user_data;
  guint64 frsize, blocks, bfree, bavail, flags;

  if (reply_type == SSH_FXP_STATUS)
    {
      guint32 code = read_status_code (reply);

      if (code == SSH_FX_NO_SUCH_FILE)
        not_dir_or_not_exist_error (backend, job, G_VFS_JOB_QUERY_FS_INFO (job)->filename);
      else
        result_from_status_code (job, code, -1, -1);
      return;
    }
  else if (reply_type != SSH_FXP_EXTENDED_REPLY)
    {
      g_vfs_job_failed (job,
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        "%s", _("Invalid reply received"));
      return;
    }

  g_data_input_stream_read_uint64 (reply, NULL, NULL); /* bsize */
  frsize = g_data_input_stream_read_uint64 (reply, NULL, NULL);
  blocks = g_data_input_stream_read_uint64 (reply, NULL, NULL);
  bfree = g_data_input_stream_read_uint64 (reply, NULL, NULL);
  bavail = g_data_input_stream_read_uint64 (reply, NULL, NULL);
  g_data_input_stream_read_uint64 (reply, NULL, NULL); /* files */
  g_data_input_stream_read_uint64 (reply, NULL, NULL); /* ffree */
  g_data_input_stream_read_uint64 (reply, NULL, NULL); /* favail */
  g_data_input_stream_read_uint64 (reply, NULL, NULL); /* fsid */
  flags = g_data_input_stream_read_uint64 (reply, NULL, NULL);
  g_data_input_stream_read_uint64 (reply, NULL, NULL); /* namemax */

  /* If free and available are both 0, treat it like the size information is
   * missing.
   * */
  if (bfree || bavail)
    {
      g_file_info_set_attribute_uint64 (info,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                        frsize * bavail);

      g_file_info_set_attribute_uint64 (info,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
                                        frsize * blocks);

      g_file_info_set_attribute_uint64 (info,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_USED,
                                        frsize * (blocks - bfree));
    }

  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_FILESYSTEM_READONLY,
                                     flags & SSH_FXE_STATVFS_ST_RDONLY);

  g_vfs_job_succeeded (job);
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  g_file_info_set_attribute_string (info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "sftp");
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW,
                                    G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS);

  if (has_extension (op_backend, SFTP_EXT_OPENSSH_STATVFS) &&
      (g_file_attribute_matcher_matches (matcher,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
       g_file_attribute_matcher_matches (matcher,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_FREE) ||
       g_file_attribute_matcher_matches (matcher,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_USED) ||
       g_file_attribute_matcher_matches (matcher,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_READONLY)))
    {
      command = new_command_stream (op_backend, SSH_FXP_EXTENDED);
      put_string (command, "statvfs@openssh.com");
      put_string (command, filename);

      queue_command_stream_and_free (&op_backend->command_connection, command,
                                     query_fs_info_reply,
                                     G_VFS_JOB (job), info);
    }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

typedef struct {
   GFileInfo *info;
   GFileAttributeMatcher *attribute_matcher;
} QueryInfoFStatData;

static void
query_info_fstat_reply (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job,
                        gpointer user_data)
{
  QueryInfoFStatData *data = user_data;
  GFileInfo *file_info;
  GFileAttributeMatcher *attribute_matcher;

  file_info = data->info;
  attribute_matcher = data->attribute_matcher;
  g_slice_free (QueryInfoFStatData, data);

  if (reply_type == SSH_FXP_STATUS)
    {
      result_from_status (job, reply, -1, -1);
      return;
    }

  if (reply_type != SSH_FXP_ATTRS)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
      return;
    }

  parse_attributes (backend,
                    file_info,
                    NULL,
                    reply,
                    attribute_matcher);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_info_fstat (GVfsBackend *backend,
                      GVfsJob *job,
                      GVfsBackendHandle _handle,
                      GFileInfo *info,
                      GFileAttributeMatcher *attribute_matcher)
{
  SftpHandle *handle = _handle;
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  QueryInfoFStatData *data;

  command = new_command_stream (op_backend, SSH_FXP_FSTAT);
  put_data_buffer (command, handle->raw_handle);

  data = g_slice_new (QueryInfoFStatData);
  data->info = info;
  data->attribute_matcher = attribute_matcher;
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 query_info_fstat_reply,
                                 G_VFS_JOB (job), data);

  return TRUE;
}

static void
move_reply (GVfsBackendSftp *backend,
            int reply_type,
            GDataInputStream *reply,
            guint32 len,
            GVfsJob *job,
            gpointer user_data)
{
  goffset *file_size;

  /* on any unknown error, return NOT_SUPPORTED to get the fallback implementation */
  if (reply_type == SSH_FXP_STATUS)
    {
      if (failure_from_status (job, reply, G_IO_ERROR_NOT_SUPPORTED, -1))
        {
          /* Succeeded, report file size */
          file_size = job->backend_data;
          if (file_size != NULL) 
            g_vfs_job_progress_callback (*file_size, *file_size, job);
          g_vfs_job_succeeded (job);
        }
    }
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static void
move_do_rename (GVfsBackendSftp *backend,
                GVfsJob *job)
{
  GVfsJobMove *op_job;
  GDataOutputStream *command;

  op_job = G_VFS_JOB_MOVE (job);

  command = new_command_stream (backend,
                                SSH_FXP_RENAME);
  put_string (command, op_job->source);
  put_string (command, op_job->destination);

  queue_command_stream_and_free (&backend->command_connection, command,
                                 move_reply,
                                 G_VFS_JOB (job), NULL);
}

static void
move_delete_target_reply (GVfsBackendSftp *backend,
                          int reply_type,
                          GDataInputStream *reply,
                          guint32 len,
                          GVfsJob *job,
                          gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    {
      if (failure_from_status (job, reply, -1, -1))
        move_do_rename (backend, job);
    }
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}


static void
move_lstat_reply (GVfsBackendSftp *backend,
                  MultiReply *replies,
                  int n_replies,
                  GVfsJob *job,
                  gpointer user_data)
{
  GVfsJobMove *op_job;
  gboolean destination_exist, source_is_dir, dest_is_dir;
  GDataOutputStream *command;
  GFileInfo *info;
  goffset *file_size;

  op_job = G_VFS_JOB_MOVE (job);
  
  if (replies[0].type == SSH_FXP_STATUS)
    {
      result_from_status (job, replies[0].data, -1, -1);
      return;
    }
  else if (replies[0].type != SSH_FXP_ATTRS)
    {
      g_vfs_job_failed (job,
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        "%s", _("Invalid reply received"));
      return;
    }

  info = g_file_info_new ();
  parse_attributes (backend, info, NULL,
                    replies[0].data, NULL);
  source_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
  file_size = g_new (goffset, 1);
  *file_size = g_file_info_get_size (info);
  g_vfs_job_set_backend_data (G_VFS_JOB (job), file_size, g_free);
  g_object_unref (info);
  
  destination_exist = FALSE;
  if (replies[1].type == SSH_FXP_ATTRS)
    {
      destination_exist = TRUE; /* Target file exists */

      info = g_file_info_new ();
      parse_attributes (backend, info, NULL,
                        replies[1].data, NULL);
      dest_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
      g_object_unref (info);
      
      if (op_job->flags & G_FILE_COPY_OVERWRITE)
	{
          
	  /* Always fail on dirs, even with overwrite */
	  if (dest_is_dir)
	    {
              if (source_is_dir)
                g_vfs_job_failed (job,
                                  G_IO_ERROR,
                                  G_IO_ERROR_WOULD_MERGE,
                                  _("Can’t move directory over directory"));
              else
                g_vfs_job_failed (job,
                                  G_IO_ERROR,
                                  G_IO_ERROR_IS_DIRECTORY,
                                  _("File is directory"));
	      return;
	    }
	}
      else
	{
	  g_vfs_job_failed (G_VFS_JOB (job),
			    G_IO_ERROR,
			    G_IO_ERROR_EXISTS,
			    _("Target file already exists"));
	  return;
	}
    }

  /* TODO: Check flags & G_FILE_COPY_BACKUP */

  if (destination_exist && (op_job->flags & G_FILE_COPY_OVERWRITE))
    {
      command = new_command_stream (backend,
                                    SSH_FXP_REMOVE);
      put_string (command, op_job->destination);
      queue_command_stream_and_free (&backend->command_connection, command,
                                     move_delete_target_reply,
                                     G_VFS_JOB (job), NULL);
      return;
    }

  move_do_rename (backend, job);
}


static gboolean
try_move (GVfsBackend *backend,
          GVfsJobMove *job,
          const char *source,
          const char *destination,
          GFileCopyFlags flags,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  Command commands[2];

  commands[0].connection = &op_backend->command_connection;
  command = commands[0].cmd =
    new_command_stream (op_backend,
                        SSH_FXP_LSTAT);
  put_string (command, source);

  commands[1].connection = &op_backend->command_connection;
  command = commands[1].cmd =
    new_command_stream (op_backend,
                        SSH_FXP_LSTAT);
  put_string (command, destination);

  queue_command_streams_and_free (commands, 2,
                                  move_lstat_reply,
                                  G_VFS_JOB (job), NULL);
  
  return TRUE;
}

static void
set_display_name_reply (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job,
                        gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static gboolean
try_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  char *dirname, *basename, *new_name;

  /* We use the same setting as for local files. Can't really
   * do better, since there is no way in this version of sftp to find out
   * the remote charset encoding
   */
  
  dirname = g_path_get_dirname (filename);
  basename = g_filename_from_utf8 (display_name, -1, NULL, NULL, NULL);
  if (basename == NULL)
    basename = g_strdup (display_name);
  new_name = g_build_filename (dirname, basename, NULL);
  g_free (dirname);
  g_free (basename);

  g_vfs_job_set_display_name_set_new_path (job,
                                           new_name);
  
  command = new_command_stream (op_backend,
                                SSH_FXP_RENAME);
  put_string (command, filename);
  put_string (command, new_name);
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 set_display_name_reply,
                                 G_VFS_JOB (job), NULL);

  g_free (new_name);

  return TRUE;
}


static void
make_symlink_reply (GVfsBackendSftp *backend,
                    int reply_type,
                    GDataInputStream *reply,
                    guint32 len,
                    GVfsJob *job,
                    gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, G_IO_ERROR_EXISTS, -1);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static gboolean
try_make_symlink (GVfsBackend *backend,
                  GVfsJobMakeSymlink *job,
                  const char *filename,
                  const char *symlink_value)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  
  command = new_command_stream (op_backend,
                                SSH_FXP_SYMLINK);
  /* Note: This is the reverse order of how this is documented in
     draft-ietf-secsh-filexfer-02.txt, but its how openssh does it. */
  put_string (command, symlink_value);
  put_string (command, filename);
  
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 make_symlink_reply,
                                 G_VFS_JOB (job), NULL);

  return TRUE;
}

static void
mkdir_stat_reply (GVfsBackendSftp *backend,
                  int reply_type,
                  GDataInputStream *reply,
                  guint32 len,
                  GVfsJob *job,
                  gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    /* We got some error, but lets report the original FAILURE, as
       these extra errors are not really mkdir errors, we just wanted
       to implement EEXISTS */
    result_from_status_code (job, SSH_FX_FAILURE, -1, -1);
  else if (reply_type != SSH_FXP_ATTRS)
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_EXISTS,
                      _("Target file exists"));
}

static void
make_directory_reply (GVfsBackendSftp *backend,
                      int reply_type,
                      GDataInputStream *reply,
                      guint32 len,
                      GVfsJob *job,
                      gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    {
      gint stat_error;

      stat_error = read_status_code (reply);
      if (stat_error == SSH_FX_FAILURE)
        {
          /* Generic SFTP error, let's stat the target */
          GDataOutputStream *command;

          command = new_command_stream (backend,
                                        SSH_FXP_LSTAT);
          put_string (command, G_VFS_JOB_MAKE_DIRECTORY (job)->filename);
          queue_command_stream_and_free (&backend->command_connection, command,
                                         mkdir_stat_reply,
                                         G_VFS_JOB (job), NULL);
        }
      else
        result_from_status_code (job, stat_error, -1, -1);
    }
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static gboolean
try_make_directory (GVfsBackend *backend,
                    GVfsJobMakeDirectory *job,
                    const char *filename)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend,
                                SSH_FXP_MKDIR);
  put_string (command, filename);
  /* No file info - flag 0 */
  g_data_output_stream_put_uint32 (command, 0, NULL, NULL);

  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 make_directory_reply,
                                 G_VFS_JOB (job), NULL);

  return TRUE;
}

static void
delete_remove_reply (GVfsBackendSftp *backend,
                     int reply_type,
                     GDataInputStream *reply,
                     guint32 len,
                     GVfsJob *job,
                     gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1); 
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static void
delete_rmdir_reply (GVfsBackendSftp *backend,
                    int reply_type,
                    GDataInputStream *reply,
                    guint32 len,
                    GVfsJob *job,
                    gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, G_IO_ERROR_NOT_EMPTY, -1); 
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static void
delete_lstat_reply (GVfsBackendSftp *backend,
                    int reply_type,
                    GDataInputStream *reply,
                    guint32 len,
                    GVfsJob *job,
                    gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else if (reply_type != SSH_FXP_ATTRS)
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
  else
    {
      GFileInfo *info;
      GDataOutputStream *command;

      info = g_file_info_new ();
      parse_attributes (backend, info, NULL, reply, NULL);

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        {
          command = new_command_stream (backend,
                                        SSH_FXP_RMDIR);
          put_string (command, G_VFS_JOB_DELETE (job)->filename);
          queue_command_stream_and_free (&backend->command_connection, command,
                                         delete_rmdir_reply,
                                         G_VFS_JOB (job), NULL);
        }
      else
        {
          command = new_command_stream (backend,
                                        SSH_FXP_REMOVE);
          put_string (command, G_VFS_JOB_DELETE (job)->filename);
          queue_command_stream_and_free (&backend->command_connection, command,
                                         delete_remove_reply,
                                         G_VFS_JOB (job), NULL);
        }

      g_object_unref (info);
    }
}

static gboolean
try_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  
  command = new_command_stream (op_backend,
                                SSH_FXP_LSTAT);
  put_string (command, filename);
  queue_command_stream_and_free (&op_backend->command_connection, command,
                                 delete_lstat_reply,
                                 G_VFS_JOB (job), NULL);

  return TRUE;
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
			       GVfsJobQueryAttributes *job,
			       const char *filename)
{
  GFileAttributeInfoList *list;

  list = g_file_attribute_info_list_new ();

  g_file_attribute_info_list_add (list,
				  G_FILE_ATTRIBUTE_UNIX_MODE,
				  G_FILE_ATTRIBUTE_TYPE_UINT32,
				  G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
				  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  
  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                  G_FILE_ATTRIBUTE_TYPE_UINT64,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);

  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_TIME_ACCESS,
                                  G_FILE_ATTRIBUTE_TYPE_UINT64,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);

  g_vfs_job_query_attributes_set_list (job, list);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_file_attribute_info_list_unref (list);
  
  return TRUE;
}

static void
set_attribute_reply (GVfsBackendSftp *backend,
		     int reply_type,
		     GDataInputStream *reply,
		     guint32 len,
		     GVfsJob *job,
		     gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else 
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
		      _("Invalid reply received"));
}

static void
set_attribute_stat_reply (GVfsBackendSftp *backend,
                          int reply_type,
                          GDataInputStream *reply,
                          guint32 len,
                          GVfsJob *job,
                          gpointer user_data)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;
  GVfsJobSetAttribute *op_job = G_VFS_JOB_SET_ATTRIBUTE (job);

  if (reply_type == SSH_FXP_ATTRS)
    {
      guint32 mtime;
      guint32 atime;
      GFileInfo *info = g_file_info_new ();

      parse_attributes (backend, info, NULL, reply, NULL);

      /* parse_attributes sets either both timestamps or none
       * so checking one of them is enough. */
      if (!g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        {
          g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            _("Operation not supported"));
          g_object_unref (info);
          return;
        }
      /* Timestamps must be read as uint64 but sftp only supports uint32 */
      if (op_job->value.uint64 > G_MAXUINT32)
        {
          g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                            _("Value out of range, sftp only supports 32bit timestamps"));
          g_object_unref (info);
          return;
        }

      if (g_strcmp0 (op_job->attribute, G_FILE_ATTRIBUTE_TIME_ACCESS) == 0)
        {
          mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
          atime = op_job->value.uint64;
        }
      else
        {
          atime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS);
          mtime = op_job->value.uint64;
        }

      g_object_unref (info);

      command = new_command_stream (op_backend, SSH_FXP_SETSTAT);
      put_string (command, op_job->filename);

      g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_ACMODTIME, NULL, NULL);
      g_data_output_stream_put_uint32 (command, atime, NULL, NULL);
      g_data_output_stream_put_uint32 (command, mtime, NULL, NULL);
      queue_command_stream_and_free (&op_backend->command_connection, command,
                                     set_attribute_reply,
                                     G_VFS_JOB (job), NULL);
    }
  else if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static gboolean
try_set_attribute (GVfsBackend *backend,
		   GVfsJobSetAttribute *job,
		   const char *filename,
		   const char *attribute,
		   GFileAttributeType type,
		   gpointer value_p,
		   GFileQueryInfoFlags flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  if (g_strcmp0 (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0)
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT32)
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            _("Invalid attribute type (uint32 expected)"));
          return TRUE;
        }

      command = new_command_stream (op_backend,
                                    SSH_FXP_SETSTAT);
      put_string (command, filename);
      g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_PERMISSIONS, NULL, NULL);
      g_data_output_stream_put_uint32 (command, (*(guint32 *)value_p) & 0777, NULL, NULL);
      queue_command_stream_and_free (&op_backend->command_connection, command,
                                     set_attribute_reply,
                                     G_VFS_JOB (job), NULL);
    }
  else if (g_strcmp0 (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED) == 0 ||
           g_strcmp0 (attribute, G_FILE_ATTRIBUTE_TIME_ACCESS) == 0)
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT64)
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            _("Invalid attribute type (uint64 expected)"));
          return TRUE;
        }

      command = new_command_stream (op_backend, SSH_FXP_LSTAT);
      put_string (command, filename);

      queue_command_stream_and_free (&op_backend->command_connection, command,
                                     set_attribute_stat_reply,
                                     G_VFS_JOB (job), NULL);
    }
  else
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
    }

  return TRUE;
}

/* Return true if the job is finished or cancelled, failing it if needed. */
static gboolean
check_finished_or_cancelled_job (GVfsJob *job)
{
  if (g_vfs_job_is_finished (job))
    return TRUE;

  if (g_vfs_job_is_cancelled (job))
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      return TRUE;
    }

  return FALSE;
}

/* The push sliding window mechanism is based on the one in the OpenSSH sftp
 * client. */

#define PUSH_MAX_REQUESTS 64

typedef struct {
  /* Job context */
  GVfsBackendSftp *backend;
  GVfsJobPush *op_job;
  GVfsJob *job;

  /* Open files */
  DataBuffer *raw_handle;
  GInputStream *in;

  /* fstat information */
  goffset size;
  guint32 permissions;
  guint64 mtime;
  guint64 atime;

  /* state */
  goffset offset;
  goffset n_written;
  int num_req;

  /* replace data */
  char *tempname;
  int temp_count;

  char buffer[MAX_BUFFER_SIZE];
} SftpPushHandle;

typedef struct {
  SftpPushHandle *handle;
  gssize count;
} PushWriteRequest;

static void
sftp_push_handle_free (SftpPushHandle *handle)
{
  GDataOutputStream *command;

  /* Only free the handle if there are no write requests outstanding and no
   * asynchronous reads pending. */
  if (handle->num_req == 0 && (!handle->in || !g_input_stream_has_pending (handle->in)))
    {
      if (handle->in)
        {
          g_input_stream_close_async (handle->in, 0, NULL, NULL, NULL);
          g_object_unref (handle->in);
        }

      /* If raw_handle is non-NULL, it means destination is still open. Close
       * it. */
      if (handle->raw_handle)
        {
          command = new_command_stream (handle->backend, SSH_FXP_CLOSE);
          put_data_buffer (command, handle->raw_handle);
          queue_command_stream_and_free (&handle->backend->data_connection,
                                         command,
                                         NULL,
                                         handle->job, NULL);
          data_buffer_free (handle->raw_handle);
        }

      /* If tempname is non-NULL, it means we failed and should delete the temp
       * file. */
      if (handle->tempname)
        {
          command = new_command_stream (handle->backend, SSH_FXP_REMOVE);
          put_string (command, handle->tempname);
          queue_command_stream_and_free (&handle->backend->command_connection,
                                         command,
                                         NULL,
                                         handle->job, NULL);

          g_free (handle->tempname);
        }

      g_object_unref (handle->backend);
      g_object_unref (handle->job);
      g_slice_free (SftpPushHandle, handle);
    }
}

static void
push_read_cb (GObject *source, GAsyncResult *res, gpointer user_data);

static void
push_enqueue_request (SftpPushHandle *handle)
{
  g_input_stream_read_async (handle->in,
                             handle->buffer,
                             MAX_BUFFER_SIZE,
                             G_PRIORITY_DEFAULT,
                             NULL,
                             push_read_cb, handle);
  handle->num_req++;
}

static void
push_close_moved_file (GVfsBackendSftp *backend,
                       int reply_type,
                       GDataInputStream *reply,
                       guint32 len,
                       GVfsJob *job,
                       gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (reply_type == SSH_FXP_STATUS)
    {
      guint32 code = read_status_code (reply);
      if (code == SSH_FX_OK)
        {
          if (handle->op_job->remove_source)
            g_unlink (handle->op_job->local_path);

          g_vfs_job_succeeded (job);
        }
      else
        result_from_status_code (job, code, -1, -1);
    }
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));

  sftp_push_handle_free (handle);
}

static void
push_close_deleted_file (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job,
                        gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (reply_type == SSH_FXP_STATUS)
    {
      guint32 code = read_status_code (reply);
      if (code == SSH_FX_OK)
        {
          /* The delete completed successfully, now rename. */
          GDataOutputStream *command = new_command_stream (backend, SSH_FXP_RENAME);
          put_string (command, handle->tempname);
          put_string (command, handle->op_job->destination);
          queue_command_stream_and_free (&backend->command_connection, command,
                                         push_close_moved_file,
                                         job, handle);

          g_free (handle->tempname);
          handle->tempname = NULL;
          return;
        }
      else
        result_from_status_code (job, code, -1, -1);
    }
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));

  sftp_push_handle_free (handle);
}

static void
push_close_delete_or_succeed (GVfsBackendSftp *backend,
                              int reply_type,
                              GDataInputStream *reply,
                              guint32 len,
                              GVfsJob *job,
                              gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (handle->tempname)
    {
      /* If we wrote to a temp file, do delete then rename. */
      GDataOutputStream *command = new_command_stream (handle->backend, SSH_FXP_REMOVE);
      put_string (command, handle->op_job->destination);
      queue_command_stream_and_free (&handle->backend->command_connection, command,
                                     push_close_deleted_file,
                                     handle->job, handle);
    }
  else
    {
      if (handle->op_job->remove_source)
        g_unlink (handle->op_job->local_path);

      g_vfs_job_succeeded (handle->job);
      sftp_push_handle_free (handle);
    }
}

static void
push_close_restore_permissions (SftpPushHandle *handle)
{
  gboolean default_perms = (handle->op_job->flags & G_FILE_COPY_TARGET_DEFAULT_PERMS);
  guint32 flags = SSH_FILEXFER_ATTR_ACMODTIME;

  if (!default_perms)
    flags |= SSH_FILEXFER_ATTR_PERMISSIONS;

  /* Restore the source file's permissions and timestamps. */
  GDataOutputStream *command = new_command_stream (handle->backend, SSH_FXP_SETSTAT);
  put_string (command, handle->tempname ? handle->tempname : handle->op_job->destination);
  g_data_output_stream_put_uint32 (command, flags, NULL, NULL);
  if (!default_perms)
    g_data_output_stream_put_uint32 (command, handle->permissions, NULL, NULL);
  g_data_output_stream_put_uint32 (command, handle->atime, NULL, NULL);
  g_data_output_stream_put_uint32 (command, handle->mtime, NULL, NULL);
  queue_command_stream_and_free (&handle->backend->command_connection, command,
                                 push_close_delete_or_succeed,
                                 handle->job, handle);
}

static void
push_close_stat_reply (GVfsBackendSftp *backend,
                       int reply_type,
                       GDataInputStream *reply,
                       guint32 len,
                       GVfsJob *job,
                       gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (reply_type == SSH_FXP_ATTRS)
    {
      GFileInfo *info = g_file_info_new ();

      parse_attributes (backend, info, NULL, reply, NULL);

      /* Don't fail on error, but fall back to the local atime
       * (assigned in push_source_fstat_cb). */
      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_ACCESS))
        handle->atime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS);

      g_object_unref (info);
    }

  push_close_restore_permissions (handle);
}

static void
push_close_write_reply (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job,
                        gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (reply_type == SSH_FXP_STATUS)
    {
      guint32 code = read_status_code (reply);
      if (code == SSH_FX_OK)
        {
          /* Atime is COPY_WHEN_MOVED, but not COPY_WITH_FILE. */
          if (!handle->op_job->remove_source &&
              !(handle->op_job->flags & G_FILE_COPY_ALL_METADATA))
            {
              GDataOutputStream *command = new_command_stream (backend, SSH_FXP_LSTAT);
              put_string (command, handle->op_job->destination);
              queue_command_stream_and_free (&backend->command_connection, command,
                                             push_close_stat_reply,
                                             job, handle);
              return;
            }

          push_close_restore_permissions (handle);
          return;
        }
      else
        result_from_status_code (job, code, -1, -1);
    }
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));

  sftp_push_handle_free (handle);
}

static void
push_finish (SftpPushHandle *handle)
{
  GDataOutputStream *command = new_command_stream (handle->backend, SSH_FXP_CLOSE);
  put_data_buffer (command, handle->raw_handle);
  queue_command_stream_and_free (&handle->backend->data_connection, command,
                                 push_close_write_reply,
                                 handle->job, handle);

  data_buffer_free (handle->raw_handle);
  handle->raw_handle = NULL;
}

static void
push_source_close_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  g_input_stream_close_finish (handle->in, res, NULL);
  g_clear_object (&handle->in);

  if (check_finished_or_cancelled_job (handle->job))
    {
      sftp_push_handle_free (handle);
      return;
    }

  /* If there are no write requests outstanding, we are done. */
  if (handle->num_req == 0)
    push_finish(handle);
}

static void
push_write_reply (GVfsBackendSftp *backend,
                  int reply_type,
                  GDataInputStream *reply,
                  guint32 len,
                  GVfsJob *job,
                  gpointer user_data)
{
  PushWriteRequest *request = user_data;
  SftpPushHandle *handle = request->handle;
  gssize count = request->count;

  g_slice_free (PushWriteRequest, request);

  handle->num_req--;

  if (check_finished_or_cancelled_job (job))
    {
      sftp_push_handle_free (handle);
      return;
    }

  if (reply_type == SSH_FXP_STATUS)
    {
      guint32 code = read_status_code (reply);

      if (code == SSH_FX_OK)
        {
          handle->n_written += count;
          g_vfs_job_progress_callback (handle->n_written, handle->size, job);

          /* Enqueue a read op if the file is still open, and there isn't
           * already one pending. */
          if (handle->in && !g_input_stream_has_pending (handle->in))
            push_enqueue_request (handle);

          /* We are done if the file is closed and there are no write requests
           * oustanding. */
          if (!handle->in && handle->num_req == 0)
            {
              push_finish (handle);
              return;
            }
        }
      else
        result_from_status_code (handle->job, code, -1, -1);
    }
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));

  sftp_push_handle_free (handle);
}

static void
push_read_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PushWriteRequest *request;
  GDataOutputStream *command;
  gssize count;

  SftpPushHandle *handle = user_data;
  GError *error = NULL;

  count = g_input_stream_read_finish (handle->in, res, &error);

  if (check_finished_or_cancelled_job (handle->job))
    {
      g_clear_error (&error);
      sftp_push_handle_free (handle);
      return;
    }

  if (error)
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);

      sftp_push_handle_free (handle);
      return;
    }

  if (count == 0)
    {
      handle->num_req--;

      g_input_stream_close_async (handle->in,
                                  0, NULL,
                                  push_source_close_cb, handle);
      return;
    }

  request = g_slice_new (PushWriteRequest);
  request->handle = handle;
  request->count = count;

  command = new_command_stream (handle->backend, SSH_FXP_WRITE);
  put_data_buffer (command, handle->raw_handle);
  g_data_output_stream_put_uint64 (command, handle->offset, NULL, NULL);
  g_data_output_stream_put_uint32 (command, count, NULL, NULL);
  g_output_stream_write_all (G_OUTPUT_STREAM (command),
                             handle->buffer, count,
                             NULL, NULL, NULL);
  queue_command_stream_and_free (&handle->backend->data_connection, command,
                                 push_write_reply,
                                 handle->job, request);
  handle->offset += count;

  if (handle->num_req < PUSH_MAX_REQUESTS)
    push_enqueue_request (handle);
}

static void push_create_temp (SftpPushHandle *handle);

static void
push_truncate_original_reply (GVfsBackendSftp *backend,
                              int reply_type,
                              GDataInputStream *reply,
                              guint32 len,
                              GVfsJob *job,
                              gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (reply_type == SSH_FXP_HANDLE)
    {
      handle->raw_handle = read_data_buffer (reply);
      push_enqueue_request (handle);
    }
  else if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));

  sftp_push_handle_free (handle);
}

static void
push_create_temp_reply (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job,
                        gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (reply_type == SSH_FXP_STATUS)
    {
      guint32 code = read_status_code (reply);
      if (code == SSH_FX_PERMISSION_DENIED)
        {
          /* The temp file creation failed. Try truncating the existing file. */
          GDataOutputStream *command;

          g_free (handle->tempname);
          handle->tempname = NULL;

          command = new_command_stream (backend, SSH_FXP_OPEN);
          put_string (command, handle->op_job->destination);
          g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_TRUNC,  NULL, NULL);
          g_data_output_stream_put_uint32 (command, 0, NULL, NULL);
          queue_command_stream_and_free (&backend->data_connection, command,
                                         push_truncate_original_reply,
                                         job, handle);

          return;
        }
      else if (code == SSH_FX_FAILURE)
        {
          push_create_temp (handle);
          return;
        }
      else
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Invalid reply received"));
    }
  else if (reply_type != SSH_FXP_HANDLE)
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
  else
    {
      handle->raw_handle = read_data_buffer (reply);
      push_enqueue_request (handle);
    }

  sftp_push_handle_free (handle);
}

static void
push_create_temp (SftpPushHandle *handle)
{
  GDataOutputStream *command;
  char *dirname;
  char basename[] = ".giosaveXXXXXX";

  /* Write to a temp file and then rename to replace. */

  handle->temp_count++;

  if (handle->temp_count == 100)
    {
      g_vfs_job_failed (handle->job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Unable to create temporary file"));
      sftp_push_handle_free (handle);
      return;
    }

  g_free (handle->tempname);
  dirname = g_path_get_dirname (handle->op_job->destination);
  gvfs_randomize_string (basename + 8, 6);
  handle->tempname = g_build_filename (dirname, basename, NULL);
  g_free (dirname);

  command = new_command_stream (handle->backend, SSH_FXP_OPEN);
  put_string (command, handle->tempname);
  g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_EXCL,  NULL, NULL);
  g_data_output_stream_put_uint32 (command, 0, NULL, NULL);
  queue_command_stream_and_free (&handle->backend->data_connection, command,
                                 push_create_temp_reply,
                                 handle->job, handle);
}

static void
push_open_stat_reply (GVfsBackendSftp *backend,
                      int reply_type,
                      GDataInputStream *reply,
                      guint32 len,
                      GVfsJob *job,
                      gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (reply_type == SSH_FXP_ATTRS)
    {
      GFileInfo *info;
      GFileType type;

      info = g_file_info_new ();
      parse_attributes (backend, info, NULL, reply, NULL);
      type = g_file_info_get_file_type (info);
      g_object_unref (info);

      if (type == G_FILE_TYPE_DIRECTORY)
        {
          /* We cannot overwrite a directory. */
          if (handle->op_job->flags & G_FILE_COPY_OVERWRITE)
            g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                              _("File is directory"));

          sftp_push_handle_free (handle);
          return;
        }

      push_create_temp (handle);
    }
  else
    {
      result_from_status (job, reply, -1, -1);
      sftp_push_handle_free (handle);
    }
}

static void
push_open_reply (GVfsBackendSftp *backend,
                 int reply_type,
                 GDataInputStream *reply,
                 guint32 len,
                 GVfsJob *job,
                 gpointer user_data)
{
  SftpPushHandle *handle = user_data;

  if (reply_type == SSH_FXP_STATUS)
    {
      guint32 code = read_status_code (reply);
      if (code == SSH_FX_NO_SUCH_FILE)
        not_dir_or_not_exist_error (backend, job, handle->op_job->destination);
      else if (code == SSH_FX_FAILURE)
        {
          if (handle->op_job->flags & G_FILE_COPY_OVERWRITE)
            {
              /* The destination probably exists. Let's see if we can overwrite
               * it. */
              GDataOutputStream *command = new_command_stream (backend, SSH_FXP_LSTAT);
              put_string (command, handle->op_job->destination);
              queue_command_stream_and_free (&backend->command_connection, command,
                                             push_open_stat_reply,
                                             job, handle);
              return;
            }
          else
            result_from_status_code (job, code, G_IO_ERROR_EXISTS, -1);
        }
      else
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Invalid reply received"));
    }
  else if (reply_type != SSH_FXP_HANDLE)
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
  else
    {
      handle->raw_handle = read_data_buffer (reply);
      push_enqueue_request (handle);
    }

  sftp_push_handle_free (handle);
}

static void
push_source_fstat_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFileInputStream *fin = G_FILE_INPUT_STREAM (source);
  SftpPushHandle *handle = user_data;
  GError *error = NULL;
  GFileInfo *info;
  GDataOutputStream *command;

  info = g_file_input_stream_query_info_finish (fin, res, &error);
  if (info)
    {
      handle->permissions = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE) & 0777;
      handle->size = g_file_info_get_size (info);
      handle->mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
      handle->atime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS);

      command = new_command_stream (handle->backend, SSH_FXP_OPEN);
      put_string (command, handle->op_job->destination);
      g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_EXCL, NULL, NULL);
      g_data_output_stream_put_uint32 (command, 0, NULL, NULL);
      queue_command_stream_and_free (&handle->backend->data_connection, command,
                                     push_open_reply,
                                     handle->job, handle);
    }
  else
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
      sftp_push_handle_free (handle);
    }
}

static void
push_source_open_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *source_file = G_FILE (source);
  SftpPushHandle *handle = user_data;
  GError *error = NULL;
  GFileInputStream *fin;

  fin = g_file_read_finish (source_file, res, &error);
  if (fin)
    {
      handle->in = G_INPUT_STREAM (fin);

      g_file_input_stream_query_info_async (fin,
                                            G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                            G_FILE_ATTRIBUTE_UNIX_MODE ","
                                            G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                            G_FILE_ATTRIBUTE_TIME_ACCESS,
                                            0, NULL,
                                            push_source_fstat_cb, handle);
    }
  else
    {
      if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_IS_DIRECTORY)
        {
          /* Fall back to default implementation to improve the error message */
          g_vfs_job_failed (handle->job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            _("Operation not supported"));
        }
      else
        g_vfs_job_failed_from_error (handle->job, error);

      g_error_free (error);
      sftp_push_handle_free (handle);
    }
}

static void
push_source_lstat_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *source_file = G_FILE (source);
  SftpPushHandle *handle = user_data;
  GError *error = NULL;
  GFileInfo *info;

  info = g_file_query_info_finish (source_file, res, &error);
  if (!info)
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
      sftp_push_handle_free (handle);
      return;
    }

  if ((handle->op_job->flags & G_FILE_COPY_NOFOLLOW_SYMLINKS) &&
      g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      /* Fall back to default implementation to copy symlink */
      g_vfs_job_failed (handle->job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
      sftp_push_handle_free (handle);
      return;
    }

  g_file_read_async (source_file, 0, NULL, push_source_open_cb, handle);
}

static gboolean
try_push (GVfsBackend *backend,
          GVfsJobPush *op_job,
          const char *destination,
          const char *local_path,
          GFileCopyFlags flags,
          gboolean remove_source,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GFile *source;
  SftpPushHandle *handle;

  if (remove_source && (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE))
    {
      g_vfs_job_failed (G_VFS_JOB (op_job),
                        G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
      return TRUE;
    }

  if (!connection_is_usable (&op_backend->data_connection))
    {
      g_vfs_job_failed (G_VFS_JOB (op_job),
                        G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
      return TRUE;
    }

  handle = g_slice_new0 (SftpPushHandle);
  handle->backend = g_object_ref (op_backend);
  handle->job = g_object_ref (G_VFS_JOB (op_job));
  handle->op_job = op_job;

  source = g_file_new_for_path (local_path);
  g_file_query_info_async (source,
                           G_FILE_ATTRIBUTE_STANDARD_TYPE,
                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                           0, NULL,
                           push_source_lstat_cb, handle);
  g_object_unref (source);

  return TRUE;
}

/* The pull sliding window mechanism is based on the one from the OpenSSH sftp
 * client. It is complicated because requests can be returned out of order. */

#define PULL_MAX_REQUESTS 64  /* Never have more than this many requests outstanding */
#define PULL_SIZE_INCOMPLETE -1  /* Indicates an incomplete fstat() request */
#define PULL_SIZE_INVALID -2  /* Indicates that no fstat() request is in progress */

typedef struct {
  /* initial job information */
  GVfsBackendSftp *backend;
  GVfsJob *job;
  GVfsJobPull *op_job;
  GFile *dest;

  /* Open files */
  DataBuffer *raw_handle;
  GOutputStream *output;

  /* fstat information */
  goffset size;
  guint32 mode;
  guint64 mtime;
  guint64 atime;

  /* state */
  goffset offset;
  goffset n_written;
  int num_req; /* Number of outstanding read requests */
  int max_req; /* Current maximum number of outstanding read requests */
  GList *queued_writes;
} SftpPullHandle;

typedef struct {
  SftpPullHandle *handle;
  guint32 request_len;    /* number of bytes requested */
  guint64 request_offset; /* offset of requested bytes */
  gssize response_len;     /* number of bytes returned */
  gssize write_offset;     /* offset in buffer of bytes written so far */
  char *buffer;
} PullRequest;

static void
pull_enqueue_next_request (SftpPullHandle *handle);

static void
pull_enqueue_request (SftpPullHandle *handle, guint64 offset, guint32 len);

static void
pull_try_start_write (SftpPullHandle *handle);

static void
pull_request_free (PullRequest *request)
{
  if (request->buffer)
      g_slice_free1 (request->response_len, request->buffer);
  g_slice_free (PullRequest, request);
}

static void
sftp_pull_handle_free (SftpPullHandle *handle)
{
  if (handle->size != PULL_SIZE_INCOMPLETE && /* fstat complete */
      (!handle->output || !g_output_stream_has_pending (handle->output)) && /* no writes outstanding */
      handle->num_req == 0) /* no reads oustanding */
    {
      if (handle->raw_handle)
        {
          GDataOutputStream *command = new_command_stream (handle->backend, SSH_FXP_CLOSE);
          put_data_buffer (command, handle->raw_handle);
          queue_command_stream_and_free (&handle->backend->data_connection, command,
                                         NULL,
                                         handle->job, NULL);
          data_buffer_free (handle->raw_handle);
        }
      g_clear_object (&handle->output);
      g_object_unref(handle->backend);
      g_object_unref(handle->op_job);
      g_object_unref(handle->dest);
      g_list_free_full (handle->queued_writes, (GDestroyNotify)pull_request_free);
      g_slice_free (SftpPullHandle, handle);
    }
}

static void
pull_remove_source_reply (GVfsBackendSftp *backend,
                          int reply_type,
                          GDataInputStream *reply,
                          guint32 len,
                          GVfsJob *job,
                          gpointer user_data)
{
  if (reply_type == SSH_FXP_STATUS)
    result_from_status (job, reply, -1, -1);
  else
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Invalid reply received"));
}

static void
pull_set_perms_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SftpPullHandle *handle = user_data;

  if (handle->op_job->remove_source)
    {
      GDataOutputStream *command = new_command_stream (handle->backend, SSH_FXP_REMOVE);
      put_string (command, handle->op_job->source);
      queue_command_stream_and_free (&handle->backend->command_connection,
                                     command,
                                     pull_remove_source_reply,
                                     handle->job,
                                     NULL);
    }
  else
    g_vfs_job_succeeded (handle->job);

  sftp_pull_handle_free (handle);
}

static void
pull_close_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SftpPullHandle *handle = user_data;
  GError *error = NULL;

  if (g_output_stream_close_finish(handle->output, res, &error))
    {
      g_vfs_job_progress_callback (handle->n_written, handle->n_written, handle->job);

      if (handle->size >= 0)
        {
          GFileInfo *info = g_file_info_new ();
          if (!(handle->op_job->flags & G_FILE_COPY_TARGET_DEFAULT_PERMS))
            g_file_info_set_attribute_uint32 (info,
                                              G_FILE_ATTRIBUTE_UNIX_MODE,
                                              handle->mode);
          g_file_info_set_attribute_uint64 (info,
                                            G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                            handle->mtime);
          /* Atime is COPY_WHEN_MOVED, but not COPY_WITH_FILE. */
          if (handle->op_job->remove_source ||
              (handle->op_job->flags & G_FILE_COPY_ALL_METADATA))
            g_file_info_set_attribute_uint64 (info,
                                              G_FILE_ATTRIBUTE_TIME_ACCESS,
                                              handle->atime);
          g_file_set_attributes_async (handle->dest,
                                       info,
                                       G_FILE_QUERY_INFO_NONE,
                                       G_PRIORITY_DEFAULT,
                                       NULL,
                                       pull_set_perms_cb, handle);
          g_object_unref (info);
          return;
        }

      if (handle->op_job->remove_source)
        {
          GDataOutputStream *command = new_command_stream (handle->backend, SSH_FXP_REMOVE);
          put_string (command, handle->op_job->source);
          queue_command_stream_and_free (&handle->backend->command_connection,
                                         command,
                                         pull_remove_source_reply,
                                         handle->job,
                                         NULL);
        }
      else
        g_vfs_job_succeeded (handle->job);
    }
  else
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
    }

  sftp_pull_handle_free (handle);
}

static void
pull_try_finish (SftpPullHandle *handle)
{
  if (handle->max_req == 0 && /* received EOF */
      handle->size != PULL_SIZE_INCOMPLETE && /* fstat complete */
      !g_output_stream_has_pending (handle->output) && /* no writes outstanding */
      handle->num_req == 0) /* no reads oustanding */
    {
      g_output_stream_close_async (handle->output,
                                   G_PRIORITY_DEFAULT,
                                   NULL,
                                   pull_close_cb, handle);
    }
}

static void
pull_write_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PullRequest *request = user_data;
  SftpPullHandle *handle = request->handle;
  GError *error = NULL;
  gssize n_written;

  n_written = g_output_stream_write_finish (handle->output, res, &error);
  if (n_written == -1)
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
      pull_request_free (request);
      sftp_pull_handle_free (handle);
      return;
    }

  handle->n_written += n_written;
  request->write_offset += n_written;

  /* If we didn't write everything, do another write */
  if (request->write_offset < request->response_len)
    {
      g_output_stream_write_async (handle->output,
                                   request->buffer + request->write_offset,
                                   request->response_len - request->write_offset,
                                   G_PRIORITY_DEFAULT,
                                   NULL,
                                   pull_write_cb, request);
      return;
    }

  if (handle->size >= 0)
    g_vfs_job_progress_callback (handle->n_written, handle->size, handle->job);

  pull_try_start_write (handle);

  /* If we read short, issue another request for the remaining data. */
  if (request->response_len < request->request_len)
    pull_enqueue_request (handle,
                          request->request_offset + request->response_len,
                          request->request_len - request->response_len);
  else if (handle->max_req == 0)
    pull_try_finish (handle);
  else
    {
      /* Once we have requested past the estimated EOF, request one at a
       * time.  Otherwise try increase the number of concurrent requests. */
      if (handle->offset > handle->size)
        handle->max_req = 1;
      else if (handle->max_req < PULL_MAX_REQUESTS)
        handle->max_req++;

      while (handle->num_req < handle->max_req)
        pull_enqueue_next_request (handle);
    }

  pull_request_free (request);
}

static void
pull_try_start_write (SftpPullHandle *handle)
{
  GError *error = NULL;
  PullRequest *request;

  if (g_output_stream_has_pending (handle->output))
    return;

  if (!handle->queued_writes)
    return;

  request = handle->queued_writes->data;
  handle->queued_writes = g_list_delete_link (handle->queued_writes, handle->queued_writes);

  if (!g_seekable_seek (G_SEEKABLE (handle->output),
                        request->request_offset, G_SEEK_SET,
                        NULL, &error))
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
      pull_request_free (request);
      sftp_pull_handle_free (handle);
      return;
    }

  g_output_stream_write_async (handle->output,
                               request->buffer, request->response_len,
                               G_PRIORITY_DEFAULT,
                               NULL,
                               pull_write_cb, request);
}

static void
pull_read_reply (GVfsBackendSftp *backend,
                 int reply_type,
                 GDataInputStream *reply,
                 guint32 len,
                 GVfsJob *job,
                 gpointer user_data)
{
  PullRequest *request = user_data;
  SftpPullHandle *handle = request->handle;

  handle->num_req--;

  if (check_finished_or_cancelled_job (job))
    {
    }
  else if (reply_type == SSH_FXP_STATUS)
    {
      guint32 code = read_status_code (reply);
      if (code == SSH_FX_EOF)
        {
          pull_request_free (request);
          handle->max_req = 0;
          pull_try_finish (handle);
          return;
        }
      else
        result_from_status_code (job, code, -1, -1);
    }
  else if (reply_type != SSH_FXP_DATA)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid reply received"));
    }
  else
    {
      request->response_len = g_data_input_stream_read_uint32 (reply, NULL, NULL);
      request->buffer = g_slice_alloc (request->response_len);

      if (g_input_stream_read_all (G_INPUT_STREAM (reply),
                                   request->buffer, request->response_len,
                                   NULL, NULL, NULL))
        {
          handle->queued_writes = g_list_append (handle->queued_writes, request);
          pull_try_start_write (handle);
          return;
        }
      else
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Invalid reply received"));
    }

  pull_request_free (request);
  sftp_pull_handle_free (handle);
}

static void
pull_enqueue_request (SftpPullHandle *handle, guint64 offset, guint32 len)
{
  PullRequest *request;
  GDataOutputStream *command;

  request = g_slice_new0 (PullRequest);
  request->handle = handle;
  request->request_len = len;
  request->request_offset = offset;

  command = new_command_stream (handle->backend, SSH_FXP_READ);
  put_data_buffer (command, handle->raw_handle);
  g_data_output_stream_put_uint64 (command, offset, NULL, NULL);
  g_data_output_stream_put_uint32 (command, len, NULL, NULL);
  queue_command_stream_and_free (&handle->backend->data_connection, command,
                                 pull_read_reply,
                                 handle->job, request);

  handle->num_req++;
}

static void
pull_enqueue_next_request (SftpPullHandle *handle)
{
  pull_enqueue_request (handle, handle->offset, MAX_BUFFER_SIZE);
  handle->offset += MAX_BUFFER_SIZE;
}

static void
pull_fstat_reply (GVfsBackendSftp *backend,
                  int reply_type,
                  GDataInputStream *reply,
                  guint32 len,
                  GVfsJob *job,
                  gpointer user_data)
{
  SftpPullHandle *handle = user_data;

  if (check_finished_or_cancelled_job (job))
    {
      handle->size = PULL_SIZE_INVALID;
      sftp_pull_handle_free (handle);
      return;
    }

  if (reply_type == SSH_FXP_ATTRS)
    {
      GFileInfo *info = g_file_info_new ();
      parse_attributes (backend, info, NULL, reply, NULL);
      handle->size = g_file_info_get_size (info);
      handle->mode = g_file_info_get_attribute_uint32 (info,
                                                       G_FILE_ATTRIBUTE_UNIX_MODE);
      handle->mtime = g_file_info_get_attribute_uint64 (info,
                                                        G_FILE_ATTRIBUTE_TIME_MODIFIED);
      handle->atime = g_file_info_get_attribute_uint64 (info,
                                                        G_FILE_ATTRIBUTE_TIME_ACCESS);
      g_object_unref (info);
    }
  else
    handle->size = PULL_SIZE_INVALID;

  pull_try_finish (handle);
}

static void
pull_dest_open_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SftpPullHandle *handle = user_data;
  GError *error = NULL;

  if (handle->op_job->flags & G_FILE_COPY_OVERWRITE)
    handle->output = G_OUTPUT_STREAM (g_file_replace_finish (handle->dest,
                                                             res,
                                                             &error));
  else
    handle->output = G_OUTPUT_STREAM (g_file_create_finish (handle->dest,
                                                            res,
                                                            &error));
  if (handle->output)
    {
      /* Do an fstat() to find out the size and mode of the file. */
      GDataOutputStream *command = new_command_stream (handle->backend,
                                                       SSH_FXP_FSTAT);
      put_data_buffer (command, handle->raw_handle);
      queue_command_stream_and_free (&handle->backend->data_connection,
                                     command,
                                     pull_fstat_reply,
                                     handle->job,
                                     handle);
      handle->size = PULL_SIZE_INCOMPLETE;

      while (handle->num_req < handle->max_req)
        pull_enqueue_next_request (handle);
    }
  else
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
      sftp_pull_handle_free (handle);
    }
}

static void
pull_open_reply (GVfsBackendSftp *backend,
                 MultiReply *replies,
                 int n_replies,
                 GVfsJob *job,
                 gpointer user_data)
{
  SftpPullHandle *handle = user_data;

  if (replies[0].type == SSH_FXP_ATTRS)
    {
      GFileType type;
      GFileInfo *info = g_file_info_new ();

      parse_attributes (backend, info, NULL, replies[0].data, NULL);
      type = g_file_info_get_file_type (info);
      g_object_unref (info);

      if (type != G_FILE_TYPE_REGULAR)
        {
          /* Fall back to default implementation to copy non-regular files */
          g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            _("Operation not supported"));
        }
      else if (replies[1].type == SSH_FXP_STATUS)
        result_from_status (job, replies[1].data, -1, -1);
      else if (replies[1].type != SSH_FXP_HANDLE)
        g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Invalid reply received"));
      else
        {
          /* We got a valid file handle. */
          handle->raw_handle = read_data_buffer (replies[1].data);

          if (handle->op_job->flags & G_FILE_COPY_OVERWRITE)
            g_file_replace_async (handle->dest,
                                  NULL,
                                  handle->op_job->flags & G_FILE_COPY_BACKUP ? TRUE : FALSE,
                                  G_FILE_CREATE_REPLACE_DESTINATION,
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  pull_dest_open_cb, handle);
          else
            g_file_create_async (handle->dest,
                                 G_FILE_CREATE_NONE,
                                 G_PRIORITY_DEFAULT,
                                 NULL,
                                 pull_dest_open_cb, handle);
          return;
        }
    }
  else if (replies[0].type == SSH_FXP_STATUS)
    result_from_status (job, replies[0].data, -1, -1);
  else
    g_vfs_job_failed (job,
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "%s", _("Invalid reply received"));

  /* If we got a file handle, store it.  It will be closed when the
   * SftpPushHandle is freed. */
  if (replies[1].type == SSH_FXP_HANDLE && !handle->raw_handle)
    handle->raw_handle = read_data_buffer (replies[1].data);

  sftp_pull_handle_free (handle);
}

static gboolean
try_pull (GVfsBackend *backend,
          GVfsJobPull *job,
          const char *source,
          const char *local_path,
          GFileCopyFlags flags,
          gboolean remove_source,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  SftpPullHandle *handle;
  Command commands[2];

  if (remove_source && (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
      return TRUE;
    }

  if (!connection_is_usable (&op_backend->data_connection))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
      return TRUE;
    }

  handle = g_slice_new0 (SftpPullHandle);
  handle->backend = g_object_ref (op_backend);
  handle->op_job = g_object_ref (job);
  handle->job = G_VFS_JOB (job);
  handle->dest = g_file_new_for_path (local_path);
  handle->size = PULL_SIZE_INVALID;
  handle->max_req = 1;

  commands[0].connection = &op_backend->command_connection;
  commands[0].cmd = new_command_stream (op_backend,
                                        flags & G_FILE_COPY_NOFOLLOW_SYMLINKS ? SSH_FXP_LSTAT : SSH_FXP_STAT);
  put_string (commands[0].cmd, source);

  commands[1].connection = &op_backend->data_connection;
  commands[1].cmd = new_command_stream (op_backend, SSH_FXP_OPEN);
  put_string (commands[1].cmd, source);
  g_data_output_stream_put_uint32 (commands[1].cmd, SSH_FXF_READ, NULL, NULL);
  g_data_output_stream_put_uint32 (commands[1].cmd, 0, NULL, NULL);

  queue_command_streams_and_free (commands, 2,
                                  pull_open_reply,
                                  G_VFS_JOB(job),
                                  handle);

  return TRUE;
}

static void
g_vfs_backend_sftp_class_init (GVfsBackendSftpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  id_q = g_quark_from_static_string ("command-id");
  
  gobject_class->finalize = g_vfs_backend_sftp_finalize;

  backend_class->mount = real_do_mount;
  backend_class->try_mount = try_mount;
  backend_class->try_unmount = try_unmount;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_read = try_read;
  backend_class->try_seek_on_read = try_seek_on_read;
  backend_class->try_close_read = try_close_read;
  backend_class->try_close_write = try_close_write;
  backend_class->try_query_info = try_query_info;
  backend_class->try_query_fs_info = try_query_fs_info;
  backend_class->try_query_info_on_read = (gpointer) try_query_info_fstat;
  backend_class->try_query_info_on_write = (gpointer) try_query_info_fstat;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_create = try_create;
  backend_class->try_append_to = try_append_to;
  backend_class->try_edit = try_edit;
  backend_class->try_replace = try_replace;
  backend_class->try_write = try_write;
  backend_class->try_seek_on_write = try_seek_on_write;
  backend_class->try_truncate = try_truncate;
  backend_class->try_move = try_move;
  backend_class->try_make_symlink = try_make_symlink;
  backend_class->try_make_directory = try_make_directory;
  backend_class->try_delete = try_delete;
  backend_class->try_set_display_name = try_set_display_name;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
  backend_class->try_set_attribute = try_set_attribute;
  backend_class->try_push = try_push;
  backend_class->try_pull = try_pull;
}
