#include "config.h"
#include "metatree.h"
#include <glib/gstdio.h>
#include "gvfsdaemonprotocol.h"
#include "metadata-dbus.h"

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
  const char *metatreefile;
  char *tree_path;
  GVfsMetadata *proxy;
  
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

  proxy = NULL;
  if (use_dbus)
    {
      proxy = gvfs_metadata_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                    G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                    G_VFS_DBUS_METADATA_NAME,
                                                    G_VFS_DBUS_METADATA_PATH,
                                                    NULL,
                                                    &error);
      
      if (proxy == NULL)
	{
	  g_printerr ("Unable to connect to dbus: %s (%s, %d)\n",
                      error->message, g_quark_to_string (error->domain), error->code);
	  g_error_free (error);
	  return 1;
	}
      
      g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), 1000*30);
    }

  if (unset)
    {
      if (use_dbus)
	{
          GVariantBuilder *builder;
          unsigned char c = 0;

          metatreefile = meta_tree_get_filename (tree);

          builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);

          /* Byte => unset */
          g_variant_builder_add (builder, "{sv}", key, g_variant_new_byte (c));

          if (! gvfs_metadata_call_set_sync (proxy,
                                             metatreefile,
                                             tree_path,
                                             g_variant_builder_end (builder),
                                             NULL,
                                             &error))
	    {
	      g_printerr ("Unset error: %s (%s, %d)\n",
                           error->message, g_quark_to_string (error->domain), error->code);
	      return 1;
	    }

          g_variant_builder_unref (builder);
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
	  GVariantBuilder *builder;

          metatreefile = meta_tree_get_filename (tree);
          strv = &argv[3];

          builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
          g_variant_builder_add (builder, "{sv}", key, g_variant_new_strv ((const gchar * const  *) strv, -1));

          if (! gvfs_metadata_call_set_sync (proxy,
                                             metatreefile,
                                             tree_path,
                                             g_variant_builder_end (builder),
                                             NULL,
                                             &error))
            {
              g_printerr ("SetStringv error: %s (%s, %d)\n",
                           error->message, g_quark_to_string (error->domain), error->code);
              return 1;
            }
          
          g_variant_builder_unref (builder);
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
          GVariantBuilder *builder;

          metatreefile = meta_tree_get_filename (tree);
          
          builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
          g_variant_builder_add (builder, "{sv}", key, g_variant_new_string (argv[3]));

          if (! gvfs_metadata_call_set_sync (proxy,
                                             metatreefile,
                                             tree_path,
                                             g_variant_builder_end (builder),
                                             NULL,
                                             &error))
            {
              g_printerr ("SetString error: %s (%s, %d)\n",
                           error->message, g_quark_to_string (error->domain), error->code);
              return 1;
            }
          
          g_variant_builder_unref (builder);
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

  if (proxy)
    g_object_unref (proxy);
    
  return 0;
}
