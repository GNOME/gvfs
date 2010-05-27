#include "config.h"
#include "metatree.h"
#include <glib/gstdio.h>
#include <dbus/dbus.h>
#include "gvfsdaemonprotocol.h"
#include "gvfsdbusutils.h"

static gboolean unset = FALSE;
static gboolean list = FALSE;
static gboolean use_dbus = FALSE;
static char *treename = NULL;
static GOptionEntry entries[] =
{
  { "dbus", 'd', 0, G_OPTION_ARG_NONE, &use_dbus, "Use dbus", NULL},
  { "tree", 't', 0, G_OPTION_ARG_STRING, &treename, "Tree", NULL},
  { "unset", 'u', 0, G_OPTION_ARG_NONE, &unset, "Unset", NULL },
  { "list", 'l', 0, G_OPTION_ARG_NONE, &list, "Set as list", NULL },
  { NULL }
};

int
main (int argc,
      char *argv[])
{
  MetaTree *tree;
  GError *error = NULL;
  GOptionContext *context;
  MetaLookupCache *lookup;
  struct stat statbuf;
  const char *path, *key;
  DBusConnection *connection;
  DBusMessage *message, *reply;
  const char *metatreefile;
  DBusError derror;
  char *tree_path;

  context = g_option_context_new ("<path> <key> <value> - set metadata");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("option parsing failed: %s\n", error->message);
      return 1;
    }

  if (argc < 2)
    {
      g_printerr ("no path specified\n");
      return 1;
    }
  path = argv[1];

  if (argc < 3)
    {
      g_printerr ("no key specified\n");
      return 1;
    }
  key = argv[2];

  if (!list && !unset && argc != 4)
    {
      g_print ("No value specified\n");
      return 1;
    }

  if (treename)
    {
      tree = meta_tree_lookup_by_name (treename, TRUE);
      if (tree)
	tree_path = g_strdup (path);

      if (tree == NULL)
	{
	  g_printerr ("can't open metadata tree %s\n", path);
	  return 1;
	}
    }
  else
    {
      lookup = meta_lookup_cache_new ();
      if (g_lstat (path, &statbuf) != 0)
	{
	  g_printerr ("can't find file %s\n", path);
	  return 1;
	}
      tree = meta_lookup_cache_lookup_path (lookup,
					    path,
					    statbuf.st_dev,
					    TRUE,
					    &tree_path);
      meta_lookup_cache_free (lookup);

      if (tree == NULL)
	{
	  g_printerr ("can't open metadata tree for file %s\n", path);
	  return 1;
	}
    }

  connection = NULL;
  if (use_dbus)
    {
      dbus_error_init (&derror);
      connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
      if (connection == NULL)
	{
	  g_printerr ("Unable to connect to dbus: %s\n", derror.message);
	  dbus_error_free (&derror);
	  return 1;
	}
    }

  if (unset)
    {
      if (use_dbus)
	{
	  message =
	    dbus_message_new_method_call (G_VFS_DBUS_METADATA_NAME,
					  G_VFS_DBUS_METADATA_PATH,
					  G_VFS_DBUS_METADATA_INTERFACE,
					  G_VFS_DBUS_METADATA_OP_UNSET);
	  metatreefile = meta_tree_get_filename (tree);
	  _g_dbus_message_append_args (message,
				       G_DBUS_TYPE_CSTRING, &metatreefile,
				       G_DBUS_TYPE_CSTRING, &tree_path,
				       DBUS_TYPE_STRING, &key,
				       0);
	  reply = dbus_connection_send_with_reply_and_block (connection, message, 1000*30,
							     &derror);
	  if (reply == NULL)
	    {
	      g_printerr ("Unset error: %s\n", derror.message);
	      return 1;
	    }
	}
      else
	{
	  if (!meta_tree_unset (tree, tree_path, key))
	    {
	      g_printerr ("Unable to unset key\n");
	      return 1;
	    }
	}
    }
  else if (list)
    {
      if (use_dbus)
	{
	  char **strv;
	  message =
	    dbus_message_new_method_call (G_VFS_DBUS_METADATA_NAME,
					  G_VFS_DBUS_METADATA_PATH,
					  G_VFS_DBUS_METADATA_INTERFACE,
					  G_VFS_DBUS_METADATA_OP_SET);
	  metatreefile = meta_tree_get_filename (tree);
	  strv = &argv[3];
	  _g_dbus_message_append_args (message,
				       G_DBUS_TYPE_CSTRING, &metatreefile,
				       G_DBUS_TYPE_CSTRING, &tree_path,
				       DBUS_TYPE_STRING, &key,
				       DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &strv, argc - 3,
				       0);
	  reply = dbus_connection_send_with_reply_and_block (connection, message, 1000*30,
							     &derror);
	  if (reply == NULL)
	    {
	      g_printerr ("SetStringv error: %s\n", derror.message);
	      return 1;
	    }
	}
      else
	{
	  if (!meta_tree_set_stringv (tree, tree_path, key, &argv[3]))
	    {
	      g_printerr ("Unable to set key\n");
	      return 1;
	    }
	}
    }
  else
    {
      if (use_dbus)
	{
	  message =
	    dbus_message_new_method_call (G_VFS_DBUS_METADATA_NAME,
					  G_VFS_DBUS_METADATA_PATH,
					  G_VFS_DBUS_METADATA_INTERFACE,
					  G_VFS_DBUS_METADATA_OP_SET);
	  metatreefile = meta_tree_get_filename (tree);
	  _g_dbus_message_append_args (message,
				       G_DBUS_TYPE_CSTRING, &metatreefile,
				       G_DBUS_TYPE_CSTRING, &tree_path,
				       DBUS_TYPE_STRING, &key,
				       DBUS_TYPE_STRING, &argv[3],
				       0);
	  reply = dbus_connection_send_with_reply_and_block (connection, message, 1000*30,
							     &derror);
	  if (reply == NULL)
	    {
	      g_printerr ("SetString error: %s\n", derror.message);
	      return 1;
	    }
	}
      else
	{
	  if (!meta_tree_set_string (tree, tree_path, key, argv[3]))
	    {
	      g_printerr ("Unable to set key\n");
	      return 1;
	    }
	}
    }

  return 0;
}
