#ifndef __G_VFS_MOUNTPOINT_H__
#define __G_VFS_MOUNTPOINT_H__

#include <glib.h>
#include <dbus/dbus.h>


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
