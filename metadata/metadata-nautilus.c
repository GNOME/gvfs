#include <string.h>
#include "metabuilder.h"
#include <libxml/tree.h>

static xmlNodePtr
xml_get_children (xmlNodePtr parent)
{
	if (parent == NULL) {
		return NULL;
	}
	return parent->children;
}

static xmlNodePtr
xml_get_root_children (xmlDocPtr document)
{
	return xml_get_children (xmlDocGetRootElement (document));
}


static char *
get_uri_from_nautilus_metafile_name (const char *filename)
{
  GString *s;
  char c;
  char *base_name, *p;
  int len;

  base_name = g_path_get_basename (filename);
  len = strlen (base_name);
  if (len <=  4 ||
      strcmp (base_name + len - 4, ".xml") != 0)
    {
      g_free (base_name);
      return NULL;
    }
  base_name[len-4] = 0;

  s = g_string_new (NULL);

  p = base_name;
  while (*p)
    {
      c = *p++;
      if (c == '%')
	{
	  c =
	    g_ascii_xdigit_value (p[0]) << 4 |
	    g_ascii_xdigit_value (p[1]);
	  p += 2;
	}
      g_string_append_c (s, c);
    }
  g_free (base_name);

  return g_string_free (s, FALSE);
}

static void
parse_xml_node (MetaFile *metafile,
		xmlNodePtr filenode)
{
  xmlChar *data;
  guint64 timestamp;
  xmlNodePtr node;
  xmlAttrPtr attr;
  xmlChar *property;
  char *combined_key;

  data = xmlGetProp (filenode, (xmlChar *)"timestamp");
  if (data)
    {
      timestamp = g_ascii_strtoll ((char *)data, NULL, 10);
      if (timestamp != 0)
	metafile->last_changed = timestamp;
    }

  for (attr = filenode->properties; attr != NULL; attr = attr->next)
    {
      if (strcmp ((char *)attr->name, "name") == 0 ||
	  strcmp ((char *)attr->name, "timestamp") == 0)
	continue;

      property = xmlGetProp (filenode, attr->name);
      if (property)
	metafile_key_set_value (metafile, (char *)attr->name, (char *)property);
      xmlFree (property);
    }

  for (node = filenode->children; node != NULL; node = node->next)
    {
      for (attr = node->properties; attr != NULL; attr = attr->next)
	{
	  property = xmlGetProp (node, attr->name);
	  if (property)
	    {
	      combined_key = g_strconcat ((char *)node->name,
					  "-",
					  (char *)attr->name,
					  NULL);
	      metafile_key_list_add (metafile, combined_key, (char *)property);
	      g_free (combined_key);
	    }
	  xmlFree (property);
	}
    }
}

static void
parse_xml_file (MetaBuilder *builder,
		xmlDocPtr xml,
		char *dir)
{
  xmlNodePtr node;
  xmlChar *name;
  char *unescaped_name;
  MetaFile *dir_metafile, *metafile;

  dir_metafile =  meta_builder_lookup (builder, dir, TRUE);

  for (node = xml_get_root_children (xml);
       node != NULL; node = node->next)
    {
      if (strcmp ((char *)node->name, "file") == 0)
	{
	  name = xmlGetProp (node, (xmlChar *)"name");
	  unescaped_name = g_uri_unescape_string ((char *)name, "/");
	  xmlFree (name);

	  if (strcmp (unescaped_name, ".") == 0)
	    metafile = dir_metafile;
	  else
	    metafile = metafile_lookup_child (dir_metafile, unescaped_name, TRUE);

	  parse_xml_node (metafile, node);
	  g_free (unescaped_name);
	}
    }
}

static void
parse_nautilus_file (MetaBuilder *builder,
		     char *file)
{
  char *uri;
  char *dir;
  gchar *contents;
  gsize length;
  xmlDocPtr xml;

  if (!g_file_get_contents (file, &contents, &length, NULL))
    {
      g_print ("failed to load %s\n", file);
      return;
    }

  uri = get_uri_from_nautilus_metafile_name (file);
  if (uri == NULL)
    {
      g_free (contents);
      return;
    }

  dir = g_filename_from_uri (uri, NULL, NULL);
  g_free (uri);
  if (dir == NULL)
    {
      g_free (contents);
      return;
    }

  xml = xmlParseMemory (contents, length);
  g_free (contents);
  if (xml == NULL)
    return;

  parse_xml_file (builder, xml, dir);
  xmlFreeDoc (xml);
}

/*static gboolean recursive = FALSE;*/
static char *filename = NULL;
static GOptionEntry entries[] =
{
  { "out", 'o', 0, G_OPTION_ARG_FILENAME, &filename, "Output filename", NULL },
  { NULL }
};

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  MetaBuilder *builder;
  GError *error = NULL;
  int i;

  context = g_option_context_new ("<nautilus metadata files> - convert nautilus metadata");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("option parsing failed: %s\n", error->message);
      return 1;
    }

  if (argc < 2)
    {
      g_print ("No files specified\n");
      return 1;
    }

  builder = meta_builder_new ();
  for (i = 1; i < argc; i++)
    parse_nautilus_file (builder, argv[i]);
  if (filename)
    meta_builder_write (builder, filename);
  else
    meta_builder_print (builder);

  return 0;
}
