#include <config.h>
#include <string.h>
#include "gvfsdaemon.h"
#include "gvfsuriutils.h"
#include "gfiledaemon.h"
#include "gfiledaemonlocal.h"
#include "gvfslocal.h"

static void g_vfs_daemon_class_init     (GVfsDaemonClass *class);
static void g_vfs_daemon_vfs_iface_init (GVfsIface       *iface);
static void g_vfs_daemon_finalize       (GObject         *object);

struct _GVfsDaemon
{
  GObject parent;

  GVfs *wrapped_vfs;
};

G_DEFINE_TYPE_WITH_CODE (GVfsDaemon, g_vfs_daemon, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VFS,
						g_vfs_daemon_vfs_iface_init))
 
static void
g_vfs_daemon_class_init (GVfsDaemonClass *class)
{
  GObjectClass *object_class;
  
  object_class = (GObjectClass *) class;

  object_class->finalize = g_vfs_daemon_finalize;
}

static void
g_vfs_daemon_finalize (GObject *object)
{
  /* must chain up */
  G_OBJECT_CLASS (g_vfs_daemon_parent_class)->finalize (object);
}

static void
g_vfs_daemon_init (GVfsDaemon *vfs)
{

  vfs->wrapped_vfs = g_vfs_local_new ();
}

GVfsDaemon *
g_vfs_daemon_new (void)
{
  return g_object_new (G_TYPE_VFS_DAEMON, NULL);
}

static GFile *
g_vfs_daemon_get_file_for_path (GVfs       *vfs,
				const char *path)
{
  GFile *file;

  /* TODO: detect fuse paths and convert to daemon vfs GFiles */
  
  file = g_vfs_get_file_for_path (G_VFS_DAEMON (vfs)->wrapped_vfs, path);
  
  return g_file_daemon_local_new (file);
}

static GFile *
g_vfs_daemon_get_file_for_uri (GVfs       *vfs,
			       const char *uri)
{
  char *base;
  GFile *file, *wrapped;
  GDecodedUri *decoded;
  
  decoded = _g_decode_uri (uri);
  if (decoded == NULL)
    return NULL;

  if (strcmp (decoded->scheme, "file") == 0)
    {
      wrapped = g_vfs_daemon_get_file_for_path  (vfs, decoded->path);
      file = g_file_daemon_local_new (wrapped);
    }
  else
    {
      base = _g_encode_uri (decoded, TRUE);
      file = g_file_daemon_new (decoded->path, base);
      g_free (base);
    }

  _g_decoded_uri_free (decoded);
  
  return file;
}

static GFile *
g_vfs_daemon_parse_name (GVfs       *vfs,
			 const char *parse_name)
{
  GFile *file;
  char *path;
  
  if (g_path_is_absolute (parse_name))
    {
      path = g_filename_from_utf8 (parse_name, -1, NULL, NULL, NULL);
      file = g_vfs_daemon_get_file_for_path  (vfs, path);
      g_free (path);
    }
  else
    {
      file = g_vfs_daemon_get_file_for_uri (vfs, parse_name);
    }

  return file;
}

static void
g_vfs_daemon_vfs_iface_init (GVfsIface *iface)
{
  iface->get_file_for_path = g_vfs_daemon_get_file_for_path;
  iface->get_file_for_uri = g_vfs_daemon_get_file_for_uri;
  iface->parse_name = g_vfs_daemon_parse_name;
}
