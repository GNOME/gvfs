#include <config.h>

#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "mount.h"
#include "gmountoperationdbus.h"

struct _Mountable {
  char *type;
  char *exec;
  gboolean automount;
}; 

static GList *mountables;

void
mount_init (void)
{
  GDir *dir;
  char *mount_dir, *path;
  const char *filename;
  GKeyFile *keyfile;
  char *type, *exec;
  Mountable *mountable;
  gboolean automount;
  
  mount_dir = MOUNTABLE_DIR;
  dir = g_dir_open (mount_dir, 0, NULL);

  if (dir)
    {
      while ((filename = g_dir_read_name (dir)) != NULL)
	{
	  path = g_build_filename (mount_dir, filename, NULL);
	  
	  keyfile = g_key_file_new ();
	  if (g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, NULL))
	    {
	      type = g_key_file_get_string (keyfile, "Mount", "Type", NULL);
	      exec = g_key_file_get_string (keyfile, "Mount", "Exec", NULL);
	      automount = g_key_file_get_boolean (keyfile, "Mount", "AutoMount", NULL);
	      if (type != NULL && exec != NULL)
		{
		  mountable = g_new0 (Mountable, 1);
		  mountable->type = g_strdup (type);
		  mountable->exec = g_strdup (exec);
		  mountable->automount = automount;
		  
		  mountables = g_list_prepend (mountables, mountable);
		}
	      g_free (type);
	      g_free (exec);
	    }
	  g_key_file_free (keyfile);
	  g_free (path);
	}
    }
}

gboolean
mountable_is_automount (Mountable *mountable)
{
  return mountable->automount;
}

static Mountable *
find_mountable (const char *type)
{
  GList *l;

  for (l = mountables; l != NULL; l = l->next)
    {
      Mountable *mountable = l->data;

      if (strcmp (mountable->type, type) == 0)
	return mountable;
    }
  
  return NULL;
}

Mountable *
lookup_mountable (GMountSpec *spec)
{
  const char *type;
  
  type = g_mount_spec_get_type (spec);
  if (type == NULL)
    return NULL;

  return find_mountable (type);
}

GMountOperation *
mountable_mount (Mountable *mountable,
		 GMountSpec *spec,
		 GError **error)
{
  DBusConnection *conn;
  const char *id;
  char *exec;
  gboolean res;
  GMountOperationDBus *op;

  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  id = dbus_bus_get_unique_name (conn);
  dbus_connection_unref (conn);

  op = g_mount_operation_dbus_new (spec);
  
  exec = g_strconcat (mountable->exec, " ", id, " ", op->obj_path, NULL);
  
  res = g_spawn_command_line_async (exec, error);
  g_free (exec);
  
  if (!res)
    {
      g_object_unref (op);
      return NULL;
    }
  
  return G_MOUNT_OPERATION (op);
}
