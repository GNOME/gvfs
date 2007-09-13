#ifndef __G_UNIX_MOUNTS_H__
#define __G_UNIX_MOUNTS_H__

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
	char *mount_path;
	char *device_path;
	char *filesystem_type;
	gboolean is_read_only;
} GUnixMount;

typedef struct {
	char *mount_path;
	char *device_path;
	char *filesystem_type;
	char *dev_opt;
	gboolean is_read_only;
	gboolean is_user_mountable;
	gboolean is_loopback;
} GUnixMountPoint;


typedef enum {
  G_UNIX_MOUNT_TYPE_UNKNOWN,
  G_UNIX_MOUNT_TYPE_FLOPPY,
  G_UNIX_MOUNT_TYPE_CDROM,
  G_UNIX_MOUNT_TYPE_NFS,
  G_UNIX_MOUNT_TYPE_ZIP,
  G_UNIX_MOUNT_TYPE_JAZ,
  G_UNIX_MOUNT_TYPE_MEMSTICK,
  G_UNIX_MOUNT_TYPE_CF,
  G_UNIX_MOUNT_TYPE_SM,
  G_UNIX_MOUNT_TYPE_SDMMC,
  G_UNIX_MOUNT_TYPE_IPOD,
  G_UNIX_MOUNT_TYPE_CAMERA,
  G_UNIX_MOUNT_TYPE_HD,
} GUnixMountType;

typedef void (* GUnixMountCallback) (gpointer user_data);

void     _g_unix_mount_free             (GUnixMount          *mount_entry);
void     _g_unix_mount_point_free       (GUnixMountPoint     *mount_point);
gint     _g_unix_mount_compare          (GUnixMount          *mount_entry1,
					 GUnixMount          *mount_entry2);
gint     _g_unix_mount_point_compare    (GUnixMountPoint     *mount_point1,
					 GUnixMountPoint     *mount_point2);
gboolean _g_get_unix_mount_points       (GList              **return_list);
gboolean _g_get_unix_mounts             (GList              **return_list);
gpointer _g_monitor_unix_mounts         (GUnixMountCallback   mountpoints_changed,
					 GUnixMountCallback   mounts_changed,
					 gpointer             user_data);
void     _g_stop_monitoring_unix_mounts (gpointer             tag);

GUnixMountType _g_guess_type_for_mount  (const char     *mount_path,
					 const char     *device_path,
					 const char     *filesystem_type);


G_END_DECLS

#endif /* __G_UNIX_MOUNTS_H__ */
