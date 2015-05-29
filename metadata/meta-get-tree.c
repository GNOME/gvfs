#include "config.h"
#include "metatree.h"
#include <glib/gstdio.h>

/*static gboolean recursive = FALSE;*/
static gboolean verbose = FALSE;
static gboolean do_pause = FALSE;
static GOptionEntry entries[] =
{
  { "verbose", 'l', 0, G_OPTION_ARG_NONE, &verbose, "Verbose", NULL },
  { "pause", 'p', 0, G_OPTION_ARG_NONE, &do_pause, "Pause", NULL },
  { NULL }
};

int
main (int argc,
      char *argv[])
{
  GError *error = NULL;
  GOptionContext *context;
  MetaLookupCache *cache;
  MetaTree *tree;
  char *tree_path;
  struct stat statbuf;
  int i;

  context = g_option_context_new ("<tree file> <dir in tree> - list entries");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("option parsing failed: %s\n", error->message);
      return 1;
    }


  cache = meta_lookup_cache_new ();

  for (i = 1; i < argc; i++)
    {
      if (g_lstat (argv[i], &statbuf) != 0)
	{
	  g_printerr ("can't stat %s\n", argv[i]);
	  return 1;
	}

      tree_path = NULL;
      tree = meta_lookup_cache_lookup_path (cache, argv[i], statbuf.st_dev,
					    FALSE, &tree_path);
      if (tree)
	g_print ("tree: %s (exists: %d), tree path: %s\n", meta_tree_get_filename (tree), meta_tree_exists (tree), tree_path);
      else
	g_print ("tree lookup failed\n");

      if (do_pause)
	{
	  char buffer[1000];
	  g_print ("Pausing, press enter\n");
	  fgets(buffer, sizeof(buffer), stdin);
	}
    }

  return 0;
}
