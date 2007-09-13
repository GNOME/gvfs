#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsdaemonoperation.h"

G_DEFINE_TYPE (GVfsDaemonOperation, g_vfs_daemon_operation, G_TYPE_VFS_DAEMON);

enum {
  CANCEL,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
g_vfs_daemon_operation_finalize (GObject *object)
{
  GVfsDaemonOperation *daemon;

  daemon = G_VFS_DAEMON_OPERATION (object);

  if (daemon->error)
    g_error_free (daemon->error);
  
  if (G_OBJECT_CLASS (g_vfs_daemon_operation_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_daemon_operation_parent_class)->finalize) (object);
}

static void
g_vfs_daemon_operation_class_init (GVfsDaemonOperationClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsDaemonClass *daemon_class = G_VFS_DAEMON_CLASS (klass);
  
  gobject_class->finalize = g_vfs_daemon_operation_finalize;

  signals[CANCEL] =
    g_signal_new (I_("done"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsDaemonOperation, cancel),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);

}

static void
g_vfs_daemon_operation_init (GVfsDaemonOperation *daemon)
{
}

GVfsDaemonOperationKind 
g_vfs_daemon_operation_get_kind (GVfsDaemonOperation *op)
{
  return op->kind;
}

void
g_vfs_daemon_operation_cancel (GVfsDaemonOperation *op)
{
  if (op->cancelled)
    return;

  op->cancelled = TRUE;
  g_signal_emit (op, signals[CANCEL], 0);
}

void
g_vfs_daemon_operation_set_failed (GVfsDaemonOperation *op,
				   GError *error)
{
  if (op->failed)
    return;

  op->failed = TRUE;
  op->error = g_error_copy (error);
}
