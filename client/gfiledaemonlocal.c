#include <config.h>

#include <string.h>

#include <gio/gfileinputstreamlocal.h>
#include <gio/gfileoutputstreamlocal.h>
#include "gfiledaemonlocal.h"
#include <glib/gi18n-lib.h>

static void g_file_daemon_local_file_iface_init (GFileIface       *iface);

struct _GFileDaemonLocal
{
  GObject parent_instance;

  GFile *wrapped;
};

G_DEFINE_TYPE_WITH_CODE (GFileDaemonLocal, g_file_daemon_local, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_file_daemon_local_file_iface_init))

static void
g_file_daemon_local_finalize (GObject *object)
{
  GFileDaemonLocal *daemon_local;

  daemon_local = G_FILE_DAEMON_LOCAL (object);

  g_object_unref (daemon_local->wrapped);
  
  if (G_OBJECT_CLASS (g_file_daemon_local_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_daemon_local_parent_class)->finalize) (object);
}

static void
g_file_daemon_local_class_init (GFileDaemonLocalClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_daemon_local_finalize;
}

static void
g_file_daemon_local_init (GFileDaemonLocal *daemon_local)
{
}

GFile *
g_file_daemon_local_new (GFile *wrapped)
{
  GFileDaemonLocal *daemon_local = g_object_new (G_TYPE_FILE_DAEMON_LOCAL, NULL);
  daemon_local->wrapped = wrapped;
  return G_FILE (daemon_local);
}

static gboolean
g_file_daemon_local_is_native (GFile *file)
{
  return TRUE;
}

static char *
g_file_daemon_local_get_path (GFile *file)
{
  return g_file_get_path (G_FILE_DAEMON_LOCAL (file)->wrapped);
}

static char *
g_file_daemon_local_get_uri (GFile *file)
{
  return g_file_get_uri (G_FILE_DAEMON_LOCAL (file)->wrapped);
}

static char *
g_file_daemon_local_get_parse_name (GFile *file)
{
  return g_file_get_parse_name (G_FILE_DAEMON_LOCAL (file)->wrapped);
}

static GFile *
g_file_daemon_local_get_parent (GFile *file)
{
  GFile *parent;

  parent = g_file_get_parent (G_FILE_DAEMON_LOCAL (file)->wrapped);
  if (parent == NULL)
    return NULL;
  return g_file_daemon_local_new (parent);
}

static GFile *
g_file_daemon_local_copy (GFile *file)
{
  GFile *copy;

  copy = g_file_copy (G_FILE_DAEMON_LOCAL (file)->wrapped);
  return g_file_daemon_local_new (copy);
}


static GFile *
g_file_daemon_local_get_child (GFile *file,
			       const char *name)
{
  GFile *child;

  child = g_file_get_child (G_FILE_DAEMON_LOCAL (file)->wrapped, name);
  if (child == NULL)
    return NULL;
  
  return g_file_daemon_local_new (child);
}

static GFileEnumerator *
g_file_daemon_local_enumerate_children (GFile *file,
					const char *attributes,
					GFileGetInfoFlags flags,
					GCancellable *cancellable,
					GError **error)
{
  return g_file_enumerate_children (G_FILE_DAEMON_LOCAL (file)->wrapped,
				    attributes, flags,
				    cancellable, error);
}

static GFileInfo *
g_file_daemon_local_get_info (GFile *file,
			      const char *attributes,
			      GFileGetInfoFlags flags,
			      GCancellable *cancellable,
			      GError **error)
{
  return g_file_get_info (G_FILE_DAEMON_LOCAL (file)->wrapped,
			  attributes, flags,
			  cancellable, error);
}

static GFileInputStream *
g_file_daemon_local_read (GFile *file,
			  GCancellable *cancellable,
			  GError **error)
{
  return g_file_read (G_FILE_DAEMON_LOCAL (file)->wrapped,
		      cancellable, error);
}

static GFileOutputStream *
g_file_daemon_local_append_to (GFile *file,
			       GCancellable *cancellable,
			       GError **error)
{
  return g_file_append_to (G_FILE_DAEMON_LOCAL (file)->wrapped, cancellable, error);
}

static GFileOutputStream *
g_file_daemon_local_create (GFile *file,
			    GCancellable *cancellable,
			    GError **error)
{
  return g_file_create (G_FILE_DAEMON_LOCAL (file)->wrapped, cancellable, error);
}

static GFileOutputStream *
g_file_daemon_local_replace (GFile *file,
			     time_t mtime,
			     gboolean  make_backup,
			     GCancellable *cancellable,
			     GError **error)
{
  return g_file_replace (G_FILE_DAEMON_LOCAL (file)->wrapped, mtime, make_backup, cancellable, error);
}

static void
g_file_daemon_local_mount (GFile *file,
			   GMountOperation *mount_op)
{
  return g_file_mount (G_FILE_DAEMON_LOCAL (file)->wrapped, mount_op);
}

static void
g_file_daemon_local_file_iface_init (GFileIface *iface)
{
  iface->copy = g_file_daemon_local_copy;
  iface->is_native = g_file_daemon_local_is_native;
  iface->get_path = g_file_daemon_local_get_path;
  iface->get_uri = g_file_daemon_local_get_uri;
  iface->get_parse_name = g_file_daemon_local_get_parse_name;
  iface->get_parent = g_file_daemon_local_get_parent;
  iface->get_child = g_file_daemon_local_get_child;
  iface->enumerate_children = g_file_daemon_local_enumerate_children;
  iface->get_info = g_file_daemon_local_get_info;
  iface->read = g_file_daemon_local_read;
  iface->append_to = g_file_daemon_local_append_to;
  iface->create = g_file_daemon_local_create;
  iface->replace = g_file_daemon_local_replace;
  iface->mount = g_file_daemon_local_mount;
}
