/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2011 Red Hat, Inc.
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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Bastien Nocera <hadess@hadess.net>
 */

#include "config.h"

#include <glib.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

static gboolean query = FALSE;
static gboolean set = FALSE;
static gboolean show_version = FALSE;

static GOptionEntry entries[] =
{
  { "query", 0, 0, G_OPTION_ARG_NONE, &query, N_("Query handler for mime-type"), NULL },
  { "set", 0, 0, G_OPTION_ARG_NONE, &set, N_("Set handler for mime-type"), NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Show program version"), NULL },
  { NULL }
};

static GAppInfo *
get_app_info_for_id (const char *id)
{
  GList *list, *l;
  GAppInfo *ret_info;

  list = g_app_info_get_all ();
  ret_info = NULL;
  for (l = list; l != NULL; l = l->next)
    {
      GAppInfo *info;

      info = l->data;
      if (ret_info == NULL && g_strcmp0 (g_app_info_get_id (info), id) == 0)
        ret_info = info;
      else
        g_object_unref (info);
    }
  g_list_free (list);

  return ret_info;
}

int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  const char *mimetype;
  gchar *param;
  gchar *summary;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  error = NULL;
  param = g_strdup_printf ("%s [%s]", _("MIMETYPE"), _("HANDLER"));
  summary = _("Get or set the handler for a mime-type.");

  context = g_option_context_new (param);
  g_option_context_set_summary (context, summary);
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  g_option_context_free (context);
  g_free (param);

  if (error != NULL || (query == set && !show_version))
    {
      g_printerr (_("Error parsing commandline options: %s\n"),
                  error ? error->message : _("Specify either --query or --set"));
      g_printerr ("\n");
      g_printerr (_("Try \"%s --help\" for more information."), g_get_prgname ());
      g_printerr ("\n");
      if (error != NULL)
        g_error_free (error);
      return 1;
    }

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  if (query && argc != 2)
    {
      g_printerr (_("Must specify a single mime-type.\n"));
      g_printerr (_("Try \"%s --help\" for more information."), g_get_prgname ());
      g_printerr ("\n");
      return 1;
    }
  else if (set && argc != 3)
    {
      g_printerr (_("Must specify the mime-type followed by the default handler.\n"));
      g_printerr (_("Try \"%s --help\" for more information."), g_get_prgname ());
      g_printerr ("\n");
      return 1;
    }

  mimetype = argv[1];

  if (query)
    {
      GAppInfo *info;

      info = g_app_info_get_default_for_type (mimetype, FALSE);
      if (!info)
        {
          g_print (_("No default applications for '%s'\n"), mimetype);
        }
      else
        {
          GList *list, *l;

          g_print (_("Default application for '%s': %s\n"), mimetype, g_app_info_get_id (info));
          g_object_unref (info);

          list = g_app_info_get_all_for_type (mimetype);
          if (list != NULL)
            g_print (_("Registered applications:\n"));
	  else
	    g_print (_("No registered applications\n"));
          for (l = list; l != NULL; l = l->next)
	    {
	      info = l->data;
	      g_print ("\t%s\n", g_app_info_get_id (info));
	      g_object_unref (info);
	    }
	  g_list_free (list);

	  list = g_app_info_get_recommended_for_type (mimetype);
	  if (list != NULL)
            g_print (_("Recommended applications:\n"));
	  else
	    g_print (_("No recommended applications\n"));
          for (l = list; l != NULL; l = l->next)
	    {
	      info = l->data;
	      g_print ("\t%s\n", g_app_info_get_id (info));
	      g_object_unref (info);
	    }
	  g_list_free (list);
        }
    }
  else if (set)
    {
      const char *handler;
      GAppInfo *info;

      handler = argv[2];

      info = get_app_info_for_id (handler);
      if (info == NULL)
        {
          g_printerr (_("Failed to load info for handler '%s'\n"), handler);
          return 1;
        }

      if (g_app_info_set_as_default_for_type (info, mimetype, &error) == FALSE)
        {
          g_printerr (_("Failed to set '%s' as the default handler for '%s': %s\n"),
                      handler, mimetype, error->message);
          g_error_free (error);
          g_object_unref (info);
          return 1;
        }
      g_print ("Set %s as the default for %s\n", g_app_info_get_id (info), mimetype);
      g_object_unref (info);
    }

  return 0;
}
