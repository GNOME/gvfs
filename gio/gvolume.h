#ifndef __G_VOLUME_H__
#define __G_VOLUME_H__

#include <glib-object.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_VOLUME            (g_volume_get_type ())
#define G_VOLUME(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_VOLUME, GVolume))
#define G_IS_VOLUME(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_VOLUME))
#define G_VOLUME_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_VOLUME, GVolumeIface))

typedef struct _GDrive          GDrive; /* Dummy typedef */
typedef struct _GVolume         GVolume; /* Dummy typedef */
typedef struct _GVolumeIface    GVolumeIface;

typedef void (*GVolumeCallback) (GError *error,
				 gpointer user_data);

struct _GVolumeIface
{
  GTypeInterface g_iface;

  /* signals */

  void (*changed) (GVolume *volume);
  
  /* Virtual Table */

  GFile *  (*get_root)    (GVolume         *volume);
  char *   (*get_name)    (GVolume         *volume);
  char *   (*get_icon)    (GVolume         *volume);
  GDrive * (*get_drive)   (GVolume         *volume);
  gboolean (*can_unmount) (GVolume         *volume);
  gboolean (*can_eject)   (GVolume         *volume);
  void     (*unmount)     (GVolume         *volume,
			   GVolumeCallback  callback,
			   gpointer         user_data);
  void     (*eject)       (GVolume         *volume,
			   GVolumeCallback  callback,
			   gpointer         user_data);
  char *   (*get_platform_id) (GVolume         *volume);
};

GType g_volume_get_type (void) G_GNUC_CONST;

GFile   *g_volume_get_root    (GVolume         *volume);
char *   g_volume_get_name    (GVolume         *volume);
char *   g_volume_get_icon    (GVolume         *volume);
GDrive * g_volume_get_drive   (GVolume         *volume);
gboolean g_volume_can_unmount (GVolume         *volume);
gboolean g_volume_can_eject   (GVolume         *volume);
void     g_volume_unmount     (GVolume         *volume,
			       GVolumeCallback  callback,
			       gpointer         user_data);
void     g_volume_eject       (GVolume         *volume,
			       GVolumeCallback  callback,
			       gpointer         user_data);


G_END_DECLS

#endif /* __G_VOLUME_H__ */
