#ifndef __G_MOUNT_OPERATION_DBUS_H__
#define __G_MOUNT_OPERATION_DBUS_H__

#include <sys/stat.h>

#include <glib-object.h>
#include <gio/gmountoperation.h>
#include <gmountspec.h>
#include <gmountsource.h>

G_BEGIN_DECLS

GMountSource *g_mount_operation_dbus_wrap (GMountOperation *op,
					   GMountSpec *spec);

G_END_DECLS

#endif /* __G_MOUNT_OPERATION_DBUS_H__ */
