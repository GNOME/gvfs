#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gfileunix.h"
#include "gvfsunixdbus.h"
#include <gvfsdaemonprotocol.h>
#include <glib/gi18n-lib.h>

static void g_file_unix_file_iface_init (GFileIface       *iface);

struct _GFileUnix
{
  GObject parent_instance;

  char *filename;
  char *mountpoint;
};

G_DEFINE_TYPE_WITH_CODE (GFileUnix, g_file_unix, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_file_unix_file_iface_init))

static void
g_file_unix_finalize (GObject *object)
{
  GFileUnix *unix_file;

  unix_file = G_FILE_UNIX (object);

  g_free (unix_file->filename);
  g_free (unix_file->mountpoint);
  
  if (G_OBJECT_CLASS (g_file_unix_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_unix_parent_class)->finalize) (object);
}

static void
g_file_unix_class_init (GFileUnixClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_unix_finalize;
}

static void
g_file_unix_init (GFileUnix *unix_file)
{
}

GFile *
g_file_unix_new (const char *filename,
		 const char *mountpoint)
{
  GFileUnix *unix_file;
  int len;

  unix_file = g_object_new (G_TYPE_FILE_UNIX, NULL);
  unix_file->filename = g_strdup (filename);
  unix_file->mountpoint = g_strdup (mountpoint);

  /* Remove any trailing slashes */
  len = strlen (unix_file->filename);

  while (len > 1 && unix_file->filename[len-1] == '/')
    {
      unix_file->filename[len-1] = 0;
      len--;
    }
  
  return G_FILE (unix_file);
}

static gboolean
g_file_unix_is_native (GFile *file)
{
  return FALSE;
}

static char *
g_file_unix_get_path (GFile *file)
{
  return NULL;
}

static char *
g_file_unix_get_uri (GFile *file)
{
  /* TODO: implement to-uri */
  return NULL;
}

static char *
g_file_unix_get_parse_name (GFile *file)
{
  /* TODO: implement to-iri */
  return NULL;
}

static GFile *
g_file_unix_get_parent (GFile *file)
{
  GFileUnix *unix_file = G_FILE_UNIX (file);
  const char *file_name;
  GFileUnix *parent;
  const char *base;
  char *parent_file_name;
  gsize len;    

  file_name = unix_file->filename;
  base = strrchr (file_name, '/');
  if (base == NULL || base == file_name)
    return NULL;

  while (base > file_name && *base == '/')
    base--;

  len = (guint) 1 + base - file_name;
  
  parent_file_name = g_new (gchar, len + 1);
  g_memmove (parent_file_name, file_name, len);
  parent_file_name[len] = 0;

  parent = g_object_new (G_TYPE_FILE_UNIX, NULL);
  parent->filename = parent_file_name;
  parent->mountpoint = g_strdup (unix_file->mountpoint);
  
  return G_FILE (parent);
}

static GFile *
g_file_unix_copy (GFile *file)
{
  GFileUnix *unix_file = G_FILE_UNIX (file);

  return g_file_unix_new (unix_file->filename,
			  unix_file->mountpoint);
}


static GFile *
g_file_unix_get_child (GFile *file,
		       const char *name)
{
  GFileUnix *unix_file = G_FILE_UNIX (file);
  char *filename;
  GFile *child;

  filename = g_build_filename (unix_file->filename, name, NULL);

  child = g_file_unix_new (filename, unix_file->mountpoint);
  g_free (filename);
  
  return child;
}

static GFileEnumerator *
g_file_unix_enumerate_children (GFile      *file,
				GFileInfoRequestFlags requested,
				const char *attributes,
				gboolean follow_symlinks)
{
  /* TODO: implement */
  return NULL;
}

static GFileInfo *
g_file_unix_get_info (GFile                *file,
		      GFileInfoRequestFlags requested,
		      const char           *attributes,
		      gboolean              follow_symlinks,
		      GError              **error)
{
  /* TODO: implement */
  return NULL;
}

/* receive a file descriptor over file descriptor fd */
static int 
receive_fd (int connection_fd)
{
  struct msghdr msg;
  struct iovec iov;
  char buf[1];
  int rv;
  char ccmsg[CMSG_SPACE (sizeof(int))];
  struct cmsghdr *cmsg;

  iov.iov_base = buf;
  iov.iov_len = 1;
  msg.msg_name = 0;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ccmsg;
  msg.msg_controllen = sizeof (ccmsg);
  
  rv = recvmsg (connection_fd, &msg, 0);
  if (rv == -1) 
    {
      perror ("recvmsg");
      return -1;
    }

  cmsg = CMSG_FIRSTHDR (&msg);
  if (!cmsg->cmsg_type == SCM_RIGHTS) {
    g_warning("got control message of unknown type %d", 
	      cmsg->cmsg_type);
    return -1;
  }

  return *(int*)CMSG_DATA(cmsg);
}

static GFileInputStream *
g_file_unix_read (GFile *file)
{
  GFileUnix *unix_file = G_FILE_UNIX (file);
  DBusConnection *connection;
  DBusMessage *message, *reply;
  DBusError error;
  char *str;
  int fd, extra_fd;

  connection = _g_vfs_unix_get_connection_sync (unix_file->mountpoint, &extra_fd);

  message = dbus_message_new_method_call ("org.gtk.vfs.Daemon",
					  G_VFS_DBUS_DAEMON_PATH,
					  G_VFS_DBUS_DAEMON_INTERFACE,
					  G_VFS_DBUS_OP_READ_FILE);

  
  /* TODO: strings are utf8, filenames are not */
  if (!dbus_message_append_args (message, 
				 DBUS_TYPE_STRING, &unix_file->filename,
				 0))
      g_error ("Out of memory");

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1,
						     &error);
  dbus_message_unref (message);

  if (!reply)
    {
      g_warning ("Error while running READ_FILE: %s",
		 error.message);
      dbus_error_free (&error);
      return NULL;
    }
  
  dbus_message_get_args (reply, NULL,
			 DBUS_TYPE_STRING, &str,
			 DBUS_TYPE_INVALID);

  g_print ("read_file: %s\n", str);

  fd = receive_fd (extra_fd);
  g_print ("new fd: %d\n", fd);

  return NULL;
}

static GFileOutputStream *
g_file_unix_append_to (GFile *file)
{
  /* TODO: implement */
  return NULL;
}

static GFileOutputStream *
g_file_unix_create (GFile *file)
{
  /* TODO: implement */
  return NULL;
}

static GFileOutputStream *
g_file_unix_replace (GFile *file,
		     time_t mtime,
		     gboolean  make_backup)
{
  /* TODO: implement */
  return NULL;
}

static void
g_file_unix_file_iface_init (GFileIface *iface)
{
  iface->copy = g_file_unix_copy;
  iface->is_native = g_file_unix_is_native;
  iface->get_path = g_file_unix_get_path;
  iface->get_uri = g_file_unix_get_uri;
  iface->get_parse_name = g_file_unix_get_parse_name;
  iface->get_parent = g_file_unix_get_parent;
  iface->get_child = g_file_unix_get_child;
  iface->enumerate_children = g_file_unix_enumerate_children;
  iface->get_info = g_file_unix_get_info;
  iface->read = g_file_unix_read;
  iface->append_to = g_file_unix_append_to;
  iface->create = g_file_unix_create;
  iface->replace = g_file_unix_replace;
}
