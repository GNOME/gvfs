#include <config.h>
#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <gio/gfile.h>
#include <gio/gfilemonitor.h>

static GMainLoop *main_loop;

static GOptionEntry entries[] = {
  { NULL }
};

static gboolean
file_monitor_callback (GFileMonitor* monitor,
		       GFile* child,
		       GFileMonitorEventFlags eflags)
{
  g_print ("File Monitor Event:\n");
  g_print ("File = %s\n", g_file_get_parse_name (child));
  switch (eflags)
    {
    case G_FILE_MONITOR_EVENT_CHANGED:
      g_print ("Event = CHANGED\n");
      break;
    case G_FILE_MONITOR_EVENT_DELETED:
      g_print ("Event = DELETED\n");
      break;
    case G_FILE_MONITOR_EVENT_CREATED:
      g_print ("Event = CREATED\n");
      break;
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
      g_print ("Event = UNMOUNTED\n");
      break;
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
      g_print ("Event = ATTRIB CHANGED\n");
      break;
    }
  
  return TRUE;
}

int
main (int argc, char *argv[])
{
  GFileMonitor* fmonitor;
  GError *error;
  GOptionContext *context;
  GFile *file;
  
  setlocale (LC_ALL, "");
  
  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- monitor file <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  
  if (argc > 1)
    {
      file = g_file_get_for_commandline_arg (argv[1]);
      fmonitor = g_file_monitor_file (file);
      g_signal_connect (fmonitor, "changed", (GCallback)file_monitor_callback, NULL);
    }
  
  main_loop = g_main_loop_new (NULL, FALSE);
  
  g_main_loop_run (main_loop);
  
  return 0;
}
