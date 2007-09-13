/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

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
#include <gio/gdatainputstream.h>
#include <gio/gdataoutputstream.h>
#include <gio/gsocketinputstream.h>
#include <gio/gsocketoutputstream.h>
#include <gio/gmemoryoutputstream.h>
#include <gio/gmemoryinputstream.h>

#include "gvfsbackendsftp.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobgetinfo.h"
#include "gvfsjobgetfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "sftp.h"
#include "pty_open.h"

#ifdef HAVE_GRANTPT
/* We only use this on systems with unix98 ptys */
#define USE_PTY 1
#endif

typedef enum {
  SFTP_VENDOR_INVALID = 0,
  SFTP_VENDOR_OPENSSH,
  SFTP_VENDOR_SSH
} SFTPClientVendor;

typedef void (*ReplyCallback) (GVfsBackendSftp *backend,
                               int reply_type,
                               GDataInputStream *reply,
                               guint32 len,
                               GVfsJob *job);
                               
typedef struct {
  guchar *data;
  gsize size;
} DataBuffer;

typedef struct {
  ReplyCallback callback;
  GVfsJob *job;
} ExpectedReply;

struct _GVfsBackendSftp
{
  GVfsBackend parent_instance;

  SFTPClientVendor client_vendor;
  char *host;
  gboolean user_specified;
  char *user;

  guint32 my_uid;
  guint32 my_gid;
  
  int protocol_version;
  
  GOutputStream *command_stream;
  GInputStream *reply_stream;
  GDataInputStream *error_stream;

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
                              GDataInputStream *reply);


G_DEFINE_TYPE (GVfsBackendSftp, g_vfs_backend_sftp, G_VFS_TYPE_BACKEND);

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
      line = g_data_input_stream_get_line (op_backend->error_stream, NULL, NULL, NULL);
      
      if (line == NULL)
	{
	  /* Error (real or WOULDBLOCK) or EOF */
	  g_set_error (error,
		       G_IO_ERROR, G_IO_ERROR_FAILED,
		       _("ssh program unexpectedly exited"));
	  return;
	}

      if (strstr (line, "Permission denied") != NULL)
	{
	  g_set_error (error,
		       G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		       _("Permission denied"));
	  return;
	}
      else if (strstr (line, "Name or service not known") != NULL)
	{
	  g_set_error (error,
		       G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND,
		       _("Hostname not known"));
	  return;
	}
      else if (strstr (line, "No route to host") != NULL)
	{
	  g_set_error (error,
		       G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND,
		       _("No route to host"));
	  return;
	}
      else if (strstr (line, "Connection refused") != NULL)
	{
	  g_set_error (error,
		       G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		       _("Connection refused by server"));
	  return;
	}
      else if (strstr (line, "Host key verification failed") != NULL) 
	{
	  g_set_error (error,
		       G_IO_ERROR, G_IO_ERROR_FAILED,
		       _("Host key verification failed"));
	  return;
	}
      
      g_free (line);
    }
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

  /* TODO: Support port 
  if (port != 0)
    {
      args[last_arg++] = g_strdup ("-p");
      args[last_arg++] = g_strdup_printf ("%d", port);
    }
  */
    

  args[last_arg++] = g_strdup ("-l");
  args[last_arg++] = g_strdup (op_backend->user);

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
      g_set_error (error,
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
		   _("Unable to spawn ssh program: %s"), my_error->msg);
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
new_command_stream (GVfsBackendSftp *backend, int type, guint32 *id_out)
{
  GOutputStream *mem_stream;
  GDataOutputStream *data_stream;
  guint32 id;

  mem_stream = g_memory_output_stream_new (NULL);
  data_stream = g_data_output_stream_new (mem_stream);
  g_object_unref (mem_stream);

  g_data_output_stream_put_int32 (data_stream, 0, NULL, NULL); /* LEN */
  g_data_output_stream_put_byte (data_stream, type, NULL, NULL);
  if (type != SSH_FXP_INIT)
    {
      id = get_new_id (backend);
      g_data_output_stream_put_uint32 (data_stream, id, NULL, NULL);
      if (id_out)
        *id_out = id;
    }
  
  return data_stream;
}

static GByteArray *
get_data_from_command_stream (GDataOutputStream *command_stream, gboolean free_on_close)
{
  GOutputStream *mem_stream;
  GByteArray *array;
  guint32 *len_ptr;
  
  mem_stream = g_filter_output_stream_get_base_stream (G_FILTER_OUTPUT_STREAM (command_stream));
  g_memory_output_stream_set_free_on_close (G_MEMORY_OUTPUT_STREAM (mem_stream), free_on_close);
  array = g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (mem_stream));

  len_ptr = (guint32 *)array->data;
  *len_ptr = GUINT32_TO_BE (array->len - 4);
  
  return array;
}

static gboolean
send_command_sync (GVfsBackendSftp *backend,
                   GDataOutputStream *command_stream,
                   GCancellable *cancellable,
                   GError **error)
{
  GByteArray *array;
  gsize bytes_written;
  gboolean res;
  
  array = get_data_from_command_stream (command_stream, TRUE);

  res = g_output_stream_write_all (backend->command_stream,
                                   array->data, array->len,
                                   &bytes_written,
                                   cancellable, error);
  
  if (error == NULL && !res)
    g_warning ("Ignored send_command error\n");
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
  
  tv.tv_sec = 20;
  tv.tv_usec = 0;
      
  ret = select (stdout_fd+1, &ifds, NULL, NULL, &tv);

  if (ret <= 0)
    {
      g_set_error (error,
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
  
  mem_stream = g_memory_input_stream_from_data (data, len);
  g_memory_input_stream_set_free_data (G_MEMORY_INPUT_STREAM (mem_stream), TRUE);

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
  gsize bytes_written;
  g_data_output_stream_put_uint32 (stream, buffer->size, NULL, NULL);
  g_output_stream_write_all (G_OUTPUT_STREAM (stream),
                             buffer->data, buffer->size,
                             &bytes_written,
                             NULL, NULL);
}

static char *
read_string (GDataInputStream *stream, gsize *len_out)
{
  guint32 len;
  char *data;
  GError *error;

  error = NULL;
  len = g_data_input_stream_get_uint32 (stream, NULL, &error);
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
  gsize bytes_written;
  
  if (op_backend->client_vendor == SFTP_VENDOR_SSH) 
    prompt_fd = stderr_fd;
  else
    prompt_fd = tty_fd;

  prompt_stream = g_socket_input_stream_new (prompt_fd, FALSE);
  reply_stream = g_socket_output_stream_new (tty_fd, FALSE);

  ret_val = TRUE;
  while (1)
    {
      FD_ZERO (&ifds);
      FD_SET (stdout_fd, &ifds);
      FD_SET (prompt_fd, &ifds);
      
      tv.tv_sec = 20;
      tv.tv_usec = 0;
      
      ret = select (MAX (stdout_fd, prompt_fd)+1, &ifds, NULL, NULL, &tv);
  
      if (ret <= 0)
	{
	  g_set_error (error,
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
	  g_str_has_prefix (buffer, "Enter passphrase for key"))
	{
	  if (!g_mount_source_ask_password (mount_source,
					    g_str_has_prefix (buffer, "Enter passphrase for key") ?
					    _("Enter passphrase for key")
					    :
					    _("Enter password"),
					    op_backend->user,
					    NULL,
					    G_PASSWORD_FLAGS_NEED_PASSWORD,
					    &aborted,
					    &new_password,
					    NULL,
					    NULL) ||
	      aborted)
	    {
	      g_set_error (error,
			   G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			   _("Password dialog cancelled"));
	      ret_val = FALSE;
	      break;
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
	      g_set_error (error,
			   G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			   _("Can't send password"));
	      ret_val = FALSE;
	      break;
	    }
	}
      else if (g_str_has_prefix (buffer, "The authenticity of host '") ||
	       strstr (buffer, "Key fingerprint:") != NULL)
	{
	  /* TODO: Handle these messages */
	}
    }

  g_object_unref (prompt_stream);
  g_object_unref (reply_stream);
  return ret_val;
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

  res = g_input_stream_read_finish (G_INPUT_STREAM (source_object), result, NULL);

  if (res <= 0)
    {
      /* TODO: unmount, etc */
      g_warning ("Error reading results");
      return;
    }

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

  type = g_data_input_stream_get_byte (reply, NULL, NULL);
  id = g_data_input_stream_get_uint32 (reply, NULL, NULL);
  
  expected_reply = g_hash_table_lookup (backend->expected_replies, GINT_TO_POINTER (id));
  if (expected_reply)
    {
      (expected_reply->callback) (backend, type, reply, backend->reply_size, expected_reply->job);
      g_hash_table_remove (backend->expected_replies, GINT_TO_POINTER (id));
    }
  else
    g_warning ("Got unhandled reply of size %d for id %d\n", backend->reply_size, id);

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

  res = g_input_stream_read_finish (G_INPUT_STREAM (source_object), result, NULL);

  if (res <= 0)
    {
      /* TODO: unmount, etc */
      g_warning ("Error reading results");
      return;
    }

  backend->reply_size_read += res;

  if (backend->reply_size_read < 4)
    {
      g_input_stream_read_async (backend->reply_stream,
				 &backend->reply_size + backend->reply_size_read, 4 - backend->reply_size_read,
				 0, NULL, read_reply_async_got_len, backend);
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
			     0, NULL, read_reply_async_got_len, backend);
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
              GVfsJob *job)
{
  ExpectedReply *expected;

  expected = g_slice_new (ExpectedReply);
  expected->callback = callback;
  expected->job = g_object_ref (job);

  g_hash_table_replace (backend->expected_replies, GINT_TO_POINTER (id), expected);
}

static void
queue_command_stream_and_free (GVfsBackendSftp *backend,
                               GDataOutputStream *command_stream,
                               guint32 id,
                               ReplyCallback callback,
                               GVfsJob *job)
{
  GByteArray *array;
  gboolean first;
  DataBuffer *buffer;

  buffer = g_slice_new (DataBuffer);

  array = get_data_from_command_stream (command_stream, FALSE);
  
  buffer->data = array->data;
  buffer->size = array->len;
  g_object_unref (command_stream);
  g_byte_array_free (array, FALSE);
  
  first = backend->command_queue == NULL;

  backend->command_queue = g_list_append (backend->command_queue, buffer);
  expect_reply (backend, id, callback, job);
  
  if (first)
    send_command (backend);
}

static gboolean
get_uid_sync (GVfsBackendSftp *backend)
{
  GDataOutputStream *command;
  GDataInputStream *reply;
  GFileInfo *info;
  int type;
  guint32 id;
  
  command = new_command_stream (backend, SSH_FXP_STAT, NULL);
  put_string (command, ".");
  send_command_sync (backend, command, NULL, NULL);
  g_object_unref (command);

  reply = read_reply_sync (backend, NULL, NULL);
  if (reply == NULL)
    return FALSE;
  
  type = g_data_input_stream_get_byte (reply, NULL, NULL);
  id = g_data_input_stream_get_uint32 (reply, NULL, NULL);

  /* On error, set uid to -1 and ignore */
  backend->my_uid = (guint32)-1;
  backend->my_gid = (guint32)-1;
  if (type == SSH_FXP_ATTRS)
    {
      info = g_file_info_new ();
      parse_attributes (backend, info, reply);
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

  g_print ("My uid is: %d\n", backend->my_uid);
  
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

  op_backend->command_stream = g_socket_output_stream_new (stdin_fd, TRUE);

  command = new_command_stream (op_backend, SSH_FXP_INIT, NULL);
  g_data_output_stream_put_int32 (command,
                                  SSH_FILEXFER_VERSION, NULL, NULL);
  send_command_sync (op_backend, command, NULL, NULL);
  g_object_unref (command);

  if (tty_fd == -1)
    res = wait_for_reply (backend, stdout_fd, &error);
  else
    res = handle_login (backend, mount_source, tty_fd, stdout_fd, stderr_fd, &error);
  
  if (!res)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  op_backend->reply_stream = g_socket_input_stream_new (stdout_fd, TRUE);

  make_fd_nonblocking (stderr_fd);
  is = g_socket_input_stream_new (stderr_fd, TRUE);
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
  
  if (g_data_input_stream_get_byte (reply, NULL, NULL) != SSH_FXP_VERSION)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Protocol error"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  
  op_backend->protocol_version = g_data_input_stream_get_uint32 (reply, NULL, NULL);

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

  if (!get_uid_sync (op_backend))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Protocol error"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  
  read_reply_async (op_backend);

  sftp_mount_spec = g_mount_spec_new ("sftp");
  if (op_backend->user_specified)
    g_mount_spec_set (sftp_mount_spec, "user", op_backend->user);
  g_mount_spec_set (sftp_mount_spec, "host", op_backend->host);

  g_vfs_backend_set_mount_spec (backend, sftp_mount_spec);
  g_mount_spec_unref (sftp_mount_spec);

  g_print ("succeeded with sftp mount\n");
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  const char *user, *host;

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
			_("Invalid mount spec"));
      return TRUE;
    }

  user = g_mount_spec_get (mount_spec, "user");

  op_backend->host = g_strdup (host);
  op_backend->user = g_strdup (user);
  if (op_backend->user)
    op_backend->user_specified = TRUE;
  else
    op_backend->user = g_strdup (g_get_user_name ());

  return FALSE;
}

static void
result_from_status (GVfsJob *job,
                    GDataInputStream *reply)
{
  GError error;
  guint32 code;

  code = g_data_input_stream_get_uint32 (reply, NULL, NULL);

  if (code == SSH_FX_OK)
    {
      g_vfs_job_succeeded (job);
      return;
    }

  error.domain = G_IO_ERROR;
  error.code = G_IO_ERROR_FAILED;

  switch (code)
    {
    default:
    case SSH_FX_EOF:
    case SSH_FX_FAILURE:
    case SSH_FX_BAD_MESSAGE:
    case SSH_FX_NO_CONNECTION:
    case SSH_FX_CONNECTION_LOST:
      break;
      
    case SSH_FX_NO_SUCH_FILE:
      error.code = G_IO_ERROR_NOT_FOUND;
      break;
      
    case SSH_FX_PERMISSION_DENIED:
      error.code = G_IO_ERROR_PERMISSION_DENIED;
      break;

    case SSH_FX_OP_UNSUPPORTED:
      error.code = G_IO_ERROR_NOT_SUPPORTED;
      break;
    }
  
  error.message = read_string (reply, NULL);
  if (error.message == NULL)
    error.message = g_strdup ("Unknown reason");

  g_vfs_job_failed_from_error (job, &error);
  g_free (error.message);
}

static void
parse_attributes_is_symlink (GFileInfo *info,
                             GDataInputStream *reply)
{
  guint32 flags;

  flags = g_data_input_stream_get_uint32 (reply, NULL, NULL);
  if (flags & SSH_FILEXFER_ATTR_SIZE)
    g_data_input_stream_get_uint64 (reply, NULL, NULL);
  
  if (flags & SSH_FILEXFER_ATTR_UIDGID)
    {
      g_data_input_stream_get_uint32 (reply, NULL, NULL);
      g_data_input_stream_get_uint32 (reply, NULL, NULL);
    }

  if (flags & SSH_FILEXFER_ATTR_PERMISSIONS)
    {
      guint32 v;
      v = g_data_input_stream_get_uint32 (reply, NULL, NULL);
      if (S_ISLNK (v))
        g_file_info_set_is_symlink (info, TRUE);
    }
}

static void
set_access_attributes (GFileInfo *info,
                       guint32 perm)
{
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
                                     perm & 0x4);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                                     perm & 0x2);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
                                     perm & 0x1);
}
  

static void
parse_attributes (GVfsBackendSftp *backend,
                  GFileInfo *info,
                  GDataInputStream *reply)
{
  guint32 flags;
  GFileType type;
  guint32 uid, gid;
  gboolean has_uid;
  
  flags = g_data_input_stream_get_uint32 (reply, NULL, NULL);
  
  if (flags & SSH_FILEXFER_ATTR_SIZE)
    {
      guint64 size = g_data_input_stream_get_uint64 (reply, NULL, NULL);
      g_file_info_set_size (info, size);
    }

  has_uid = FALSE;
  uid = gid = 0; /* Avoid warnings */
  if (flags & SSH_FILEXFER_ATTR_UIDGID)
    {
      has_uid = TRUE;
      uid = g_data_input_stream_get_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, uid);
      gid = g_data_input_stream_get_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, gid);
    }

  type = G_FILE_TYPE_UNKNOWN;
  
  if (flags & SSH_FILEXFER_ATTR_PERMISSIONS)
    {
      guint32 v;
      v = g_data_input_stream_get_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, v);

      if (S_ISREG (v))
        type = G_FILE_TYPE_REGULAR;
      else if (S_ISDIR (v))
        type = G_FILE_TYPE_DIRECTORY;
      else if (S_ISFIFO (v) ||
               S_ISSOCK (v) ||
               S_ISCHR (v) ||
               S_ISBLK (v))
        type = G_FILE_TYPE_SPECIAL;
      else if (S_ISLNK (v))
        type = G_FILE_TYPE_SYMBOLIC_LINK;

      if (has_uid && backend->my_uid != (guint32)-1)
        {
          if (uid == backend->my_uid)
            set_access_attributes (info, (v >> 6) & 0x7);
          else if (gid == backend->my_gid)
            set_access_attributes (info, (v >> 3) & 0x7);
          else
            set_access_attributes (info, (v >> 0) & 0x7);
        }
    }

  g_file_info_set_file_type (info, type);
  
  if (flags & SSH_FILEXFER_ATTR_ACMODTIME)
    {
      guint32 v;
      v = g_data_input_stream_get_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, v);
      v = g_data_input_stream_get_uint32 (reply, NULL, NULL);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, v);
    }
  
  if (flags & SSH_FILEXFER_ATTR_EXTENDED)
    {
      guint32 v;
      v = g_data_input_stream_get_uint32 (reply, NULL, NULL);
      g_print ("extended count: %d\n", v);
      /* TODO: Handle more */
    }
}
                  
typedef struct {
  DataBuffer *handle;
} ReadDirData;

static
void
read_dir_data_free (ReadDirData *data)
{
  data_buffer_free (data->handle);
  g_slice_free (ReadDirData, data);
}

static void
read_dir_reply (GVfsBackendSftp *backend,
                int reply_type,
                GDataInputStream *reply,
                guint32 len,
                GVfsJob *job)
{
  guint32 count;
  int i;
  GList *infos;
  guint32 id;
  GDataOutputStream *command;
  ReadDirData *data;

  data = job->backend_data;

  g_print ("read_dir_reply %d\n", reply_type);

  if (reply_type != SSH_FXP_NAME)
    {
      /* Ignore all error, including the expected END OF FILE.
       * Real errors are expected in open_dir anyway */
      g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
      return;
    }

  infos = NULL;
  count = g_data_input_stream_get_uint32 (reply, NULL, NULL);
  g_print ("count: %d\n", count);
  for (i = 0; i < count; i++)
    {
      GFileInfo *info;
      char *name;
      char *longname;

      info = g_file_info_new ();
      name = read_string (reply, NULL);
      g_print ("name: %s\n", name);
      g_file_info_set_name (info, name);
      
      longname = read_string (reply, NULL);
      g_free (longname);
      
      parse_attributes (backend, info, reply);
      if (strcmp (".", name) == 0 ||
          strcmp ("..", name) == 0)
        g_object_unref (info);
      else
        infos = g_list_prepend (infos, info);
      
      g_free (name);
    }
  
  g_vfs_job_enumerate_add_info (G_VFS_JOB_ENUMERATE (job), infos);
  g_list_foreach (infos, (GFunc)g_object_unref, NULL);
  g_list_free (infos);

  command = new_command_stream (backend,
                                SSH_FXP_READDIR,
                                &id);
  put_data_buffer (command, data->handle);
  queue_command_stream_and_free (backend, command, id, read_dir_reply, G_VFS_JOB (job));
}

static void
open_dir_reply (GVfsBackendSftp *backend,
                int reply_type,
                GDataInputStream *reply,
                guint32 len,
                GVfsJob *job)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  guint32 id;
  GDataOutputStream *command;
  ReadDirData *data;

  g_print ("open_dir_reply %d\n", reply_type);

  data = job->backend_data;
  
  if (reply_type == SSH_FXP_STATUS)
    {
      result_from_status (job, reply);
      return;
    }

  if (reply_type != SSH_FXP_HANDLE)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Invalid reply recieved"));
      return;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  data->handle = read_data_buffer (reply);
  
  command = new_command_stream (op_backend,
                                SSH_FXP_READDIR,
                                &id);
  put_data_buffer (command, data->handle);
  
  queue_command_stream_and_free (op_backend, command, id, read_dir_reply, G_VFS_JOB (job));
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileGetInfoFlags flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  guint32 id;
  GDataOutputStream *command;
  ReadDirData *data;

  data = g_slice_new0 (ReadDirData);

  g_vfs_job_set_backend_data (G_VFS_JOB (job), data, (GDestroyNotify)read_dir_data_free);
  command = new_command_stream (op_backend,
                                SSH_FXP_OPENDIR,
                                &id);
  put_string (command, filename);
  
  queue_command_stream_and_free (op_backend, command, id, open_dir_reply, G_VFS_JOB (job));

  return TRUE;
}

static void
get_info_reply (GVfsBackendSftp *backend,
                int reply_type,
                GDataInputStream *reply,
                guint32 len,
                GVfsJob *job)
{
  GFileInfo *info;
  int finished_count;
  
  if (job->sent_reply)
    return; /* Other might have failed */
  
  if (reply_type == SSH_FXP_STATUS)
    {
      result_from_status (job, reply);
      return;
    }

  if (reply_type != SSH_FXP_ATTRS)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Invalid reply recieved"));
      return;
    }

  info = G_VFS_JOB_GET_INFO (job)->file_info;

  parse_attributes (backend, info, reply);

  finished_count = GPOINTER_TO_INT (job->backend_data);
  job->backend_data = GINT_TO_POINTER (--finished_count);
  if (finished_count == 0)
    g_vfs_job_succeeded (job);
}

static void
get_info_is_link_reply (GVfsBackendSftp *backend,
                        int reply_type,
                        GDataInputStream *reply,
                        guint32 len,
                        GVfsJob *job)
{
  GFileInfo *info;
  int finished_count;

  if (job->sent_reply)
    return; /* Other might have failed */
  
  if (reply_type == SSH_FXP_STATUS)
    {
      result_from_status (job, reply);
      return;
    }

  if (reply_type != SSH_FXP_ATTRS)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Invalid reply recieved"));
      return;
    }

  info = G_VFS_JOB_GET_INFO (job)->file_info;

  parse_attributes_is_symlink (info, reply);

  finished_count = GPOINTER_TO_INT (job->backend_data);
  job->backend_data = GINT_TO_POINTER (--finished_count);
  if (finished_count == 0)
    g_vfs_job_succeeded (job);
}


static gboolean
try_get_info (GVfsBackend *backend,
              GVfsJobGetInfo *job,
              const char *filename,
              GFileGetInfoFlags flags,
              GFileInfo *info,
              GFileAttributeMatcher *matcher)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  guint32 id;
  GDataOutputStream *command;

  if (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS)
    {
      G_VFS_JOB (job)->backend_data = GINT_TO_POINTER (1);
      command = new_command_stream (op_backend,
                                    SSH_FXP_LSTAT,
                                    &id);
      put_string (command, filename);
      
      queue_command_stream_and_free (op_backend, command, id, get_info_reply, G_VFS_JOB (job));
    }
  else
    {
      if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STD_IS_SYMLINK))
        {
          G_VFS_JOB (job)->backend_data = GINT_TO_POINTER (2);
          command = new_command_stream (op_backend,
                                        SSH_FXP_LSTAT,
                                        &id);
          put_string (command, filename);
          queue_command_stream_and_free (op_backend, command, id, get_info_is_link_reply, G_VFS_JOB (job));
        }
      else
        G_VFS_JOB (job)->backend_data = GINT_TO_POINTER (1);
      
      command = new_command_stream (op_backend,
                                    SSH_FXP_STAT,
                                    &id);
      put_string (command, filename);
      queue_command_stream_and_free (op_backend, command, id, get_info_reply, G_VFS_JOB (job));
    }

  return TRUE;
}


static void
g_vfs_backend_sftp_class_init (GVfsBackendSftpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_sftp_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->try_get_info = try_get_info;
  backend_class->try_enumerate = try_enumerate;
}
