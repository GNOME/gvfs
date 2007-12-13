/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <locale.h>
#include <gio/gfile.h>

static int outstanding_mounts = 0;
static GMainLoop *main_loop;


static gboolean mount_mountable = FALSE;

static GOptionEntry entries[] = 
{
	{ "mountable", 'm', 0, G_OPTION_ARG_NONE, &mount_mountable, "Mount as mountable", NULL },
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
mount_mountable_done_cb (GObject *object,
			 GAsyncResult *res,
			 gpointer user_data)
{
  GFile *target;
  GError *error = NULL;
  
  target = g_file_mount_mountable_finish (G_FILE (object), res, &error);

  if (target == NULL)
    g_print ("Error mounting location: %s\n", error->message);
  else
    g_object_unref (target);
  
  outstanding_mounts--;
  
  if (outstanding_mounts == 0)
    g_main_loop_quit (main_loop);
}

static void
mount_done_cb (GObject *object,
	       GAsyncResult *res,
	       gpointer user_data)
{
  gboolean succeeded;
  GError *error = NULL;

  succeeded = g_file_mount_enclosing_volume_finish (G_FILE (object), res, &error);

  if (!succeeded)
    g_print ("Error mounting location: %s\n", error->message);
  
  outstanding_mounts--;
  
  if (outstanding_mounts == 0)
    g_main_loop_quit (main_loop);
}

static GMountOperation *
new_mount_op (void)
{
  GMountOperation *op;
  
  op = g_mount_operation_new ();

  g_signal_connect (op, "ask_password", (GCallback)ask_password_cb, NULL);

  return op;
}


static void
mount (GFile *file)
{
  GMountOperation *op;

  if (file == NULL)
    return;

  op = new_mount_op ();

  if (mount_mountable)
    g_file_mount_mountable (file, op, NULL, mount_mountable_done_cb, op);
  else
    g_file_mount_enclosing_volume (file, op, NULL, mount_done_cb, op);
    
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
  g_option_context_free (context);
  
  if (argc > 1)
    {
      int i;
      
      for (i = 1; i < argc; i++) {
	file = g_file_new_for_commandline_arg (argv[i]);
	mount (file);
	g_object_unref (file);
      }
    }

  main_loop = g_main_loop_new (NULL, FALSE);

  if (outstanding_mounts > 0)
    g_main_loop_run (main_loop);
  
  return 0;
}
