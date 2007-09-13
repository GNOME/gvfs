#include <config.h>

#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib/gthread.h>
#include <glib/gi18n.h>
#include "gvfsdaemonutils.h"
#include "gvfsdaemonprotocol.h"

static gint32 extra_fd_slot = -1;
static GStaticMutex extra_lock = G_STATIC_MUTEX_INIT;

typedef struct {
  int extra_fd;
  int fd_count;
} ConnectionExtra;

void
g_dbus_oom (void)
{
  g_error ("DBus failed with out of memory error");
  exit (1);
}

/* We use _ for escaping, so its not valid */
#define VALID_INITIAL_NAME_CHARACTER(c)         \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') )
#define VALID_NAME_CHARACTER(c)                 \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z'))


static void
append_escaped_name (GString *s,
		     const char *unescaped)
{
  char c;
  gboolean first;
  static const gchar hex[16] = "0123456789ABCDEF";

  while ((c = *unescaped++) != 0)
    {
      if (first)
	{
	  if (VALID_INITIAL_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}
      else
	{
	  if (VALID_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}

      first = FALSE;
      g_string_append_c (s, '_');
      g_string_append_c (s, hex[((guchar)c) >> 4]);
      g_string_append_c (s, hex[((guchar)c) & 0xf]);
    }
}

DBusMessage *
dbus_message_new_error_from_gerror (DBusMessage *message,
				    GError *error)
{
  DBusMessage *reply;
  GString *str;

  str = g_string_new ("org.glib.GError.");
  append_escaped_name (str, g_quark_to_string (error->domain));
  g_string_append_printf (str, ".c%d", error->code);
  reply = dbus_message_new_error (message, str->str, error->message);
  g_string_free (str, TRUE);
  return reply;
}

/* We use _ for escaping */
#define VALID_INITIAL_BUS_NAME_CHARACTER(c)         \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
   /*((c) == '_') || */((c) == '-'))
#define VALID_BUS_NAME_CHARACTER(c)                 \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
   /*((c) == '_')||*/  ((c) == '-'))


static void
append_escaped_bus_name (GString *s,
			 const char *unescaped)
{
  char c;
  gboolean first;
  static const gchar hex[16] = "0123456789ABCDEF";

  while ((c = *unescaped++) != 0)
    {
      if (first)
	{
	  if (VALID_INITIAL_BUS_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}
      else
	{
	  if (VALID_BUS_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}

      first = FALSE;
      g_string_append_c (s, '_');
      g_string_append_c (s, hex[((guchar)c) >> 4]);
      g_string_append_c (s, hex[((guchar)c) & 0xf]);
    }
}

char *
_g_dbus_bus_name_from_mountpoint (const char *mountpoint)
{
  GString *bus_name;
  
  bus_name = g_string_new (G_VFS_DBUS_MOUNTPOINT_NAME);
  append_escaped_bus_name (bus_name, mountpoint);
  return g_string_free (bus_name, FALSE);
}


static void
free_extra (gpointer p)
{
  ConnectionExtra *extra = p;
  close (extra->extra_fd);
  g_free (extra);
}

void
dbus_connection_add_fd_send_fd (DBusConnection *connection,
				int extra_fd)
{
  ConnectionExtra *extra;

  if (extra_fd_slot == -1 && 
      !dbus_connection_allocate_data_slot (&extra_fd_slot))
    g_error ("Unable to allocate data slot");

  extra = g_new0 (ConnectionExtra, 1);
  extra->extra_fd = extra_fd;
  
  if (!dbus_connection_set_data (connection, extra_fd_slot, extra, free_extra))
    g_dbus_oom ();
}

static int
send_fd (int connection_fd, 
	 int fd)
{
  struct msghdr msg;
  struct iovec vec;
  char buf[1] = {'x'};
  char ccmsg[CMSG_SPACE (sizeof (fd))];
  struct cmsghdr *cmsg;
  int ret;
  
  msg.msg_name = NULL;
  msg.msg_namelen = 0;

  vec.iov_base = buf;
  vec.iov_len = 1;
  msg.msg_iov = &vec;
  msg.msg_iovlen = 1;
  msg.msg_control = ccmsg;
  msg.msg_controllen = sizeof (ccmsg);
  cmsg = CMSG_FIRSTHDR (&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN (sizeof(fd));
  *(int*)CMSG_DATA (cmsg) = fd;
  msg.msg_controllen = cmsg->cmsg_len;
  msg.msg_flags = 0;

  ret = sendmsg (connection_fd, &msg, 0);
  g_print ("sendmesg ret: %d\n", ret);
  return ret;
}

gboolean 
dbus_connection_send_fd (DBusConnection *connection,
			 int fd, 
			 int *fd_id,
			 GError **error)
{
  ConnectionExtra *extra;

  g_assert (extra_fd_slot != -1);
  extra = dbus_connection_get_data (connection, extra_fd_slot);
  g_assert (extra != NULL);

  if (extra->extra_fd == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   G_FILE_ERROR_IO,
		   _("No fd passing socket availible"));
      return FALSE;
    }

  g_static_mutex_lock (&extra_lock);

  if (send_fd (extra->extra_fd, fd) == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error sending fd: %s"),
		   g_strerror (errno));
      g_static_mutex_unlock (&extra_lock);
      return FALSE;
    }

  *fd_id = extra->fd_count++;

  g_static_mutex_unlock (&extra_lock);

  return TRUE;
}

char *
g_error_to_daemon_reply (GError *error, guint32 seq_nr, gsize *len_out)
{
  char *buffer;
  const char *domain;
  gsize domain_len, message_len;
  GVfsDaemonSocketProtocolReply *reply;
  gsize len;
  
  domain = g_quark_to_string (error->domain);
  domain_len = strlen (domain);
  message_len = strlen (error->message);

  len = G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE +
    domain_len + 1 + message_len + 1;
  buffer = g_malloc (len);

  reply = (GVfsDaemonSocketProtocolReply *)buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR);
  reply->seq_nr = g_htonl (seq_nr);
  reply->arg1 = g_htonl (error->code);
  reply->arg2 = g_htonl (domain_len + 1 + message_len + 1);

  memcpy (buffer + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
	  domain, domain_len + 1);
  memcpy (buffer + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE + domain_len + 1,
	  error->message, message_len + 1);
  
  *len_out = len;
  
  return buffer;
}

void
_g_dbus_message_iter_append_cstring (DBusMessageIter *iter, const char *filename)
{
  DBusMessageIter array;

  if (filename == NULL)
    filename = "";
  
  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_TYPE_BYTE_AS_STRING,
					 &array))
    g_dbus_oom ();

  if (!dbus_message_iter_append_fixed_array (&array,
					     DBUS_TYPE_BYTE,
					     &filename, strlen (filename)))
    g_dbus_oom ();

  if (!dbus_message_iter_close_container (iter, &array))
    g_dbus_oom ();
}
