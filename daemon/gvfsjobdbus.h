#ifndef __G_VFS_JOB_DBUS_H__
#define __G_VFS_JOB_DBUS_H__

#include <dbus/dbus.h>
#include <gvfsjob.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_DBUS         (g_vfs_job_dbus_get_type ())
#define G_VFS_JOB_DBUS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_DBUS, GVfsJobDBus))
#define G_VFS_JOB_DBUS_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_DBUS, GVfsJobDBusClass))
#define G_VFS_IS_JOB_DBUS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_DBUS))
#define G_VFS_IS_JOB_DBUS_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_DBUS))
#define G_VFS_JOB_DBUS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_DBUS, GVfsJobDBusClass))

typedef struct _GVfsJobDBus        GVfsJobDBus;
typedef struct _GVfsJobDBusClass   GVfsJobDBusClass;

struct _GVfsJobDBus
{
  GVfsJob parent_instance;

  DBusConnection *connection;
  DBusMessage *message;
};

struct _GVfsJobDBusClass
{
  GVfsJobClass parent_class;

  /* Might be called on an i/o thread */
  DBusMessage * (*create_reply) (GVfsJob *job,
				 DBusConnection *connection,
				 DBusMessage *message);
};

GType g_vfs_job_dbus_get_type (void) G_GNUC_CONST;

gboolean        g_vfs_job_dbus_is_serial      (GVfsJobDBus    *job_dbus,
					       DBusConnection *connection,
					       dbus_uint32_t   serial);
DBusConnection *g_vfs_job_dbus_get_connection (GVfsJobDBus    *job_dbus);
DBusMessage    *g_vfs_job_dbus_get_message    (GVfsJobDBus    *job_dbus);

G_END_DECLS

#endif /* __G_VFS_JOB_DBUS_H__ */
