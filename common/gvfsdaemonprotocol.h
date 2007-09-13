#ifndef __G_VFS_DAEMON_PROTOCOL_H__
#define __G_VFS_DAEMON_PROTOCOL_H__

#include <gio/gfileinfo.h>

G_BEGIN_DECLS

/* The well known name of the main daemon */
#define G_VFS_DBUS_DAEMON_NAME "org.gtk.vfs.Daemon"

/* The mount tracking interface in the main daemon */
#define G_VFS_DBUS_MOUNTTRACKER_INTERFACE "org.gtk.vfs.MountTracker"
#define G_VFS_DBUS_MOUNTTRACKER_PATH "/org/gtk/vfs/mounttracker"
#define G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT "lookupMount"
#define G_VFS_DBUS_MOUNTTRACKER_OP_MOUNT_LOCATION "mountLocation"
#define G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNTS "listMounts"
#define G_VFS_DBUS_MOUNTTRACKER_OP_REGISTER_MOUNT "registerMount"
#define G_VFS_DBUS_MOUNTTRACKER_SIGNAL_MOUNTED "mounted"
#define G_VFS_DBUS_MOUNTTRACKER_SIGNAL_UNMOUNTED "unmounted"

/* Each mount (there might be several in a daemon) implements one of these interfaces
   for standard i/o operations */
#define G_VFS_DBUS_MOUNT_INTERFACE "org.gtk.vfs.Mount"
#define G_VFS_DBUS_MOUNT_OP_OPEN_FOR_READ "OpenForRead"
#define G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE "OpenForWrite"
#define G_VFS_DBUS_MOUNT_OP_GET_INFO "GetInfo"
#define G_VFS_DBUS_MOUNT_OP_GET_FILESYSTEM_INFO "GetFilesystemInfo"
#define G_VFS_DBUS_MOUNT_OP_ENUMERATE "Enumerate"
#define G_VFS_DBUS_MOUNT_OP_MOUNT_MOUNTABLE "MountMountable"
#define G_VFS_DBUS_MOUNT_OP_SET_DISPLAY_NAME "SetDisplayName"
#define G_VFS_DBUS_MOUNT_OP_DELETE "Delete"
#define G_VFS_DBUS_MOUNT_OP_TRASH "Trash"
#define G_VFS_DBUS_MOUNT_OP_MAKE_DIRECTORY "MakeDirectory"
#define G_VFS_DBUS_MOUNT_OP_MAKE_SYMBOLIC_LINK "MakeSymbolicLink"
#define G_VFS_DBUS_MOUNT_OP_COPY "Copy"
#define G_VFS_DBUS_MOUNT_OP_MOVE "Move"
#define G_VFS_DBUS_MOUNT_OP_SET_ATTRIBUTE "SetAttribute"
#define G_VFS_DBUS_MOUNT_OP_QUERY_SETTABLE_ATTRIBUTES "QuerySettableAttributes"
#define G_VFS_DBUS_MOUNT_OP_QUERY_WRITABLE_NAMESPACES "QueryWritableNamespaces"

/* Progress callback interface for copy and move */
#define G_VFS_DBUS_PROGRESS_INTERFACE "org.gtk.vfs.Progress"
#define G_VFS_DBUS_PROGRESS_OP_PROGRESS "Progress"

/* mount daemons that support mounting more mounts implement this,
   and set the dbus name in the mountable description file */
#define G_VFS_DBUS_MOUNTABLE_INTERFACE "org.gtk.vfs.Mountable"
#define G_VFS_DBUS_MOUNTABLE_PATH "/org/gtk/vfs/mountable"
#define G_VFS_DBUS_MOUNTABLE_OP_MOUNT "mount"

#define G_VFS_DBUS_ERROR_SOCKET_FAILED "org.gtk.vfs.Error.SocketFailed"

/* Each daemon (main and for mounts) implement this. */
#define G_VFS_DBUS_DAEMON_INTERFACE "org.gtk.vfs.Daemon"
#define G_VFS_DBUS_DAEMON_PATH "/org/gtk/vfs/Daemon"
#define G_VFS_DBUS_OP_GET_CONNECTION "GetConnection"
#define G_VFS_DBUS_OP_CANCEL "Cancel"

/* Used by the dbus-proxying implementation of GMoutOperation */
#define G_VFS_DBUS_MOUNT_OPERATION_INTERFACE "org.gtk.vfs.MountOperation"
#define G_VFS_DBUS_MOUNT_OPERATION_OP_ASK_PASSWORD "askPassword"
#define G_VFS_DBUS_MOUNT_OPERATION_OP_ASK_QUESTION "askQuestion"

/* Implemented by the spawner of a process, the spawned process sends the
   spawned message (with noreply) when it has spawned and gotten a dbus id */
#define G_VFS_DBUS_SPAWNER_INTERFACE "org.gtk.vfs.Spawner"
#define G_VFS_DBUS_OP_SPAWNED "spawned"

/* Implemented by client side for a file enumerator */
#define G_VFS_DBUS_ENUMERATOR_INTERFACE "org.gtk.vfs.Enumerator"
#define G_VFS_DBUS_ENUMERATOR_OP_DONE "Done"
#define G_VFS_DBUS_ENUMERATOR_OP_GOT_INFO "GotInfo"

/* Mounts time out in 10 minutes, since they can be slow, with auth, etc */
#define G_VFS_DBUS_MOUNT_TIMEOUT_MSECS (1000*60*10)
/* Normal ops are faster, one minute timeout */
#define G_VFS_DBUS_TIMEOUT_MSECS (1000*60)

typedef struct {
  guint32 command;
  guint32 seq_nr;
  guint32 arg1;
  guint32 arg2;
  guint32 data_len;
} GVfsDaemonSocketProtocolRequest;

#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE sizeof(GVfsDaemonSocketProtocolRequest)

#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_READ 0
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_WRITE 1
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CLOSE 2
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL 3
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR 4
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET 5
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END 6

/*
read, readahead reply:
type, seek_generation, size, data

seek reply:
type, pos (64),

error:
type, code, size, data (size bytes, 2 strings: domain, message)
*/

typedef struct {
  guint32 type;
  guint32 seq_nr;
  guint32 arg1;
  guint32 arg2;
} GVfsDaemonSocketProtocolReply;

#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE 16

#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA     0
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR    1
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS 2
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_WRITTEN  3
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED   4

#define G_FILE_INFO_INNER_TYPE_AS_STRING         \
  DBUS_TYPE_ARRAY_AS_STRING			 \
    DBUS_STRUCT_BEGIN_CHAR_AS_STRING		 \
      DBUS_TYPE_STRING_AS_STRING		 \
      DBUS_TYPE_VARIANT_AS_STRING		 \
    DBUS_STRUCT_END_CHAR_AS_STRING

#define G_FILE_INFO_TYPE_AS_STRING      \
  DBUS_STRUCT_BEGIN_CHAR_AS_STRING      \
    G_FILE_INFO_INNER_TYPE_AS_STRING    \
  DBUS_STRUCT_END_CHAR_AS_STRING 


gchar     *_g_dbus_type_from_file_attribute_type (GFileAttributeType          type);
void       _g_dbus_append_file_attribute         (DBusMessageIter            *iter,
						  const char                 *attribute,
						  const GFileAttributeValue  *value);
void       _g_dbus_append_file_info              (DBusMessageIter            *iter,
						  GFileInfo                  *file_info);
gboolean   _g_dbus_get_file_attribute            (DBusMessageIter            *iter,
						  gchar                     **attribute,
						  GFileAttributeValue        *value);
GFileInfo *_g_dbus_get_file_info                 (DBusMessageIter            *iter,
						  GError                    **error);

GFileAttributeInfoList *_g_dbus_get_attribute_info_list    (DBusMessageIter         *iter,
							    GError                 **error);
void                    _g_dbus_append_attribute_info_list (DBusMessageIter         *iter,
							    GFileAttributeInfoList  *list);

G_END_DECLS

#endif /* __G_VFS_DAEMON_PROTOCOL_H__ */
