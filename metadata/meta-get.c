#include "config.h"
#include "metatree.h"
#include <glib/gstdio.h>

static char *treename = NULL;
static char *treefilename = NULL;
static gboolean recursive = FALSE;
static GOptionEntry entries[] =
{
  { "tree", 't', 0, G_OPTION_ARG_STRING, &treename, "Tree", NULL},
  { "file", 'f', 0, G_OPTION_ARG_STRING, &treefilename, "Tree file", NULL},
  { "recursive", 'r', 0, G_OPTION_ARG_NONE, &recursive, "Recursive", NULL},
  { NULL }
};

static gboolean
print_key (const char *key,
	   MetaKeyType type,
	   gpointer value,
	   gpointer user_data)
{
  int indent = GPOINTER_TO_INT (user_data);
  char **values;
  int i;

  g_assert (type != META_KEY_TYPE_NONE);

  if (type == META_KEY_TYPE_STRING)
    g_print ("%*s%s=%s\n", indent, "",key, (char *)value);
  else
    {
      values = value;
      g_print ("%*s%s=[", indent, "",key);
      for (i = 0; values[i] != NULL; i++)
	{
	  if (values[i+1] != NULL)
	    g_print ("%s,", values[i]);
	  else
	    g_print ("%s", values[i]);
	}
      g_print ("]\n");
    }
  return TRUE;
}

static gboolean
prepend_name (const char *entry,
	      guint64 last_changed,
	      gboolean has_children,
	      gboolean has_data,
	      gpointer user_data)
{
  GList **children = user_data;

  *children = g_list_prepend (*children,
			      g_strdup (entry));
  return TRUE;
}

static void
enum_keys (MetaTree *tree, char *path,
	   gboolean recurse, int indent)
{
  GList *children, *l;
  char *child_name, *child_path;

  g_print ("%*s%s\n", indent, "", path);
  meta_tree_enumerate_keys (tree, path,
			    print_key, GINT_TO_POINTER (indent+1));

  if (recurse)
    {
      children = NULL;
      meta_tree_enumerate_dir (tree, path,
			       prepend_name,
			       &children);
      for (l = children; l != NULL; l = l->next)
	{
	  child_name = l->data;
	  child_path = g_build_filename (path, l->data, NULL);
	  g_free (child_name);

	  enum_keys (tree, child_path, recurse, indent + 3);

	  g_free (child_path);
	}
      g_list_free (children);
    }
}

int
main (int argc,
      char *argv[])
{
  MetaTree *tree;
  GError *error = NULL;
  GOptionContext *context;
  MetaKeyType type;
  const char *path, *key;
  MetaLookupCache *lookup;
  struct stat statbuf;
  char *tree_path;
  char **strings;
  int i, j;

  context = g_option_context_new ("<path> [keys..]- read metadata");
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

  if (treefilename)
    {
      tree = meta_tree_open (treefilename, FALSE);
      if (tree)
	tree_path = g_strdup (path);

      if (tree == NULL)
	{
	  g_printerr ("can't open metadata file %s\n", treefilename);
	  return 1;
	}
    }
  else if (treename)
    {
      tree = meta_tree_lookup_by_name (treename, FALSE);
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
					    FALSE,
					    &tree_path);
      meta_lookup_cache_free (lookup);

      if (tree == NULL)
	{
	  g_printerr ("can't open metadata tree for file %s\n", path);
	  return 1;
	}
    }

  if (argc > 2)
    {
      for (i = 2; i < argc; i++)
	{
	  key = argv[i];
	  type = meta_tree_lookup_key_type  (tree, tree_path, key);
	  if (type == META_KEY_TYPE_NONE)
	    g_print ("%s Not set\n", key);
	  else if (type == META_KEY_TYPE_STRING)
	    g_print ("%s=%s\n", key, meta_tree_lookup_string (tree, path, key));
	  else
	    {
	      strings = meta_tree_lookup_stringv (tree, path, key);
	      g_print ("%s=[", key);
	      for (j = 0; strings[j] != NULL; j++)
		{
		  if (strings[j+1] == NULL)
		    g_print ("%s", strings[j]);
		  else
		    g_print ("%s,", strings[j]);
		}
	      g_print ("]\n");
	    }
	}
    }
  else
    {
      enum_keys (tree, tree_path, recursive, 0);
    }

  return 0;
}
