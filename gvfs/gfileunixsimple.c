#include <config.h>

#include <string.h>

#include "gfileunixsimple.h"
#include "glocalfileinputstream.h"
#include "glocalfileoutputstream.h"
#include <glib/gi18n-lib.h>

static void g_file_unix_simple_file_iface_init (GFileIface       *iface);

struct _GFileUnixSimple
{
  GObject parent_instance;

  GFile *wrapped;
};

G_DEFINE_TYPE_WITH_CODE (GFileUnixSimple, g_file_unix_simple, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_file_unix_simple_file_iface_init))

static void
g_file_unix_simple_finalize (GObject *object)
{
  GFileUnixSimple *unix_simple;

  unix_simple = G_FILE_UNIX_SIMPLE (object);

  g_object_unref (unix_simple->wrapped);
  
  if (G_OBJECT_CLASS (g_file_unix_simple_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_unix_simple_parent_class)->finalize) (object);
}

static void
g_file_unix_simple_class_init (GFileUnixSimpleClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_unix_simple_finalize;
}

static void
g_file_unix_simple_init (GFileUnixSimple *unix_simple)
{
}

GFile *
g_file_unix_simple_new (GFile *wrapped)
{
  GFileUnixSimple *unix_simple = g_object_new (G_TYPE_FILE_UNIX_SIMPLE, NULL);
  unix_simple->wrapped = wrapped;
  return G_FILE (unix_simple);
}

static gboolean
g_file_unix_simple_is_native (GFile *file)
{
  return TRUE;
}

static char *
g_file_unix_simple_get_path (GFile *file)
{
  return g_file_get_path (G_FILE_UNIX_SIMPLE (file)->wrapped);
}

static char *
g_file_unix_simple_get_uri (GFile *file)
{
  return g_file_get_uri (G_FILE_UNIX_SIMPLE (file)->wrapped);
}

static char *
g_file_unix_simple_get_parse_name (GFile *file)
{
  return g_file_get_parse_name (G_FILE_UNIX_SIMPLE (file)->wrapped);
}

static GFile *
g_file_unix_simple_get_parent (GFile *file)
{
  GFile *parent;

  parent = g_file_get_parent (G_FILE_UNIX_SIMPLE (file)->wrapped);
  if (parent == NULL)
    return NULL;
  return g_file_unix_simple_new (parent);
}

static GFile *
g_file_unix_simple_copy (GFile *file)
{
  GFile *copy;

  copy = g_file_copy (G_FILE_UNIX_SIMPLE (file)->wrapped);
  return g_file_unix_simple_new (copy);
}


static GFile *
g_file_unix_simple_get_child (GFile *file,
			 const char *name)
{
  GFile *child;

  child = g_file_get_child (G_FILE_UNIX_SIMPLE (file)->wrapped, name);
  if (child == NULL)
    return NULL;
  
  return g_file_unix_simple_new (child);
}

static GFileEnumerator *
g_file_unix_simple_enumerate_children (GFile      *file,
				       GFileInfoRequestFlags requested,
				       const char *attributes,
				       gboolean follow_symlinks)
{
  return g_file_enumerate_children (G_FILE_UNIX_SIMPLE (file)->wrapped,
				    requested, attributes, follow_symlinks);
}

static GFileInfo *
g_file_unix_simple_get_info (GFile                *file,
			     GFileInfoRequestFlags requested,
			     const char           *attributes,
			     gboolean              follow_symlinks,
			     GError              **error)
{
  return g_file_get_info (G_FILE_UNIX_SIMPLE (file)->wrapped,
			  requested, attributes, follow_symlinks,
			  error);
}

static GFileInputStream *
g_file_unix_simple_read (GFile *file)
{
  return g_file_read (G_FILE_UNIX_SIMPLE (file)->wrapped);
}

static GFileOutputStream *
g_file_unix_simple_append_to (GFile *file)
{
  return g_file_append_to (G_FILE_UNIX_SIMPLE (file)->wrapped);
}

static GFileOutputStream *
g_file_unix_simple_create (GFile *file)
{
  return g_file_create (G_FILE_UNIX_SIMPLE (file)->wrapped);
}

static GFileOutputStream *
g_file_unix_simple_replace (GFile *file,
			    time_t mtime,
			    gboolean  make_backup)
{
  return g_file_replace (G_FILE_UNIX_SIMPLE (file)->wrapped, mtime, make_backup);
}

static void
g_file_unix_simple_file_iface_init (GFileIface *iface)
{
  iface->copy = g_file_unix_simple_copy;
  iface->is_native = g_file_unix_simple_is_native;
  iface->get_path = g_file_unix_simple_get_path;
  iface->get_uri = g_file_unix_simple_get_uri;
  iface->get_parse_name = g_file_unix_simple_get_parse_name;
  iface->get_parent = g_file_unix_simple_get_parent;
  iface->get_child = g_file_unix_simple_get_child;
  iface->enumerate_children = g_file_unix_simple_enumerate_children;
  iface->get_info = g_file_unix_simple_get_info;
  iface->read = g_file_unix_simple_read;
  iface->append_to = g_file_unix_simple_append_to;
  iface->create = g_file_unix_simple_create;
  iface->replace = g_file_unix_simple_replace;
}
