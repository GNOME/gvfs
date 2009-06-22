#include "config.h"
#include "metatree.h"

/*static gboolean recursive = FALSE;*/
static gboolean verbose = FALSE;
static GOptionEntry entries[] =
{
  { "verbose", 'l', 0, G_OPTION_ARG_NONE, &verbose, "Verbose", NULL },
  /*  { "recursive", 'r', 0, G_OPTION_ARG_NONE, &recursive, "Recursively list", NULL }, */
  { NULL }
};

static gboolean
print_dir (const char *name,
	   guint64 last_changed,
	   gboolean has_children,
	   gboolean has_data,
	   gpointer user_data)
{
  if (verbose)
    g_print ("%-16s %s%s  %"G_GUINT64_FORMAT"\n",
	     name,
	     has_children?"c":" ",
	     has_data?"d":" ",
	     last_changed);
  else
    g_print ("%s\n", name);
  return TRUE;
}

static void
dir (MetaTree *tree,
     const char *path)
{
  meta_tree_enumerate_dir (tree, path,
			   print_dir, NULL);
}

int
main (int argc,
      char *argv[])
{
  MetaTree *tree;
  GError *error = NULL;
  GOptionContext *context;
  int i;

  context = g_option_context_new ("<tree file> <dir in tree> - list entries");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("option parsing failed: %s\n", error->message);
      return 1;
    }

  if (argc < 3)
    {
      if (argc < 2)
	g_printerr ("No metadata tree specified\n");
      else
	g_printerr ("no dir specified\n");
      return 1;
    }

  tree = meta_tree_open (argv[1], TRUE);
  if (tree == NULL)
    {
      g_printerr ("can't open metadata tree %s\n", argv[1]);
      return 1;
    }

  for (i = 2; i < argc; i++)
    {
      if (argc > 3)
	g_print ("%s:\n", argv[i]);

      dir (tree, argv[i]);
    }

  return 0;
}
