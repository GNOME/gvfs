#ifndef __G_VFS_MOUNTPOINT_H__
#define __G_VFS_MOUNTPOINT_H__

#include <glib.h>
#include <dbus/dbus.h>

#define G_VFS_MOUNTPOINT_SIGNATURE		\
  DBUS_TYPE_STRING_AS_STRING			\
  DBUS_TYPE_STRING_AS_STRING			\
  DBUS_TYPE_STRING_AS_STRING			\
  DBUS_TYPE_INT32_AS_STRING			\
  DBUS_TYPE_ARRAY_AS_STRING			\
    DBUS_TYPE_BYTE_AS_STRING			\

typedef struct {
  char *method;
  char *user;
  char *host;
  int port;
  char *path;
} GVfsMountpoint;

GVfsMountpoint *g_vfs_mountpoint_copy      (GVfsMountpoint  *mountpoint);
void            g_vfs_mountpoint_free      (GVfsMountpoint  *mountpoint);
GVfsMountpoint *g_vfs_mountpoint_from_dbus (DBusMessageIter *iter);
void            g_vfs_mountpoint_to_dbus   (GVfsMountpoint  *mountpoint,
					    DBusMessageIter *iter);

#endif /* __G_VFS_MOUNTPOINT_H__ */
