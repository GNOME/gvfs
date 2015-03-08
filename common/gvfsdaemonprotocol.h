#ifndef __G_VFS_DAEMON_PROTOCOL_H__
#define __G_VFS_DAEMON_PROTOCOL_H__

#include <gio/gio.h>

G_BEGIN_DECLS

/* The well known name of the main daemon */
#define G_VFS_DBUS_DAEMON_NAME "org.gtk.vfs.Daemon"

#define G_VFS_DBUS_MOUNTTRACKER_PATH "/org/gtk/vfs/mounttracker"
#define G_VFS_DBUS_MOUNTABLE_PATH "/org/gtk/vfs/mountable"
#define G_VFS_DBUS_DAEMON_PATH "/org/gtk/vfs/Daemon"
#define G_VFS_DBUS_METADATA_NAME "org.gtk.vfs.Metadata"
#define G_VFS_DBUS_METADATA_PATH "/org/gtk/vfs/metadata"

/* Mounts time out in 30 minutes, since they can be slow, with auth, etc */
#define G_VFS_DBUS_MOUNT_TIMEOUT_MSECS (1000*60*30)
/* Normal ops are faster, one minute timeout */
#define G_VFS_DBUS_TIMEOUT_MSECS (1000*60)

/* Flags for the OpenForWriteFlags method */
#define OPEN_FOR_WRITE_FLAG_CAN_SEEK     (1<<0)
#define OPEN_FOR_WRITE_FLAG_CAN_TRUNCATE (1<<1)

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
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_TRUNCATE 7

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
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_TRUNCATED 6


typedef union {
  gboolean boolean;
  guint32 uint32;
  guint64 uint64;
  gpointer ptr;
} GDBusAttributeValue;

void       _g_dbus_attribute_value_destroy       (GFileAttributeType          type,
						  GDBusAttributeValue        *value);
gpointer   _g_dbus_attribute_as_pointer          (GFileAttributeType          type,
						  GDBusAttributeValue        *value);
GVariant * _g_dbus_append_file_attribute         (const char                 *attribute,
						  GFileAttributeStatus        status,
						  GFileAttributeType          type,
						  gpointer                    value_p);
GVariant * _g_dbus_append_file_info              (GFileInfo                  *file_info);
gboolean   _g_dbus_get_file_attribute            (GVariant                   *value,
						  gchar                     **attribute,
						  GFileAttributeStatus       *status,
						  GFileAttributeType         *type,
						  GDBusAttributeValue        *attr_value);
GFileInfo *_g_dbus_get_file_info                 (GVariant                   *value,
						  GError                    **error);

GFileAttributeInfoList *_g_dbus_get_attribute_info_list    (GVariant                *value,
							    GError                 **error);
GVariant *              _g_dbus_append_attribute_info_list (GFileAttributeInfoList  *list);

G_END_DECLS

#endif /* __G_VFS_DAEMON_PROTOCOL_H__ */
