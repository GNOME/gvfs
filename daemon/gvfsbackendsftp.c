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

#include <stdlib.h>
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
#include "gvfsdaemonprotocol.h"
#include "gvfskeyring.h"
#include "sftp.h"
#include "pty_open.h"

/* TODO for sftp:
 * Implement can_delete & can_rename
 * fstat
 */

#ifdef HAVE_GRANTPT
/* We only use this on systems with unix98 ptys */
#define USE_PTY 1
#endif

#define SFTP_READ_TIMEOUT 40   /* seconds */

static GQuark id_q;

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
  gboolean make_backup;
} SftpHandle;


typedef struct {
  ReplyCallback callback;
  GVfsJob *job;
  gpointer user_data;
} ExpectedReply;

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
  
  GOutputStream *command_stream;
  GInputStream *reply_stream;
  GDataInputStream *error_stream;

  GCancellable *reply_stream_cancellable;

  guint32 current_id;
  
  /* Output Queue */
  
  gsize command_bytes_written;
  GList *command_queue;
  
  /* Reply reading: */
  GHashTable *expected_replies;
  guint32 reply_size;
  guint32 reply_size_read;
  guint8 *reply;
  
  GMountSource *mount_source; /* Only used/set during mount */
  int mount_try;
  gboolean mount_try_again;
};

static void parse_attributes (GVfsBackendSftp *backend,
                              GFileInfo *info,
                              const char *basename,
                              GDataInputStream *reply,
                              GFileAttributeMatcher *attribute_matcher);

static void setup_icon (GVfsBackendSftp *op_backend,
                        GVfsJobMount    *job);


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

static void
g_vfs_backend_sftp_finalize (GObject *object)
{
  GVfsBackendSftp *backend;

  backend = G_VFS_BACKEND_SFTP (object);

  g_hash_table_destroy (backend->expected_replies);
  
  if (backend->command_stream)
    g_object_unref (backend->command_stream);
  
  if (backend->reply_stream_cancellable)
    g_object_unref (backend->reply_stream_cancellable);

  if (backend->reply_stream)
    g_object_unref (backend->reply_stream);
  
  if (backend->error_stream)
    g_object_unref (backend->error_stream);
  
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
  backend->expected_replies = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)expected_reply_free);
}

static void
look_for_stderr_errors (GVfsBackend *backend, GError **error)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  char *line;

  while (1)
    {
      line = g_data_input_stream_read_line (op_backend->error_stream, NULL, NULL, NULL);
      
      if (line == NULL)
        {
          /* Error (real or WOULDBLOCK) or EOF */
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_FAILED,
        	               _("ssh program unexpectedly exited"));
          return;
        }
      
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
      else if (strstr (line, "Connection refused") != NULL)
        {
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
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
      
      g_free (line);
    }
}

static gchar*
read_dbus_string_dict_value (DBusMessageIter *args, const gchar *key)
{
  DBusMessageIter items, entry;
  gchar *str, *sig;

  sig = dbus_message_iter_get_signature (args);
  if (!sig || strcmp (sig, "a{ss}") != 0)
    return NULL;

  dbus_message_iter_recurse (args, &items);

  if (dbus_message_iter_has_next (&items))
    {
      do
	{
	  dbus_message_iter_recurse (&items, &entry);
	  dbus_message_iter_get_basic (&entry, &str);
	  if (str && strcmp (key, str) == 0) 
	    {
	      dbus_message_iter_next (&entry);
	      dbus_message_iter_get_basic (&entry, &str);
	      return g_strdup (str);
	    }
	}
      while (dbus_message_iter_next (&items));
    }

  return NULL;
}

static void
setup_ssh_environment (void)
{
  DBusConnection *dconn;
  DBusMessage *reply;
  DBusMessage *msg;
  DBusMessageIter args;
  DBusError derr;
  gchar *env;

  dbus_error_init (&derr);
  dconn = dbus_bus_get (DBUS_BUS_SESSION, &derr);
  if (!dconn)
    return;

  msg = dbus_message_new_method_call ("org.gnome.keyring",
				      "/org/gnome/keyring/daemon",
				      "org.gnome.keyring.Daemon",
				      "GetEnvironment");
  if (!msg)
    {
      dbus_connection_unref (dconn);
      return;
    }

  /* Send message and get a handle for a reply */
  reply = dbus_connection_send_with_reply_and_block (dconn, msg, 1000, &derr);
  dbus_message_unref (msg);
  if (!reply)
    {
      dbus_connection_unref (dconn);
      return;
    }

  /* Read the return value */
  if (dbus_message_iter_init (reply, &args))
    {
      env = read_dbus_string_dict_value (&args, "SSH_AUTH_SOCK");
      if (env && env[0])
	g_setenv ("SSH_AUTH_SOCK", env, TRUE);
      g_free (env);
    }

  dbus_message_unref (reply);
  dbus_connection_unref (dconn);
}

static char **
setup_ssh_commandline (GVfsBackend *backend)
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
      args[last_arg++] = g_strdup ("-oClearAllForwardings yes");
      args[last_arg++] = g_strdup ("-oProtocol 2");
      args[last_arg++] = g_strdup ("-oNoHostAuthenticationForLocalhost yes");
#ifndef USE_PTY
      args[last_arg++] = g_strdup ("-oBatchMode yes");
#endif
    
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
           GError **error)
{
#ifdef USE_PTY
  *tty_fd = pty_open(pid, PTY_REAP_CHILD, NULL,
		     args[0], args, NULL,
		     300, 300, 
		     stdin_fd, stdout_fd, stderr_fd);
  if (*tty_fd == -1)
    {
      g_set_error_literal (error,
			   G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Unable to spawn ssh program"));
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
                   _("Unable to spawn ssh program: %s"), my_error->message);
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
send_command_sync_and_unref_command (GVfsBackendSftp *backend,
                                     GDataOutputStream *command_stream,
                                     GCancellable *cancellable,
                                     GError **error)
{
  gpointer data;
  gsize len;
  gsize bytes_written;
  gboolean res;
  
  data = get_data_from_command_stream (command_stream, &len);

  res = g_output_stream_write_all (backend->command_stream,
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
read_reply_sync (GVfsBackendSftp *backend, gsize *len_out, GError **error)
{
  guint32 len;
  gsize bytes_read;
  GByteArray *array;
  guint8 *data;
  
  if (!g_input_stream_read_all (backend->reply_stream,
				&len, 4,
				&bytes_read, NULL, error))
    return NULL;

  /* Make sure we handle ssh exiting early, e.g. if no further
     authentication methods */
  if (bytes_read == 0)
    {
      g_set_error_literal (error,
			   G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("ssh program unexpectedly exited"));
      return NULL;
    }
  
  len = GUINT32_FROM_BE (len);
  
  array = g_byte_array_sized_new (len);

  if (!g_input_stream_read_all (backend->reply_stream,
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
              GError **error)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GInputStream *prompt_stream;
  GOutputStream *reply_stream;
  fd_set ifds;
  struct timeval tv;
  int ret;
  int prompt_fd;
  char buffer[1024];
  gsize len;
  gboolean aborted = FALSE;
  gboolean ret_val;
  char *new_password = NULL;
  char *new_user = NULL;
  gsize bytes_written;
  gboolean password_in_keyring = FALSE;
  const gchar *authtype = NULL;
  gchar *object = NULL;
  char *prompt;
  
  if (op_backend->client_vendor == SFTP_VENDOR_SSH) 
    prompt_fd = stderr_fd;
  else
    prompt_fd = tty_fd;

  prompt_stream = g_unix_input_stream_new (prompt_fd, FALSE);
  reply_stream = g_unix_output_stream_new (tty_fd, FALSE);

  ret_val = TRUE;
  while (1)
    {
      FD_ZERO (&ifds);
      FD_SET (stdout_fd, &ifds);
      FD_SET (prompt_fd, &ifds);
      
      tv.tv_sec = SFTP_READ_TIMEOUT;
      tv.tv_usec = 0;
      
      ret = select (MAX (stdout_fd, prompt_fd)+1, &ifds, NULL, NULL, &tv);
      
      if (ret <= 0)
        {
          g_set_error_literal (error,
	                       G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
        	               _("Timed out when logging in"));
          ret_val = FALSE;
          break;
        }
      
      if (FD_ISSET (stdout_fd, &ifds))
        break; /* Got reply to initial INIT request */
      
      g_assert (FD_ISSET (prompt_fd, &ifds));
      
      
      len = g_input_stream_read (prompt_stream,
                                 buffer, sizeof (buffer) - 1,
                                 NULL, error);
      
      if (len == -1)
        {
          ret_val = FALSE;
          break;
        }
      
      buffer[len] = 0;

      /*
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
          g_str_has_prefix (buffer, "Password for ") ||
          g_str_has_prefix (buffer, "Enter Kerberos password") ||
          g_str_has_prefix (buffer, "Enter passphrase for key"))
        {
	  authtype = get_authtype_from_password_line (buffer);
	  object = get_object_from_password_line (buffer);

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
                                              NULL,
                                              NULL,
                                              &new_password)))
            {
              GAskPasswordFlags flags = G_ASK_PASSWORD_NEED_PASSWORD;
              
              if (g_vfs_keyring_is_available ())
                flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;
	      if (strcmp (authtype, "password") == 0 &&
		  !op_backend->user_specified)
	        flags |= G_ASK_PASSWORD_NEED_USERNAME;

              g_free (new_password);
              
              if (op_backend->user_specified)
                /* Translators: the first %s is the username, the second the host name */
                if (strcmp (authtype, "publickey") == 0)
                  prompt = g_strdup_printf (_("Enter passphrase for key for ssh as %s on %s"), op_backend->user, op_backend->host);
                else
                  prompt = g_strdup_printf (_("Enter password for ssh as %s on %s"), op_backend->user, op_backend->host);
              else
                /* translators: %s here is the hostname */
                if (strcmp (authtype, "publickey") == 0)
                  prompt = g_strdup_printf (_("Enter passphrase for key for ssh on %s"), op_backend->host);
                else
                  prompt = g_strdup_printf (_("Enter password for ssh on %s"), op_backend->host);

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
	      /* I already a have a password of a previous login attempt
	       * that failed because the user provided a new user name
	       */
	      new_password = op_backend->tmp_password;
	      op_backend->tmp_password = NULL;
	    }
          else
            password_in_keyring = TRUE;

	  if (new_user &&
	      (op_backend->user == NULL ||
	       strcmp (new_user, op_backend->user) != 0))
	    {
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
        	                   _("Can't send password"));
              ret_val = FALSE;
              break;
            }
        }
      else if (g_str_has_prefix (buffer, "The authenticity of host '") ||
               strstr (buffer, "Key fingerprint:") != NULL)
        {
	  const gchar *choices[] = {_("Log In Anyway"), _("Cancel Login")};
	  const gchar *choice_string;
	  gchar *hostname = NULL;
	  gchar *fingerprint = NULL;
	  gint choice;
	  gchar *message;

	  get_hostname_and_fingerprint_from_line (buffer, &hostname, &fingerprint);

	  message = g_strdup_printf (_("The identity of the remote computer (%s) is unknown.\n"
				       "This happens when you log in to a computer the first time.\n\n"
				       "The identity sent by the remote computer is %s. "
				       "If you want to be absolutely sure it is safe to continue, "
				       "contact the system administrator."),
				     hostname ? hostname : op_backend->host, fingerprint);

	  g_free (hostname);
	  g_free (fingerprint);

	  if (!g_mount_source_ask_question (mount_source,
					    message,
					    choices,
					    2,
					    &aborted,
					    &choice) || 
	      aborted)
	    {
	      g_set_error_literal (error,
				   G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
				   _("Login dialog cancelled"));
	      g_free (message);
	      ret_val = FALSE;
	      break;
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
				   G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
				   _("Can't send host identity confirmation"));
	      ret_val = FALSE;
	      break;
	    }
	}
    }
  
  if (ret_val)
    {
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
    }

  g_free (object);
  g_free (new_password);
  g_object_unref (prompt_stream);
  g_object_unref (reply_stream);
  return ret_val;
}

static void
fail_jobs_and_die (GVfsBackendSftp *backend, GError *error)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, backend->expected_replies);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      ExpectedReply *expected_reply = (ExpectedReply *) value;
      g_vfs_job_failed_from_error (expected_reply->job, error);
    }

  g_error_free (error);

  _exit (1);
}

static void
check_input_stream_read_result (GVfsBackendSftp *backend, gssize res, GError *error)
{
  if (G_UNLIKELY (res <= 0))
    {
      if (res == 0 || error == NULL)
        {
          g_clear_error (&error);
          g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       _("Internal error: %s"),
                       res == 0 ? "The underlying ssh process died" : "Unkown Error");
        }

      fail_jobs_and_die (backend, error);
    }
}

static void read_reply_async (GVfsBackendSftp *backend);

static void
read_reply_async_got_data  (GObject *source_object,
                            GAsyncResult *result,
                            gpointer user_data)
{
  GVfsBackendSftp *backend = user_data;
  gssize res;
  GDataInputStream *reply;
  ExpectedReply *expected_reply;
  guint32 id;
  int type;
  GError *error;

  error = NULL;
  res = g_input_stream_read_finish (G_INPUT_STREAM (source_object), result, &error);

  check_input_stream_read_result (backend, res, error);

  backend->reply_size_read += res;

  if (backend->reply_size_read < backend->reply_size)
    {
      g_input_stream_read_async (backend->reply_stream,
				 backend->reply + backend->reply_size_read, backend->reply_size - backend->reply_size_read,
				 0, NULL, read_reply_async_got_data, backend);
      return;
    }

  reply = make_reply_stream (backend->reply, backend->reply_size);
  backend->reply = NULL;

  type = g_data_input_stream_read_byte (reply, NULL, NULL);
  id = g_data_input_stream_read_uint32 (reply, NULL, NULL);

  expected_reply = g_hash_table_lookup (backend->expected_replies, GINT_TO_POINTER (id));
  if (expected_reply)
    {
      if (expected_reply->callback != NULL)
        (expected_reply->callback) (backend, type, reply, backend->reply_size,
                                    expected_reply->job, expected_reply->user_data);
      g_hash_table_remove (backend->expected_replies, GINT_TO_POINTER (id));
    }
  else
    g_warning ("Got unhandled reply of size %"G_GUINT32_FORMAT" for id %"G_GUINT32_FORMAT"\n", backend->reply_size, id);

  g_object_unref (reply);

  read_reply_async (backend);
  
}

static void
read_reply_async_got_len  (GObject *source_object,
                           GAsyncResult *result,
                           gpointer user_data)
{
  GVfsBackendSftp *backend = user_data;
  gssize res;
  GError *error;

  error = NULL;
  res = g_input_stream_read_finish (G_INPUT_STREAM (source_object), result, &error);

  /* Bail out if cancelled */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_object_unref (backend);
      return;
    }

  check_input_stream_read_result (backend, res, error);

  backend->reply_size_read += res;

  if (backend->reply_size_read < 4)
    {
      g_input_stream_read_async (backend->reply_stream,
				 &backend->reply_size + backend->reply_size_read, 4 - backend->reply_size_read,
				 0, backend->reply_stream_cancellable, read_reply_async_got_len,
				 backend);
      return;
    }
  backend->reply_size = GUINT32_FROM_BE (backend->reply_size);

  backend->reply_size_read = 0;
  backend->reply = g_malloc (backend->reply_size);
  g_input_stream_read_async (backend->reply_stream,
			     backend->reply, backend->reply_size,
			     0, NULL, read_reply_async_got_data, backend);
}

static void
read_reply_async (GVfsBackendSftp *backend)
{
  backend->reply_size_read = 0;
  g_input_stream_read_async (backend->reply_stream,
                             &backend->reply_size, 4,
                             0, backend->reply_stream_cancellable,
                             read_reply_async_got_len,
                             backend);
}

static void send_command (GVfsBackendSftp *backend);

static void
send_command_data (GObject *source_object,
                   GAsyncResult *result,
                   gpointer user_data)
{
  GVfsBackendSftp *backend = user_data;
  gssize res;
  DataBuffer *buffer;

  res = g_output_stream_write_finish (G_OUTPUT_STREAM (source_object), result, NULL);

  if (res <= 0)
    {
      /* TODO: unmount, etc */
      g_warning ("Error sending command");
      return;
    }

  buffer = backend->command_queue->data;
  
  backend->command_bytes_written += res;

  if (backend->command_bytes_written < buffer->size)
    {
      g_output_stream_write_async (backend->command_stream,
                                   buffer->data + backend->command_bytes_written,
                                   buffer->size - backend->command_bytes_written,
                                   0,
                                   NULL,
                                   send_command_data,
                                   backend);
      return;
    }

  data_buffer_free (buffer);

  backend->command_queue = g_list_delete_link (backend->command_queue, backend->command_queue);

  if (backend->command_queue != NULL)
    send_command (backend);
}

static void
send_command (GVfsBackendSftp *backend)
{
  DataBuffer *buffer;

  buffer = backend->command_queue->data;
  
  backend->command_bytes_written = 0;
  g_output_stream_write_async (backend->command_stream,
                               buffer->data,
                               buffer->size,
                               0,
                               NULL,
                               send_command_data,
                               backend);
}

static void
expect_reply (GVfsBackendSftp *backend,
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

  g_hash_table_replace (backend->expected_replies, GINT_TO_POINTER (id), expected);
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
queue_command_buffer (GVfsBackendSftp *backend,
                      DataBuffer *buffer)
{
  gboolean first;
  
  first = backend->command_queue == NULL;

  backend->command_queue = g_list_append (backend->command_queue, buffer);
  
  if (first)
    send_command (backend);
}

static void
queue_command_stream_and_free (GVfsBackendSftp *backend,
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

  expect_reply (backend, id, callback, job, user_data);
  queue_command_buffer (backend, buffer);
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

static void
queue_command_streams_and_free (GVfsBackendSftp *backend,
                                GDataOutputStream **commands,
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
      queue_command_stream_and_free (backend,
                                     commands[i],
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
  send_command_sync_and_unref_command (backend, command, NULL, NULL);

  reply = read_reply_sync (backend, NULL, NULL);
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
  send_command_sync_and_unref_command (backend, command, NULL, NULL);

  reply = read_reply_sync (backend, NULL, NULL);
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
          gboolean is_automount)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  gchar **args; /* Enough for now, extend if you add more args */
  pid_t pid;
  int tty_fd, stdout_fd, stdin_fd, stderr_fd;
  GError *error;
  GInputStream *is;
  GDataOutputStream *command;
  GDataInputStream *reply;
  gboolean res;
  GMountSpec *sftp_mount_spec;
  char *extension_name, *extension_data;
  char *display_name;

  args = setup_ssh_commandline (backend);

  error = NULL;
  if (!spawn_ssh (backend,
		  args, &pid,
		  &tty_fd, &stdin_fd, &stdout_fd, &stderr_fd,
		  &error))
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_strfreev (args);
      return;
    }

  g_strfreev (args);

  op_backend->command_stream = g_unix_output_stream_new (stdin_fd, TRUE);

  command = new_command_stream (op_backend, SSH_FXP_INIT);
  g_data_output_stream_put_int32 (command,
                                  SSH_FILEXFER_VERSION, NULL, NULL);
  send_command_sync_and_unref_command (op_backend, command, NULL, NULL);

  if (tty_fd == -1)
    res = wait_for_reply (backend, stdout_fd, &error);
  else
    res = handle_login (backend, mount_source, tty_fd, stdout_fd, stderr_fd, &error);
  
  if (!res)
    {
      if (error->code == G_IO_ERROR_INVALID_ARGUMENT)
        {
	  /* New username provided by the user,
	   * we need to re-spawn the ssh command
	   */
	  g_error_free (error);
	  do_mount (backend, job, mount_spec, mount_source, is_automount);
	}
      else
        {
	  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
	  g_error_free (error);
	}
      
      return;
    }

  op_backend->reply_stream = g_unix_input_stream_new (stdout_fd, TRUE);
  op_backend->reply_stream_cancellable = g_cancellable_new ();

  make_fd_nonblocking (stderr_fd);
  is = g_unix_input_stream_new (stderr_fd, TRUE);
  op_backend->error_stream = g_data_input_stream_new (is);
  g_object_unref (is);
  
  reply = read_reply_sync (op_backend, NULL, NULL);
  if (reply == NULL)
    {
      look_for_stderr_errors (backend, &error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  
  if (g_data_input_stream_read_byte (reply, NULL, NULL) != SSH_FXP_VERSION)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Protocol error"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  
  op_backend->protocol_version = g_data_input_stream_read_uint32 (reply, NULL, NULL);

  while ((extension_name = read_string (reply, NULL)) != NULL)
    {
      extension_data = read_string (reply, NULL);
      if (extension_data)
        {
          /* TODO: Do something with this */
        }
      g_free (extension_name);
      g_free (extension_data);
    }

  g_object_unref (reply);

  if (!get_uid_sync (op_backend) || !get_home_sync (op_backend))
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Protocol error"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  read_reply_async (g_object_ref (op_backend));

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
    /* Translators: This is the name of an sftp share, like "sftp for <user>on <hostname>" */
    display_name = g_strdup_printf (_("sftp for %s on %s"), op_backend->user, op_backend->host);
  else
    /* Translators: This is the name of an sftp share, like "sftp on <hostname>" */
    display_name = g_strdup_printf (_("sftp on %s"), op_backend->host);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  /* checks for /etc/favicon.png */
  setup_icon (op_backend, job);
  
  /* NOTE: job_succeeded called async from setup_icon reply */
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
			_("Unable to find supported ssh command"));
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

  if (op_backend->reply_stream && op_backend->reply_stream_cancellable)
    g_cancellable_cancel (op_backend->reply_stream_cancellable);
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
      return _("Operation unsupported");
    case G_IO_ERROR_PERMISSION_DENIED:
      return _("Permission denied");
    case G_IO_ERROR_NOT_FOUND:
      return _("No such file or directory");
    default:
      return "Unknown reason";
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
  queue_command_stream_and_free (op_backend, command, error_from_lstat_reply,
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
  GIcon *icon;
  
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
          g_file_info_set_is_symlink (info, TRUE);
          mimetype = "inode/symlink";
        }

      free_mimetype = FALSE;
      if (mimetype == NULL)
        {
          if (basename)
            {
              mimetype = g_content_type_guess (basename, NULL, 0, NULL);
              free_mimetype = TRUE;
            }
          else
            mimetype = "application/octet-stream";
        }
      
      g_file_info_set_content_type (info, mimetype);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, mimetype);
      
      if (g_file_attribute_matcher_matches (matcher,
                                            G_FILE_ATTRIBUTE_STANDARD_ICON))
        {
          icon = NULL;
          if (S_ISDIR(mode))
            icon = g_themed_icon_new ("folder");
          else if (mimetype)
              icon = g_content_type_get_icon (mimetype);

          if (icon == NULL)
            icon = g_themed_icon_new ("text-x-generic");

          g_file_info_set_icon (info, icon);
          g_object_unref (icon);
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

          
          /* Translators: This is the name of the root of an sftp share, like "/ on <hostname>" */
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
          queue_command_stream_and_free (backend, command, NULL, G_VFS_JOB (job), NULL);

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
  queue_command_stream_and_free (op_backend, command, open_stat_reply, G_VFS_JOB (job), NULL);

  command = new_command_stream (op_backend,
                                SSH_FXP_OPEN);
  put_string (command, filename);
  g_data_output_stream_put_uint32 (command, SSH_FXF_READ, NULL, NULL); /* open flags */
  g_data_output_stream_put_uint32 (command, 0, NULL, NULL); /* Attr flags */
  
  queue_command_stream_and_free (op_backend, command, open_for_read_reply, G_VFS_JOB (job), NULL);

  return TRUE;
}

static gboolean
try_open_icon_for_read (GVfsBackend *backend,
                        GVfsJobOpenIconForRead *job,
                        const char *icon_id)
{
  if (g_str_has_prefix (icon_id, "favicon:"))
    {
      return try_open_for_read (backend,
                                G_VFS_JOB_OPEN_FOR_READ (job),
                                icon_id + sizeof ("favicon:") -1);
    }

  g_vfs_job_failed (G_VFS_JOB (job),
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    _("Invalid icon_id '%s' in OpenIconForRead"),
                    icon_id);
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
  
  queue_command_stream_and_free (op_backend, command, read_reply, G_VFS_JOB (job), handle);

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

  switch (op_job->seek_type)
    {
    case G_SEEK_CUR:
      handle->offset += op_job->requested_offset;
      break;
    case G_SEEK_SET:
      handle->offset = op_job->requested_offset;
      break;
    case G_SEEK_END:
      handle->offset = file_size + op_job->requested_offset;
      break;
    }

  if (handle->offset < 0)
    handle->offset = 0;
  if (handle->offset > file_size)
    handle->offset = file_size;
  
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

  command = new_command_stream (op_backend,
                                SSH_FXP_FSTAT);
  put_data_buffer (command, handle->raw_handle);
  
  queue_command_stream_and_free (op_backend, command, seek_read_fstat_reply, G_VFS_JOB (job), handle);

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
      queue_command_stream_and_free (backend, command, NULL, job, NULL);
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
  queue_command_stream_and_free (backend, command, close_moved_tempfile, G_VFS_JOB (job), handle);
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
      command = new_command_stream (backend,
                                    SSH_FXP_SETSTAT);
      put_string (command, handle->tempname);
      g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_PERMISSIONS, NULL, NULL);
      g_data_output_stream_put_uint32 (command, handle->permissions, NULL, NULL);
      queue_command_stream_and_free (backend, command, close_restore_permissions, G_VFS_JOB (job), handle);
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
      queue_command_stream_and_free (backend, command, close_moved_tempfile, G_VFS_JOB (job), handle);
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
  queue_command_stream_and_free (backend, command, close_moved_file, G_VFS_JOB (job), handle);
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
              queue_command_stream_and_free (backend, command, close_deleted_backup, G_VFS_JOB (job), handle);
            }
          else
            {
              command = new_command_stream (backend,
                                            SSH_FXP_REMOVE);
              put_string (command, handle->filename);
              queue_command_stream_and_free (backend, command, close_deleted_file, G_VFS_JOB (job), handle);
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

  queue_command_stream_and_free (backend, command, close_write_reply, G_VFS_JOB (job), handle);
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

  queue_command_stream_and_free (op_backend, command, close_write_fstat_reply, G_VFS_JOB (job), handle);

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

  queue_command_stream_and_free (op_backend, command, close_read_reply, G_VFS_JOB (job), handle);

  return TRUE;
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
create_reply (GVfsBackendSftp *backend,
              int reply_type,
              GDataInputStream *reply,
              guint32 len,
              GVfsJob *job,
              gpointer user_data)
{
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
  g_vfs_job_succeeded (job);
}

static gboolean
try_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend,
                                SSH_FXP_OPEN);
  put_string (command, filename);
  g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_EXCL,  NULL, NULL); /* open flags */
  g_data_output_stream_put_uint32 (command, 0, NULL, NULL); /* Attr flags */
  
  queue_command_stream_and_free (op_backend, command, create_reply, G_VFS_JOB (job), NULL);

  return TRUE;
}

static void
append_to_error (GVfsBackendSftp *backend,
		 GVfsJob *job,
		 gint original_error,
		 gint stat_error,
		 GFileInfo *info,
		 gpointer user_data)
{
  if ((original_error == SSH_FX_FAILURE) &&
      info != NULL &&
      g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
			_("File is directory"));
      return;
    }

  if (original_error == SSH_FX_NO_SUCH_FILE)
    {
      not_dir_or_not_exist_error (backend, job,
				  G_VFS_JOB_OPEN_FOR_WRITE (job)->filename);
      return;
    }
  

  result_from_status_code (job, original_error, -1, -1);
}

static void
append_to_reply (GVfsBackendSftp *backend,
                 int reply_type,
                 GDataInputStream *reply,
                 guint32 len,
                 GVfsJob *job,
                 gpointer user_data)
{
  SftpHandle *handle;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      error_from_lstat (backend, job, read_status_code (reply),
			G_VFS_JOB_OPEN_FOR_WRITE (job)->filename,
			append_to_error,
			NULL);
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
  g_vfs_job_open_for_write_set_can_seek (G_VFS_JOB_OPEN_FOR_WRITE (job), FALSE);
  g_vfs_job_succeeded (job);
}

static gboolean
try_append_to (GVfsBackend *backend,
               GVfsJobOpenForWrite *job,
               const char *filename,
               GFileCreateFlags flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GDataOutputStream *command;

  command = new_command_stream (op_backend,
                                SSH_FXP_OPEN);
  put_string (command, filename);
  g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_APPEND,  NULL, NULL); /* open flags */
  g_data_output_stream_put_uint32 (command, 0, NULL, NULL); /* Attr flags */
  
  queue_command_stream_and_free (op_backend, command, append_to_reply, G_VFS_JOB (job), NULL);

  return TRUE;
}

typedef struct {
  guint32 permissions;
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
  handle->make_backup = op_job->make_backup;
  
  g_vfs_job_open_for_write_set_handle (op_job, handle);
  g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
  
  g_vfs_job_succeeded (job);
}

static void
replace_truncate_original (GVfsBackendSftp *backend,
                           GVfsJob *job)
{
  GVfsJobOpenForWrite *op_job;
  GDataOutputStream *command;
  ReplaceData *data;

  data = job->backend_data;
  op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  
  command = new_command_stream (backend,
                                SSH_FXP_OPEN);
  put_string (command, op_job->filename);
  g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_TRUNC,  NULL, NULL); /* open flags */
  g_data_output_stream_put_uint32 (command, 0, NULL, NULL); /* Attr flags */
  
  queue_command_stream_and_free (backend, command, replace_truncate_original_reply, job, NULL);
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
      else if (data->set_ownership &&
	       error->code == G_IO_ERROR_PERMISSION_DENIED)
        {
          g_error_free (error);
	  
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
				_("backups not supported yet"));
	    }
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
  handle->make_backup = op_job->make_backup;
  
  g_vfs_job_open_for_write_set_handle (op_job, handle);
  g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
  
  g_vfs_job_succeeded (job);
}

static void
random_text (char *s)
{
  static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static const int NLETTERS = sizeof (letters) - 1;
  static int counter = 0;

  GTimeVal tv;
  glong value;

  /* Get some more or less random data.  */
  g_get_current_time (&tv);
  value = (tv.tv_usec ^ tv.tv_sec) + counter++;

  /* Fill in the random bits.  */
  s[0] = letters[value % NLETTERS];
  value /= NLETTERS;
  s[1] = letters[value % NLETTERS];
  value /= NLETTERS;
  s[2] = letters[value % NLETTERS];
  value /= NLETTERS;
  s[3] = letters[value % NLETTERS];
  value /= NLETTERS;
  s[4] = letters[value % NLETTERS];
  value /= NLETTERS;
  s[5] = letters[value % NLETTERS];
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
  random_text (basename + 8);
  data->tempname = g_build_filename (dirname, basename, NULL);
  g_free (dirname);

  command = new_command_stream (op_backend,
                                SSH_FXP_OPEN);
  put_string (command, data->tempname);
  g_data_output_stream_put_uint32 (command, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_EXCL,  NULL, NULL); /* open flags */
  g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_PERMISSIONS | (data->set_ownership ? SSH_FILEXFER_ATTR_UIDGID : 0), NULL, NULL); /* Attr flags */
  
  if (data->set_ownership)
  {
    g_data_output_stream_put_uint32 (command, data->uid, NULL, NULL);
    g_data_output_stream_put_uint32 (command, data->gid, NULL, NULL);
  }
  
  g_data_output_stream_put_uint32 (command, data->permissions, NULL, NULL);
  queue_command_stream_and_free (op_backend, command, replace_create_temp_reply, G_VFS_JOB (job), NULL);
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
  gboolean set_ownership = FALSE;
  ReplaceData *data;

  op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);

  permissions = 0644;
  
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

      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_MODE))
        permissions = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE) & 0777;
      
      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_UID) && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_GID))
      {
        set_ownership = TRUE;
        uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);
        gid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID);
      }
    }

  data = g_slice_new0 (ReplaceData);
  data->permissions = permissions;
  data->set_ownership = set_ownership;
  
  if (set_ownership)
  {
    data->uid = uid;
    data->gid = gid;
  }
  
  g_vfs_job_set_backend_data (job, data, (GDestroyNotify)replace_data_free);

  replace_create_temp (backend, op_job);    
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
          queue_command_stream_and_free (backend, command, replace_stat_reply, G_VFS_JOB (job), NULL);
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
  g_data_output_stream_put_uint32 (command, 0, NULL, NULL); /* Attr flags */
  
  queue_command_stream_and_free (op_backend, command, replace_exclusive_reply, G_VFS_JOB (job), NULL);

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
          handle->offset += G_VFS_JOB_WRITE (job)->data_size;
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

  command = new_command_stream (op_backend,
                                SSH_FXP_WRITE);
  put_data_buffer (command, handle->raw_handle);
  g_data_output_stream_put_uint64 (command, handle->offset, NULL, NULL);
  g_data_output_stream_put_uint32 (command, buffer_size, NULL, NULL);
  /* Ideally we shouldn't do this copy, but doing the writes as multiple writes
     caused problems on the read side in openssh */
  g_output_stream_write_all (G_OUTPUT_STREAM (command),
                             buffer, buffer_size,
                             NULL, NULL, NULL);
  
  queue_command_stream_and_free (op_backend, command, write_reply, G_VFS_JOB (job), handle);

  /* We always write the full size (on success) */
  g_vfs_job_write_set_written_size (job, buffer_size);

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

  switch (op_job->seek_type)
    {
    case G_SEEK_CUR:
      handle->offset += op_job->requested_offset;
      break;
    case G_SEEK_SET:
      handle->offset = op_job->requested_offset;
      break;
    case G_SEEK_END:
      handle->offset = file_size + op_job->requested_offset;
      break;
    }

  if (handle->offset < 0)
    handle->offset = 0;
  if (handle->offset > file_size)
    handle->offset = file_size;
  
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

  command = new_command_stream (op_backend,
                                SSH_FXP_FSTAT);
  put_data_buffer (command, handle->raw_handle);
  
  queue_command_stream_and_free (op_backend, command, seek_write_fstat_reply, G_VFS_JOB (job), handle);

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
      queue_command_stream_and_free (backend, command, read_dir_readlink_reply, G_VFS_JOB (job), g_object_ref (info));
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
      queue_command_stream_and_free (backend, command, NULL, G_VFS_JOB (job), NULL);
  
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
          
          queue_command_stream_and_free (backend, command, read_dir_symlink_reply, G_VFS_JOB (job), g_object_ref (info));
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
  queue_command_stream_and_free (backend, command, read_dir_reply, G_VFS_JOB (job), NULL);
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
  
  queue_command_stream_and_free (op_backend, command, read_dir_reply, G_VFS_JOB (job), NULL);
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
  
  queue_command_stream_and_free (op_backend, command, open_dir_reply, G_VFS_JOB (job), NULL);

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
          guint32 count;
          
          count = g_data_input_stream_read_uint32 (reply->data, NULL, NULL);
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
  GDataOutputStream *commands[3];
  GDataOutputStream *command;
  int n_commands;

  n_commands = 0;
  
  command = commands[n_commands++] =
    new_command_stream (op_backend,
                        SSH_FXP_LSTAT);
  put_string (command, filename);
  
  if (! (job->flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS))
    {
      command = commands[n_commands++] =
        new_command_stream (op_backend,
                            SSH_FXP_STAT);
      put_string (command, filename);
    }

  if (g_file_attribute_matcher_matches (job->attribute_matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
    {
      command = commands[n_commands++] =
        new_command_stream (op_backend,
                            SSH_FXP_READLINK);
      put_string (command, filename);
    }

  queue_command_streams_and_free (op_backend, commands, n_commands, query_info_reply, G_VFS_JOB (job), NULL);
  
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
  queue_command_stream_and_free (op_backend, command, query_info_fstat_reply, G_VFS_JOB (job), data);

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
            g_vfs_job_move_progress_callback (*file_size, *file_size, job);
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

  queue_command_stream_and_free (backend, command, move_reply, G_VFS_JOB (job), NULL);
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
                                  _("Can't move directory over directory"));
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
      queue_command_stream_and_free (backend, command, move_delete_target_reply, G_VFS_JOB (job), NULL);
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
  GDataOutputStream *commands[2];

  command = commands[0] =
    new_command_stream (op_backend,
                        SSH_FXP_LSTAT);
  put_string (command, source);

  command = commands[1] =
    new_command_stream (op_backend,
                        SSH_FXP_LSTAT);
  put_string (command, destination);

  queue_command_streams_and_free (op_backend, commands, 2, move_lstat_reply, G_VFS_JOB (job), NULL);
  
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
  
  queue_command_stream_and_free (op_backend, command, set_display_name_reply, G_VFS_JOB (job), NULL);

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
    result_from_status (job, reply, -1, -1); 
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
  
  queue_command_stream_and_free (op_backend, command, make_symlink_reply, G_VFS_JOB (job), NULL);

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
          queue_command_stream_and_free (backend, command, mkdir_stat_reply, G_VFS_JOB (job), NULL);
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

  queue_command_stream_and_free (op_backend, command, make_directory_reply, G_VFS_JOB (job), NULL);

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
          queue_command_stream_and_free (backend, command, delete_rmdir_reply, G_VFS_JOB (job), NULL);
        }
      else
        {
          command = new_command_stream (backend,
                                        SSH_FXP_REMOVE);
          put_string (command, G_VFS_JOB_DELETE (job)->filename);
          queue_command_stream_and_free (backend, command, delete_remove_reply, G_VFS_JOB (job), NULL);
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
  queue_command_stream_and_free (op_backend, command, delete_lstat_reply, G_VFS_JOB (job), NULL);

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

  if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) != 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation unsupported"));
      return TRUE;
    }

  command = new_command_stream (op_backend,
                                SSH_FXP_SETSTAT);
  put_string (command, filename);
  g_data_output_stream_put_uint32 (command, SSH_FILEXFER_ATTR_PERMISSIONS, NULL, NULL);
  g_data_output_stream_put_uint32 (command, (*(guint32 *)value_p) & 0777, NULL, NULL);
  queue_command_stream_and_free (op_backend, command, set_attribute_reply, G_VFS_JOB (job), NULL);
  
  return TRUE;
}

static void
setup_icon_reply (GVfsBackendSftp *backend,
                  MultiReply *replies,
                  int n_replies,
                  GVfsJob *job,
                  gpointer user_data)
{
  GIcon *icon;
  gboolean have_favicon;
  MultiReply *stat_reply;

  have_favicon = FALSE;

  stat_reply = &replies[0];
  if (stat_reply->type == SSH_FXP_ATTRS)
    have_favicon = TRUE;

  if (have_favicon)
    {
      icon = g_vfs_icon_new (g_vfs_backend_get_mount_spec (G_VFS_BACKEND (backend)),
                             "favicon:/etc/favicon.png");
      g_vfs_backend_set_icon (G_VFS_BACKEND (backend), icon);
      g_object_unref (icon);
    }
  else
    {
      g_vfs_backend_set_icon_name (G_VFS_BACKEND (backend), "folder-remote");
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* called from do_mount(); finds out if there's an /etc/favicon.png file; if so, use it as the icon */
static void
setup_icon (GVfsBackendSftp *op_backend,
            GVfsJobMount    *job)
{
  GDataOutputStream *command;

  command = new_command_stream (op_backend, SSH_FXP_STAT);
  put_string (command, "/etc/favicon.png");

  queue_command_streams_and_free (op_backend,
                                  &command,
                                  1,
                                  setup_icon_reply,
                                  G_VFS_JOB (job),
                                  NULL);
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
  backend_class->try_open_icon_for_read = try_open_icon_for_read;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_read = try_read;
  backend_class->try_seek_on_read = try_seek_on_read;
  backend_class->try_close_read = try_close_read;
  backend_class->try_close_write = try_close_write;
  backend_class->try_query_info = try_query_info;
  backend_class->try_query_info_on_read = (gpointer) try_query_info_fstat;
  backend_class->try_query_info_on_write = (gpointer) try_query_info_fstat;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_create = try_create;
  backend_class->try_append_to = try_append_to;
  backend_class->try_replace = try_replace;
  backend_class->try_write = try_write;
  backend_class->try_seek_on_write = try_seek_on_write;
  backend_class->try_move = try_move;
  backend_class->try_make_symlink = try_make_symlink;
  backend_class->try_make_directory = try_make_directory;
  backend_class->try_delete = try_delete;
  backend_class->try_set_display_name = try_set_display_name;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
  backend_class->try_set_attribute = try_set_attribute;
}
