#include <config.h>
#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <gio/gfile.h>
#include <gio/gdirectorymonitor.h>

static GMainLoop *main_loop;

static GOptionEntry entries[] = {
  { NULL }
};

static gboolean
dir_monitor_callback (GDirectoryMonitor* monitor,
		      GFile* child,
		      GFile* other_file,
		      GDirectoryMonitorEvent eflags)
{
  g_print ("Directory Monitor Event:\n");
  g_print ("Child = %s\n", g_file_get_parse_name (child));
  switch (eflags)
    {
    case G_DIRECTORY_MONITOR_EVENT_CHANGED:
      g_print ("Event = CHANGED\n");
      break;
    case G_DIRECTORY_MONITOR_EVENT_DELETED:
      g_print ("Event = DELETED\n");
      break;
    case G_DIRECTORY_MONITOR_EVENT_CREATED:
      g_print ("Event = CREATED\n");
      break;
    case G_DIRECTORY_MONITOR_EVENT_UNMOUNTED:
      g_print ("Event = UNMOUNTED\n");
      break;
    case G_DIRECTORY_MONITOR_EVENT_ATTRIBUTE_CHANGED:
      g_print ("Event = ATTRIB CHANGED\n");
      break;
    }
  
  return TRUE;
}

int
main (int argc, char *argv[])
{
  GDirectoryMonitor* dmonitor;
  GError *error;
  GOptionContext *context;
  GFile *file;
  
  setlocale (LC_ALL, "");
  
  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- monitor directory <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  
  if (argc > 1)
    {
      file = g_file_get_for_commandline_arg (argv[1]);
      dmonitor = g_file_monitor_directory (file);
      g_signal_connect (dmonitor, "changed", (GCallback)dir_monitor_callback, NULL);
    }
  
  main_loop = g_main_loop_new (NULL, FALSE);
  
  g_main_loop_run (main_loop);
  
  return 0;
}
