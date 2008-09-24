/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
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
#include <gio/gio.h>

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#define STDIN_FILENO 0

static int outstanding_mounts = 0;
static GMainLoop *main_loop;


static gboolean mount_mountable = FALSE;
static gboolean mount_unmount = FALSE;
static gboolean mount_list = FALSE;
static gboolean mount_list_info = FALSE;
static const char *unmount_scheme = NULL;

static const GOptionEntry entries[] = 
{
  { "mountable", 'm', 0, G_OPTION_ARG_NONE, &mount_mountable, "Mount as mountable", NULL },
  { "unmount", 'u', 0, G_OPTION_ARG_NONE, &mount_unmount, "Unmount", NULL},
  { "unmount-scheme", 's', 0, G_OPTION_ARG_STRING, &unmount_scheme, "Unmount all mounts with the given scheme", NULL},
  { "list", 'l', 0, G_OPTION_ARG_NONE, &mount_list, "List", NULL},
  { "list-info", 'i', 0, G_OPTION_ARG_NONE, &mount_list_info, "List extra information", NULL},
  { NULL }
};

static char *
prompt_for (const char *prompt, const char *default_value, gboolean echo)
{
#ifdef HAVE_TERMIOS_H
  struct termios term_attr; 
  int old_flags;
  gboolean restore_flags;
#endif
  char data[256];
  int len;

  if (default_value && *default_value != 0)
    g_print ("%s [%s]: ", prompt, default_value);
  else
    g_print ("%s: ", prompt);

  data[0] = 0;

#ifdef HAVE_TERMIOS_H
  restore_flags = FALSE;
  if (!echo && tcgetattr (STDIN_FILENO, &term_attr) == 0)
    {
      old_flags = term_attr.c_lflag; 
      term_attr.c_lflag &= ~ECHO;
      restore_flags = TRUE;
      
      if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &term_attr) != 0)
        g_print ("Warning! Password will be echoed");
    }

#endif

  fgets(data, sizeof (data), stdin);
  
#ifdef HAVE_TERMIOS_H
  if (restore_flags)
    {
      term_attr.c_lflag = old_flags;
      tcsetattr (STDIN_FILENO, TCSAFLUSH, &term_attr);
    }
#endif

  len = strlen (data);
  if (len > 0 && data[len-1] == '\n')
    data[len-1] = 0;

  if (!echo)
    g_print ("\n");
  
  if (*data == 0 && default_value)
    return g_strdup (default_value);
  return g_strdup (data);
}

static void
ask_password_cb (GMountOperation *op,
		 const char      *message,
		 const char      *default_user,
		 const char      *default_domain,
		 GAskPasswordFlags flags)
{
  char *s;
  g_print ("%s\n", message);

  if (flags & G_ASK_PASSWORD_NEED_USERNAME)
    {
      s = prompt_for ("User", default_user, TRUE);
      g_mount_operation_set_username (op, s);
      g_free (s);
    }
  
  if (flags & G_ASK_PASSWORD_NEED_DOMAIN)
    {
      s = prompt_for ("Domain", default_domain, TRUE);
      g_mount_operation_set_domain (op, s);
      g_free (s);
    }
  
  if (flags & G_ASK_PASSWORD_NEED_PASSWORD)
    {
      s = prompt_for ("Password", NULL, FALSE);
      g_mount_operation_set_password (op, s);
      g_free (s);
    }

  g_mount_operation_reply (op, G_MOUNT_OPERATION_HANDLED);
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
    g_file_mount_mountable (file, 0, op, NULL, mount_mountable_done_cb, op);
  else
    g_file_mount_enclosing_volume (file, 0, op, NULL, mount_done_cb, op);
    
  outstanding_mounts++;
}

static void
unmount_done_cb (GObject *object,
                 GAsyncResult *res,
                 gpointer user_data)
{
  gboolean succeeded;
  GError *error = NULL;

  succeeded = g_mount_unmount_finish (G_MOUNT (object), res, &error);

  g_object_unref (G_MOUNT (object));

  if (!succeeded)
    g_print ("Error unmounting mount: %s\n", error->message);
  
  outstanding_mounts--;
  
  if (outstanding_mounts == 0)
    g_main_loop_quit (main_loop);
}

static void
unmount (GFile *file)
{
  GMount *mount;
  GError *error = NULL;

  if (file == NULL)
    return;

  mount = g_file_find_enclosing_mount (file, NULL, &error);
  if (mount == NULL)
    {
      g_print ("Error finding enclosing mount: %s\n", error->message);
      return;
    }

  g_mount_unmount (mount, 0, NULL, unmount_done_cb, NULL);

  outstanding_mounts++;
}

/* =============== list mounts ================== */

static gboolean
iterate_gmain_timeout_function (gpointer data)
{
  g_main_loop_quit (main_loop);
  return FALSE;
}

static void
iterate_gmain(void)
{
  g_timeout_add (500, iterate_gmain_timeout_function, NULL);  
  g_main_loop_run (main_loop);
}

static void
show_themed_icon_names (GThemedIcon *icon, int indent)
{
  char **names;
  char **iter;

  g_print ("%*sthemed icons:", indent, " ");

  names = NULL;

  g_object_get (icon, "names", &names, NULL);

  for (iter = names; *iter; iter++)
    g_print ("  [%s]", *iter);

  g_print ("\n");
  g_strfreev (names);
}

static void
list_mounts (GList *mounts,
	     int indent,
	     gboolean only_with_no_volume)
{
  GList *l;
  int c;
  GMount *mount;
  GVolume *volume;
  char *name, *uuid, *uri;
  GFile *root;
  GIcon *icon;
  char **x_content_types;
  
  for (c = 0, l = mounts; l != NULL; l = l->next, c++)
    {
      mount = (GMount *) l->data;
      
      if (only_with_no_volume)
	{
	  volume = g_mount_get_volume (mount);
	  if (volume != NULL)
	    {
	      g_object_unref (volume);
	      continue;
	    }
	}

      name = g_mount_get_name (mount);
      root = g_mount_get_root (mount);
      uri = g_file_get_uri (root);
      
      g_print ("%*sMount(%d): %s -> %s\n", indent, "", c, name, uri);

      if (mount_list_info)
	{
	  uuid = g_mount_get_uuid (mount);
	  if (uuid)
	    g_print ("%*suuid=%s\n", indent + 2, "", uuid);

          icon = g_mount_get_icon (mount);
          if (icon)
            {
              if (G_IS_THEMED_ICON (icon))
                show_themed_icon_names (G_THEMED_ICON (icon), indent + 2);
              
              g_object_unref (icon);
            }

          x_content_types = g_mount_guess_content_type_sync (mount, FALSE, NULL, NULL);
          if (x_content_types != NULL && g_strv_length (x_content_types) > 0)
            {
              int n;
              g_print ("%*sx_content_types:", indent + 2, "");
              for (n = 0; x_content_types[n] != NULL; n++)
                  g_print (" %s", x_content_types[n]);
              g_print ("\n");
            }
          g_strfreev (x_content_types);

	  g_print ("%*scan_unmount=%d\n", indent + 2, "", g_mount_can_unmount (mount));
	  g_print ("%*scan_eject=%d\n", indent + 2, "", g_mount_can_eject (mount));
	  g_free (uuid);
	}
      
      g_object_unref (root);
      g_free (name);
      g_free (uri);
    }  
}

static void
list_volumes (GList *volumes,
	      int indent,
	      gboolean only_with_no_drive)
{
  GList *l, *mounts;
  int c, i;
  GMount *mount;
  GVolume *volume;
  GDrive *drive;
  char *name;
  char *uuid;
  GFile *activation_root;
  char **ids;
  GIcon *icon;
  
  for (c = 0, l = volumes; l != NULL; l = l->next, c++)
    {
      volume = (GVolume *) l->data;
      
      if (only_with_no_drive)
	{
	  drive = g_volume_get_drive (volume);
	  if (drive != NULL)
	    {
	      g_object_unref (drive);
	      continue;
	    }
	}
      
      name = g_volume_get_name (volume);
      
      g_print ("%*sVolume(%d): %s\n", indent, "", c, name);
      g_free (name);

      if (mount_list_info)
	{
	  ids = g_volume_enumerate_identifiers (volume);
	  if (ids && ids[0] != NULL)
	    {
	      g_print ("%*sids:\n", indent+2, "");
	      for (i = 0; ids[i] != NULL; i++)
		{
		  char *id = g_volume_get_identifier (volume,
						      ids[i]);
		  g_print ("%*s %s: '%s'\n", indent+2, "", ids[i], id);
		  g_free (id);
		}
	    }
	  g_strfreev (ids);
	  
	  uuid = g_volume_get_uuid (volume);
	  if (uuid)
	    g_print ("%*suuid=%s\n", indent + 2, "", uuid);
	  activation_root = g_volume_get_activation_root (volume);
	  if (activation_root)
            {
              char *uri;
              uri = g_file_get_uri (activation_root);
              g_print ("%*sactivation_root=%s\n", indent + 2, "", uri);
              g_free (uri);
              g_object_unref (activation_root);
            }
      icon = g_volume_get_icon (volume);
      if (icon)
        {
          if (G_IS_THEMED_ICON (icon))
            show_themed_icon_names (G_THEMED_ICON (icon), indent + 2);

          g_object_unref (icon);
        }

	  g_print ("%*scan_mount=%d\n", indent + 2, "", g_volume_can_mount (volume));
	  g_print ("%*scan_eject=%d\n", indent + 2, "", g_volume_can_eject (volume));
	  g_free (uuid);
	}
      
      mount = g_volume_get_mount (volume);
      if (mount)
	{
	  mounts = g_list_prepend (NULL, mount);
	  list_mounts (mounts, indent + 2, FALSE);
	  g_list_free (mounts);
	  g_object_unref (mount);
	}
    }  
}

static void
list_drives (GList *drives,
	     int indent)
{
  GList *volumes, *l;
  int c, i;
  GDrive *drive;
  char *name;
  char **ids;
  GIcon *icon;
  
  for (c = 0, l = drives; l != NULL; l = l->next, c++)
    {
      drive = (GDrive *) l->data;
      name = g_drive_get_name (drive);
      
      g_print ("%*sDrive(%d): %s\n", indent, "", c, name);
      g_free (name);
      
      if (mount_list_info)
	{
	  ids = g_drive_enumerate_identifiers (drive);
	  if (ids && ids[0] != NULL)
	    {
	      g_print ("%*sids:\n", indent+2, "");
	      for (i = 0; ids[i] != NULL; i++)
		{
		  char *id = g_drive_get_identifier (drive,
						     ids[i]);
		  g_print ("%*s %s: '%s'\n", indent+2, "", ids[i], id);
		  g_free (id);
		}
	    }
	  g_strfreev (ids);

          icon = g_drive_get_icon (drive);
          if (icon)
          {
                  if (G_IS_THEMED_ICON (icon))
                          show_themed_icon_names (G_THEMED_ICON (icon), indent + 2);
                  g_object_unref (icon);
          }

	  g_print ("%*sis_media_removable=%d\n", indent + 2, "", g_drive_is_media_removable (drive));
	  g_print ("%*shas_media=%d\n", indent + 2, "", g_drive_has_media (drive));
	  g_print ("%*sis_media_check_automatic=%d\n", indent + 2, "", g_drive_is_media_check_automatic (drive));
	  g_print ("%*scan_poll_for_media=%d\n", indent + 2, "", g_drive_can_poll_for_media (drive));
	  g_print ("%*scan_eject=%d\n", indent + 2, "", g_drive_can_eject (drive));
	}
      
      volumes = g_drive_get_volumes (drive);
      list_volumes (volumes, indent + 2, FALSE);
      g_list_foreach (volumes, (GFunc)g_object_unref, NULL);
      g_list_free (volumes);
    }
}


static void
list_monitor_items(void)
{
  GVolumeMonitor *volume_monitor;
  GList *drives, *volumes, *mounts;

  volume_monitor = g_volume_monitor_get();
  
  /* populate gvfs network mounts */
  iterate_gmain();

  drives = g_volume_monitor_get_connected_drives (volume_monitor);
  list_drives (drives, 0);
  g_list_foreach (drives, (GFunc)g_object_unref, NULL);
  g_list_free (drives);
  
  volumes = g_volume_monitor_get_volumes (volume_monitor);
  list_volumes (volumes, 0, TRUE);
  g_list_foreach (volumes, (GFunc)g_object_unref, NULL);
  g_list_free (volumes);
  
  mounts = g_volume_monitor_get_mounts (volume_monitor);
  list_mounts (mounts, 0, TRUE);
  g_list_foreach (mounts, (GFunc)g_object_unref, NULL);
  g_list_free (mounts);

  g_object_unref (volume_monitor);
}

static void
unmount_all_with_scheme (const char *scheme)
{
  GVolumeMonitor *volume_monitor;
  GList *mounts;
  GList *l;

  volume_monitor = g_volume_monitor_get();

  /* populate gvfs network mounts */
  iterate_gmain();

  mounts = g_volume_monitor_get_mounts (volume_monitor);
  for (l = mounts; l != NULL; l = l->next) {
    GMount *mount = G_MOUNT (l->data);
    GFile *root;

    root = g_mount_get_root (mount);
    if (g_file_has_uri_scheme (root, scheme)) {
            unmount (root);
    }
    g_object_unref (root);
  }
  g_list_foreach (mounts, (GFunc)g_object_unref, NULL);
  g_list_free (mounts);

  g_object_unref (volume_monitor);
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

  main_loop = g_main_loop_new (NULL, FALSE);
  
  if (mount_list)
    list_monitor_items ();
  else if (unmount_scheme != NULL)
    {
      unmount_all_with_scheme (unmount_scheme);
    }
  else if (argc > 1)
    {
      int i;
      
      for (i = 1; i < argc; i++) {
	file = g_file_new_for_commandline_arg (argv[i]);
	if (mount_unmount)
	  unmount (file);
	else
	  mount (file);
	g_object_unref (file);
      }
    }
  
  if (outstanding_mounts > 0)
    g_main_loop_run (main_loop);
  
  return 0;
}
