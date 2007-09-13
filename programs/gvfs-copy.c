#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>
#include <string.h>

#include <glib.h>
#include <gio/gfile.h>

static gboolean progress = FALSE;
static gboolean interactive = FALSE;
static gboolean backup = FALSE;
static gboolean no_target_directory = FALSE;

static GOptionEntry entries[] = 
{
	{ "no-target-directory", 'T', 0, G_OPTION_ARG_NONE, &no_target_directory, "no target directory", NULL },
	{ "progress", 'p', 0, G_OPTION_ARG_NONE, &progress, "show progress", NULL },
	{ "interactive", 'i', 0, G_OPTION_ARG_NONE, &interactive, "prompt before overwrite", NULL },
	{ "backup", 'b', 0, G_OPTION_ARG_NONE, &backup, "backup existing destination files", NULL },
	{ NULL }
};

static gboolean
is_dir (GFile *file)
{
  GFileInfo *info;
  gboolean res;
  
  info = g_file_get_info (file, G_FILE_ATTRIBUTE_STD_TYPE, 0, NULL, NULL);
  res = info && g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
  if (info)
    g_object_unref (info);
  return res;
}

static void
show_progress (goffset current_num_bytes,
	       goffset total_num_bytes,
	       gpointer user_data)
{
  g_print ("progress %"G_GUINT64_FORMAT"/%"G_GUINT64_FORMAT"\n",
	   current_num_bytes, total_num_bytes);
}


int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  GFile *source, *dest, *target;
  gboolean dest_is_dir;
  char *basename;
  int i;
  GFileCopyFlags flags;
  
  setlocale (LC_ALL, "");

  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- output files at <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);

  if (argc <= 2)
    {
      g_printerr ("Missing operand\n");
      return 1;
    }

  dest = g_file_get_for_commandline_arg (argv[argc-1]);

  if (no_target_directory && argc > 3)
    {
      g_printerr ("Too many arguments\n");
      g_object_unref (dest);
      return 1;
    }
  
  dest_is_dir = is_dir (dest);

  if (!dest_is_dir && argc > 3)
    {
      g_printerr ("Target %s is not a directory\n", argv[argc-1]);
      g_object_unref (dest);
      return 1;
    }

  for (i = 1; i < argc - 1; i++)
    {
      source = g_file_get_for_commandline_arg (argv[i]);

      if (dest_is_dir && !no_target_directory)
	{
	  basename = g_file_get_basename (source);
	  target = g_file_get_child (dest, basename);
	  g_free (basename);
	}
      else
	target = g_object_ref (dest);

      flags = 0;
      if (backup)
	flags |= G_FILE_COPY_BACKUP;
      if (!interactive)
	flags |= G_FILE_COPY_OVERWRITE;
	
      error = NULL;
      if (!g_file_copy (source, target, flags, NULL, progress?show_progress:NULL, NULL, &error))
	{
	  if (interactive && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
	    {
	      char line[16];
	      
	      g_error_free (error);
	      error = NULL;

	      basename = g_file_get_basename (target);
	      g_print ("overwrite %s?", basename);
	      g_free (basename);

	      if (fgets(line, sizeof (line), stdin) &&
		  line[0] == 'y')
		{
		  flags |= G_FILE_COPY_OVERWRITE;
		  if (!g_file_copy (source, target, flags, NULL, NULL, NULL, &error))
		    goto copy_failed;
		}
	    }
	  else
	    {
	    copy_failed:
	      g_printerr ("Error copying file %s: %s\n", argv[i], error->message);
	      g_error_free (error);
	    }
	}

      g_object_unref (source);
      g_object_unref (target);
    }

  g_object_unref (dest);

  return 0;
}
