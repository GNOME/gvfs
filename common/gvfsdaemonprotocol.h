#ifndef __G_VFS_DAEMON_PROTOCOL_H__
#define __G_VFS_DAEMON_PROTOCOL_H__

#include <gio/gio.h>

G_BEGIN_DECLS

/* The well known name of the main daemon */
#define G_VFS_DBUS_DAEMON_NAME "org.gtk.vfs.Daemon"

/* The mount tracking interface in the main daemon */
#define G_VFS_DBUS_MOUNTTRACKER_INTERFACE "org.gtk.vfs.MountTracker"
#define G_VFS_DBUS_MOUNTTRACKER_PATH "/org/gtk/vfs/mounttracker"
#define G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT "lookupMount"
#define G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT_BY_FUSE_PATH "lookupMountByFusePath"
#define G_VFS_DBUS_MOUNTTRACKER_OP_MOUNT_LOCATION "mountLocation"
#define G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNTS "listMounts"
#define G_VFS_DBUS_MOUNTTRACKER_OP_REGISTER_MOUNT "registerMount"
#define G_VFS_DBUS_MOUNTTRACKER_OP_UNREGISTER_MOUNT "unregisterMount"
#define G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNT_TYPES "listMountTypes"
#define G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNTABLE_INFO "listMountableInfo"
#define G_VFS_DBUS_MOUNTTRACKER_OP_REGISTER_FUSE "registerFuse"
#define G_VFS_DBUS_MOUNTTRACKER_SIGNAL_MOUNTED "mounted"
#define G_VFS_DBUS_MOUNTTRACKER_SIGNAL_UNMOUNTED "unmounted"

/* Each mount (there might be several in a daemon) implements one of these interfaces
   for standard i/o operations */
#define G_VFS_DBUS_MOUNT_INTERFACE "org.gtk.vfs.Mount"
#define G_VFS_DBUS_MOUNT_OP_UNMOUNT "Unmount"
#define G_VFS_DBUS_MOUNT_OP_OPEN_FOR_READ "OpenForRead"
#define G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE "OpenForWrite"
#define G_VFS_DBUS_MOUNT_OP_QUERY_INFO "QueryInfo"
#define G_VFS_DBUS_MOUNT_OP_QUERY_FILESYSTEM_INFO "QueryFilesystemInfo"
#define G_VFS_DBUS_MOUNT_OP_ENUMERATE "Enumerate"
#define G_VFS_DBUS_MOUNT_OP_CREATE_DIR_MONITOR "CreateDirectoryMonitor"
#define G_VFS_DBUS_MOUNT_OP_CREATE_FILE_MONITOR "CreateFileMonitor"
#define G_VFS_DBUS_MOUNT_OP_MOUNT_MOUNTABLE "MountMountable"
#define G_VFS_DBUS_MOUNT_OP_UNMOUNT_MOUNTABLE "UnountMountable"
#define G_VFS_DBUS_MOUNT_OP_EJECT_MOUNTABLE "EjectMountable"
#define G_VFS_DBUS_MOUNT_OP_START_MOUNTABLE "StartMountable"
#define G_VFS_DBUS_MOUNT_OP_STOP_MOUNTABLE "StopMountable"
#define G_VFS_DBUS_MOUNT_OP_POLL_MOUNTABLE "PollMountable"
#define G_VFS_DBUS_MOUNT_OP_SET_DISPLAY_NAME "SetDisplayName"
#define G_VFS_DBUS_MOUNT_OP_DELETE "Delete"
#define G_VFS_DBUS_MOUNT_OP_TRASH "Trash"
#define G_VFS_DBUS_MOUNT_OP_MAKE_DIRECTORY "MakeDirectory"
#define G_VFS_DBUS_MOUNT_OP_MAKE_SYMBOLIC_LINK "MakeSymbolicLink"
#define G_VFS_DBUS_MOUNT_OP_COPY "Copy"
#define G_VFS_DBUS_MOUNT_OP_MOVE "Move"
#define G_VFS_DBUS_MOUNT_OP_PUSH "Push"
#define G_VFS_DBUS_MOUNT_OP_PULL "Pull"
#define G_VFS_DBUS_MOUNT_OP_SET_ATTRIBUTE "SetAttribute"
#define G_VFS_DBUS_MOUNT_OP_QUERY_SETTABLE_ATTRIBUTES "QuerySettableAttributes"
#define G_VFS_DBUS_MOUNT_OP_QUERY_WRITABLE_NAMESPACES "QueryWritableNamespaces"
#define G_VFS_DBUS_MOUNT_OP_OPEN_ICON_FOR_READ "OpenIconForRead"

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
#define G_VFS_DBUS_MOUNT_OPERATION_OP_SHOW_PROCESSES "showProcesses"
#define G_VFS_DBUS_MOUNT_OPERATION_OP_ABORTED "aborted"

/* Implemented by the spawner of a process, the spawned process sends the
   spawned message (with noreply) when it has spawned and gotten a dbus id */
#define G_VFS_DBUS_SPAWNER_INTERFACE "org.gtk.vfs.Spawner"
#define G_VFS_DBUS_OP_SPAWNED "spawned"

/* Implemented by client side for a file enumerator */
#define G_VFS_DBUS_ENUMERATOR_INTERFACE "org.gtk.vfs.Enumerator"
#define G_VFS_DBUS_ENUMERATOR_OP_DONE "Done"
#define G_VFS_DBUS_ENUMERATOR_OP_GOT_INFO "GotInfo"

#define G_VFS_DBUS_MONITOR_INTERFACE "org.gtk.vfs.Monitor"
#define G_VFS_DBUS_MONITOR_OP_SUBSCRIBE "Subscribe"
#define G_VFS_DBUS_MONITOR_OP_UNSUBSCRIBE "Unsubscribe"

#define G_VFS_DBUS_MONITOR_CLIENT_INTERFACE "org.gtk.vfs.MonitorClient"
#define G_VFS_DBUS_MONITOR_CLIENT_OP_CHANGED "Changed"

/* The well known name of the metadata daemon */
#define G_VFS_DBUS_METADATA_NAME "org.gtk.vfs.Metadata"
#define G_VFS_DBUS_METADATA_PATH "/org/gtk/vfs/metadata"
#define G_VFS_DBUS_METADATA_INTERFACE "org.gtk.vfs.Metadata"
#define G_VFS_DBUS_METADATA_OP_SET "Set"
#define G_VFS_DBUS_METADATA_OP_GET "Get"
#define G_VFS_DBUS_METADATA_OP_UNSET "Unset"
#define G_VFS_DBUS_METADATA_OP_REMOVE "Remove"
#define G_VFS_DBUS_METADATA_OP_MOVE "Move"

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
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET 4
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END 5
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_QUERY_INFO 6

/*
read, readahead reply:
type, seek_generation, size, data

seek reply:
type, pos (64),

error:
type, code, size, data (size bytes, 2 strings: domain, message)

info:
type,    0, size, data 

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
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_INFO     5

#define G_FILE_INFO_INNER_TYPE_AS_STRING         \
  DBUS_TYPE_ARRAY_AS_STRING			 \
    DBUS_STRUCT_BEGIN_CHAR_AS_STRING		 \
      DBUS_TYPE_STRING_AS_STRING		 \
      DBUS_TYPE_UINT32_AS_STRING		 \
      DBUS_TYPE_VARIANT_AS_STRING		 \
    DBUS_STRUCT_END_CHAR_AS_STRING

#define G_FILE_INFO_TYPE_AS_STRING      \
  DBUS_STRUCT_BEGIN_CHAR_AS_STRING      \
    G_FILE_INFO_INNER_TYPE_AS_STRING    \
  DBUS_STRUCT_END_CHAR_AS_STRING 


typedef union {
  gboolean boolean;
  guint32 uint32;
  guint64 uint64;
  gpointer ptr;
} GDbusAttributeValue;

void       _g_dbus_attribute_value_destroy       (GFileAttributeType          type,
						  GDbusAttributeValue        *value);
gpointer   _g_dbus_attribute_as_pointer          (GFileAttributeType          type,
						  GDbusAttributeValue        *value);
const char*_g_dbus_type_from_file_attribute_type (GFileAttributeType          type);
void       _g_dbus_append_file_attribute         (DBusMessageIter            *iter,
						  const char                 *attribute,
						  GFileAttributeStatus        status,
						  GFileAttributeType          type,
						  gpointer                    value_p);
void       _g_dbus_append_file_info              (DBusMessageIter            *iter,
						  GFileInfo                  *file_info);
gboolean   _g_dbus_get_file_attribute            (DBusMessageIter            *iter,
						  gchar                     **attribute,
						  GFileAttributeStatus       *status,
						  GFileAttributeType         *type,
						  GDbusAttributeValue        *value);
GFileInfo *_g_dbus_get_file_info                 (DBusMessageIter            *iter,
						  GError                    **error);

GFileAttributeInfoList *_g_dbus_get_attribute_info_list    (DBusMessageIter         *iter,
							    GError                 **error);
void                    _g_dbus_append_attribute_info_list (DBusMessageIter         *iter,
							    GFileAttributeInfoList  *list);

G_END_DECLS

#endif /* __G_VFS_DAEMON_PROTOCOL_H__ */
