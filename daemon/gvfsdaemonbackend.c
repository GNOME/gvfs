#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsdaemonbackend.h"

G_DEFINE_TYPE (GVfsDaemonBackend, g_vfs_daemon_backend, G_TYPE_OBJECT);

static void
g_vfs_daemon_backend_finalize (GObject *object)
{
  GVfsDaemonBackend *daemon;

  daemon = G_VFS_DAEMON_BACKEND (object);

  if (G_OBJECT_CLASS (g_vfs_daemon_backend_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_daemon_backend_parent_class)->finalize) (object);
}

static void
g_vfs_daemon_backend_class_init (GVfsDaemonBackendClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_vfs_daemon_backend_finalize;
}

static void
g_vfs_daemon_backend_init (GVfsDaemonBackend *daemon)
{
}

