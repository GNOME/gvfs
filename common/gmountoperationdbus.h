#ifndef __G_MOUNT_OPERATION_DBUS_H__
#define __G_MOUNT_OPERATION_DBUS_H__

#include <sys/stat.h>

#include <glib-object.h>
#include <gio/gmountoperation.h>
#include <gmountspec.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

#define G_TYPE_MOUNT_OPERATION_DBUS         (g_mount_operation_dbus_get_type ())
#define G_MOUNT_OPERATION_DBUS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MOUNT_OPERATION_DBUS, GMountOperationDBus))
#define G_MOUNT_OPERATION_DBUS_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MOUNT_OPERATION_DBUS, GMountOperationDBusClass))
#define G_IS_MOUNT_OPERATION_DBUS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MOUNT_OPERATION_DBUS))
#define G_IS_MOUNT_OPERATION_DBUS_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MOUNT_OPERATION_DBUS))
#define G_MOUNT_OPERATION_DBUS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_MOUNT_OPERATION_DBUS, GMountOperationDBusClass))

typedef struct _GMountOperationDBus        GMountOperationDBus;
typedef struct _GMountOperationDBusClass   GMountOperationDBusClass;

struct _GMountOperationDBus
{
  GMountOperation parent_instance;

  char *obj_path;
  DBusConnection *connection;
  GMountSpec *mount_spec;
};

struct _GMountOperationDBusClass
{
  GMountOperationClass parent_class;
};

GType g_mount_operation_dbus_get_type (void) G_GNUC_CONST;
  
GMountOperationDBus *g_mount_operation_dbus_new          (GMountSpec          *spec);
void                 g_mount_operation_dbus_fail_at_idle (GMountOperationDBus *op,
							  GError              *error);

G_END_DECLS

#endif /* __G_MOUNT_OPERATION_DBUS_H__ */
