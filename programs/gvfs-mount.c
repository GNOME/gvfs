#include <config.h>

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <locale.h>
#include <gio/gfile.h>

static int outstanding_mounts = 0;
static GMainLoop *main_loop;

static GOptionEntry entries[] = 
{
	{ NULL }
};

static char *
prompt_for (const char *prompt, const char *default_value)
{
  char data[256];
  int len;

  if (default_value && *default_value != 0)
    g_print ("%s [%s]: ", prompt, default_value);
  else
    g_print ("%s: ", prompt);

  data[0] = 0;
  fgets(data, sizeof (data), stdin);
  len = strlen (data);
  if (len > 0 && data[len-1] == '\n')
    data[len-1] = 0;
  
  if (*data == 0 && default_value)
    return g_strdup (default_value);
  return g_strdup (data);
}

static gboolean
ask_password_cb (GMountOperation *op,
		 const char      *message,
		 const char      *default_user,
		 const char      *default_domain,
		 GPasswordFlags   flags)
{
  char *s;
  g_print ("%s\n", message);

  if (flags & G_PASSWORD_FLAGS_NEED_USERNAME)
    {
      s = prompt_for ("User", default_user);
      g_mount_operation_set_username (op, s);
      g_free (s);
    }
  
  if (flags & G_PASSWORD_FLAGS_NEED_DOMAIN)
    {
      s = prompt_for ("Domain", default_domain);
      g_mount_operation_set_domain (op, s);
      g_free (s);
    }
  
  if (flags & G_PASSWORD_FLAGS_NEED_PASSWORD)
    {
      s = prompt_for ("Password", NULL);
      g_mount_operation_set_password (op, s);
      g_free (s);
    }

  g_mount_operation_reply (op, FALSE);

  return TRUE;
}

static void
mount_done_cb (GMountOperation *op,
	       gboolean         succeeded,
	       GError          *error)
{
  if (!succeeded)
    g_print ("Error mounting location: %s\n", error->message);
  
  outstanding_mounts--;
  
  if (outstanding_mounts == 0)
    g_main_loop_quit (main_loop);
}

static void
mount (GFile *file)
{
  GMountOperation *op;

  if (file == NULL)
    return;

  op = g_mount_operation_new ();

  g_signal_connect (op, "ask_password", (GCallback)ask_password_cb, NULL);
  g_signal_connect (op, "done", (GCallback)mount_done_cb, NULL);
  
  g_file_mount (file, op);
  outstanding_mounts++;
}

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error;
  GFile *file;
  
  setlocale (LC_ALL, "");

  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- mount <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  
  if (argc > 1)
    {
      int i;
      
      for (i = 1; i < argc; i++) {
	file = g_file_get_for_commandline_arg (argv[i]);
	mount (file);
	g_object_unref (file);
      }
    }

  main_loop = g_main_loop_new (NULL, FALSE);

  if (outstanding_mounts > 0)
    g_main_loop_run (main_loop);
  
  return 0;
}
