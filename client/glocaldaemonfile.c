#include <config.h>

#include <string.h>

#include <gio/glocalfileinputstream.h>
#include <gio/glocalfileoutputstream.h>
#include "glocaldaemonfile.h"
#include <glib/gi18n-lib.h>

static void g_local_daemon_file_file_iface_init (GFileIface       *iface);

struct _GLocalDaemonFile
{
  GObject parent_instance;

  GFile *wrapped;
};

G_DEFINE_TYPE_WITH_CODE (GLocalDaemonFile, g_local_daemon_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_local_daemon_file_file_iface_init))

static void
g_local_daemon_file_finalize (GObject *object)
{
  GLocalDaemonFile *daemon_local;

  daemon_local = G_LOCAL_DAEMON_FILE (object);

  g_object_unref (daemon_local->wrapped);
  
  if (G_OBJECT_CLASS (g_local_daemon_file_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_local_daemon_file_parent_class)->finalize) (object);
}

static void
g_local_daemon_file_class_init (GLocalDaemonFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_local_daemon_file_finalize;
}

static void
g_local_daemon_file_init (GLocalDaemonFile *daemon_local)
{
}

GFile *
g_local_daemon_file_new (GFile *wrapped)
{
  GLocalDaemonFile *daemon_local = g_object_new (G_TYPE_LOCAL_DAEMON_FILE, NULL);
  daemon_local->wrapped = wrapped;
  return G_FILE (daemon_local);
}

static gboolean
g_local_daemon_file_is_native (GFile *file)
{
  return TRUE;
}

static char *
g_local_daemon_file_get_path (GFile *file)
{
  return g_file_get_path (G_LOCAL_DAEMON_FILE (file)->wrapped);
}

static char *
g_local_daemon_file_get_uri (GFile *file)
{
  return g_file_get_uri (G_LOCAL_DAEMON_FILE (file)->wrapped);
}

static char *
g_local_daemon_file_get_parse_name (GFile *file)
{
  return g_file_get_parse_name (G_LOCAL_DAEMON_FILE (file)->wrapped);
}

static GFile *
g_local_daemon_file_get_parent (GFile *file)
{
  GFile *parent;

  parent = g_file_get_parent (G_LOCAL_DAEMON_FILE (file)->wrapped);
  if (parent == NULL)
    return NULL;
  return g_local_daemon_file_new (parent);
}

static GFile *
g_local_daemon_file_dup (GFile *file)
{
  GFile *copy;

  copy = g_file_dup (G_LOCAL_DAEMON_FILE (file)->wrapped);
  return g_local_daemon_file_new (copy);
}

static guint
g_local_daemon_file_hash (GFile *file)
{
  return g_file_hash (G_LOCAL_DAEMON_FILE (file)->wrapped);
}

static gboolean
g_local_daemon_file_equal (GFile *file1,
			   GFile *file2)
{
  GFile *wrapped1 = G_LOCAL_DAEMON_FILE (file1)->wrapped;
  GFile *wrapped2;

  /* g_file_equal makes sure that if only one file is GLocalFile it will be file2 */
  if (G_IS_LOCAL_DAEMON_FILE (file2))
    wrapped2 = G_LOCAL_DAEMON_FILE (file2)->wrapped;
  else
    wrapped2 = file2;
  
  return g_file_equal (wrapped1, wrapped2);
}

static GFile *
g_local_daemon_file_resolve_relative (GFile *file,
				      const char *rel)
{
  GFile *child;

  child = g_file_resolve_relative (G_LOCAL_DAEMON_FILE (file)->wrapped, rel);
  if (child == NULL)
    return NULL;
  
  return g_local_daemon_file_new (child);
}

static GFileEnumerator *
g_local_daemon_file_enumerate_children (GFile *file,
					const char *attributes,
					GFileGetInfoFlags flags,
					GCancellable *cancellable,
					GError **error)
{
  return g_file_enumerate_children (G_LOCAL_DAEMON_FILE (file)->wrapped,
				    attributes, flags,
				    cancellable, error);
}

static GFileInfo *
g_local_daemon_file_get_info (GFile *file,
			      const char *attributes,
			      GFileGetInfoFlags flags,
			      GCancellable *cancellable,
			      GError **error)
{
  return g_file_get_info (G_LOCAL_DAEMON_FILE (file)->wrapped,
			  attributes, flags,
			  cancellable, error);
}

static GFileInputStream *
g_local_daemon_file_read (GFile *file,
			  GCancellable *cancellable,
			  GError **error)
{
  return g_file_read (G_LOCAL_DAEMON_FILE (file)->wrapped,
		      cancellable, error);
}

static GFileOutputStream *
g_local_daemon_file_append_to (GFile *file,
			       GCancellable *cancellable,
			       GError **error)
{
  return g_file_append_to (G_LOCAL_DAEMON_FILE (file)->wrapped, cancellable, error);
}

static GFileOutputStream *
g_local_daemon_file_create (GFile *file,
			    GCancellable *cancellable,
			    GError **error)
{
  return g_file_create (G_LOCAL_DAEMON_FILE (file)->wrapped, cancellable, error);
}

static GFileOutputStream *
g_local_daemon_file_replace (GFile *file,
			     time_t mtime,
			     gboolean  make_backup,
			     GCancellable *cancellable,
			     GError **error)
{
  return g_file_replace (G_LOCAL_DAEMON_FILE (file)->wrapped, mtime, make_backup, cancellable, error);
}

static void
g_local_daemon_file_mount (GFile *file,
			   GMountOperation *mount_op)
{
  return g_file_mount (G_LOCAL_DAEMON_FILE (file)->wrapped, mount_op);
}

static void
g_local_daemon_file_file_iface_init (GFileIface *iface)
{
  iface->dup = g_local_daemon_file_dup;
  iface->hash = g_local_daemon_file_hash;
  iface->equal = g_local_daemon_file_equal;
  iface->is_native = g_local_daemon_file_is_native;
  iface->get_path = g_local_daemon_file_get_path;
  iface->get_uri = g_local_daemon_file_get_uri;
  iface->get_parse_name = g_local_daemon_file_get_parse_name;
  iface->get_parent = g_local_daemon_file_get_parent;
  iface->resolve_relative = g_local_daemon_file_resolve_relative;
  iface->enumerate_children = g_local_daemon_file_enumerate_children;
  iface->get_info = g_local_daemon_file_get_info;
  iface->read = g_local_daemon_file_read;
  iface->append_to = g_local_daemon_file_append_to;
  iface->create = g_local_daemon_file_create;
  iface->replace = g_local_daemon_file_replace;
  iface->mount = g_local_daemon_file_mount;
}
