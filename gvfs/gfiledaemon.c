#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gfiledaemon.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gfileinputstreamdaemon.h>
#include <glib/gi18n-lib.h>

static void g_file_daemon_file_iface_init (GFileIface       *iface);

struct _GFileDaemon
{
  GObject parent_instance;

  char *filename;
  char *mountpoint;
};

G_DEFINE_TYPE_WITH_CODE (GFileDaemon, g_file_daemon, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_file_daemon_file_iface_init))

static void
g_file_daemon_finalize (GObject *object)
{
  GFileDaemon *daemon_file;

  daemon_file = G_FILE_DAEMON (object);

  g_free (daemon_file->filename);
  g_free (daemon_file->mountpoint);
  
  if (G_OBJECT_CLASS (g_file_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_daemon_parent_class)->finalize) (object);
}

static void
g_file_daemon_class_init (GFileDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_daemon_finalize;
}

static void
g_file_daemon_init (GFileDaemon *daemon_file)
{
}

GFile *
g_file_daemon_new (const char *filename,
		 const char *mountpoint)
{
  GFileDaemon *daemon_file;
  int len;

  daemon_file = g_object_new (G_TYPE_FILE_DAEMON, NULL);
  daemon_file->filename = g_strdup (filename);
  daemon_file->mountpoint = g_strdup (mountpoint);

  /* Remove any trailing slashes */
  len = strlen (daemon_file->filename);

  while (len > 1 && daemon_file->filename[len-1] == '/')
    {
      daemon_file->filename[len-1] = 0;
      len--;
    }
  
  return G_FILE (daemon_file);
}

static gboolean
g_file_daemon_is_native (GFile *file)
{
  return FALSE;
}

static char *
g_file_daemon_get_path (GFile *file)
{
  return NULL;
}

static char *
g_file_daemon_get_uri (GFile *file)
{
  /* TODO: implement to-uri */
  return NULL;
}

static char *
g_file_daemon_get_parse_name (GFile *file)
{
  /* TODO: implement to-iri */
  return NULL;
}

static GFile *
g_file_daemon_get_parent (GFile *file)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  const char *file_name;
  GFileDaemon *parent;
  const char *base;
  char *parent_file_name;
  gsize len;    

  file_name = daemon_file->filename;
  base = strrchr (file_name, '/');
  if (base == NULL || base == file_name)
    return NULL;

  while (base > file_name && *base == '/')
    base--;

  len = (guint) 1 + base - file_name;
  
  parent_file_name = g_new (gchar, len + 1);
  g_memmove (parent_file_name, file_name, len);
  parent_file_name[len] = 0;

  parent = g_object_new (G_TYPE_FILE_DAEMON, NULL);
  parent->filename = parent_file_name;
  parent->mountpoint = g_strdup (daemon_file->mountpoint);
  
  return G_FILE (parent);
}

static GFile *
g_file_daemon_copy (GFile *file)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);

  return g_file_daemon_new (daemon_file->filename,
			  daemon_file->mountpoint);
}


static GFile *
g_file_daemon_get_child (GFile *file,
		       const char *name)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  char *filename;
  GFile *child;

  filename = g_build_filename (daemon_file->filename, name, NULL);

  child = g_file_daemon_new (filename, daemon_file->mountpoint);
  g_free (filename);
  
  return child;
}

static GFileEnumerator *
g_file_daemon_enumerate_children (GFile      *file,
				GFileInfoRequestFlags requested,
				const char *attributes,
				gboolean follow_symlinks)
{
  /* TODO: implement */
  return NULL;
}

static GFileInfo *
g_file_daemon_get_info (GFile                *file,
		      GFileInfoRequestFlags requested,
		      const char           *attributes,
		      gboolean              follow_symlinks,
		      GError              **error)
{
  /* TODO: implement */
  return NULL;
}

static GFileInputStream *
g_file_daemon_read (GFile *file)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  GFileInputStream *stream;

  stream = g_file_input_stream_daemon_new (daemon_file->filename,
					   daemon_file->mountpoint);
  return stream;
}

static GFileOutputStream *
g_file_daemon_append_to (GFile *file)
{
  /* TODO: implement */
  return NULL;
}

static GFileOutputStream *
g_file_daemon_create (GFile *file)
{
  /* TODO: implement */
  return NULL;
}

static GFileOutputStream *
g_file_daemon_replace (GFile *file,
		     time_t mtime,
		     gboolean  make_backup)
{
  /* TODO: implement */
  return NULL;
}

static void
g_file_daemon_file_iface_init (GFileIface *iface)
{
  iface->copy = g_file_daemon_copy;
  iface->is_native = g_file_daemon_is_native;
  iface->get_path = g_file_daemon_get_path;
  iface->get_uri = g_file_daemon_get_uri;
  iface->get_parse_name = g_file_daemon_get_parse_name;
  iface->get_parent = g_file_daemon_get_parent;
  iface->get_child = g_file_daemon_get_child;
  iface->enumerate_children = g_file_daemon_enumerate_children;
  iface->get_info = g_file_daemon_get_info;
  iface->read = g_file_daemon_read;
  iface->append_to = g_file_daemon_append_to;
  iface->create = g_file_daemon_create;
  iface->replace = g_file_daemon_replace;
}
