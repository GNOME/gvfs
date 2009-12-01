
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
 * Author: Alexader Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include <gconf/gconf-client.h>

#include "gapplookupgconf.h"


struct _GAppLookupGConf {
  GObject parent;

};

static void lookup_iface_init (GDesktopAppInfoLookupIface *iface);
static void g_app_lookup_gconf_finalize (GObject *object);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GAppLookupGConf, g_app_lookup_gconf, G_TYPE_OBJECT, 0,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_DESKTOP_APP_INFO_LOOKUP,
							       lookup_iface_init))

static void
g_app_lookup_gconf_finalize (GObject *object)
{
  if (G_OBJECT_CLASS (g_app_lookup_gconf_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_app_lookup_gconf_parent_class)->finalize) (object);
}

static GObject *
g_app_lookup_gconf_constructor (GType                  type,
                                guint                  n_construct_properties,
                                GObjectConstructParam *construct_properties)
{
  GObject *object;
  GAppLookupGConfClass *klass;
  GObjectClass *parent_class;  

  object = NULL;

  /* Invoke parent constructor. */
  klass = G_APP_LOOKUP_GCONF_CLASS (g_type_class_peek (G_TYPE_APP_LOOKUP_GCONF));
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
  object = parent_class->constructor (type,
                                      n_construct_properties,
                                      construct_properties);

  return object;
}

static void
g_app_lookup_gconf_init (GAppLookupGConf *lookup)
{
}

static void
g_app_lookup_gconf_class_finalize (GAppLookupGConfClass *klass)
{
}


static void
g_app_lookup_gconf_class_init (GAppLookupGConfClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructor = g_app_lookup_gconf_constructor;
  gobject_class->finalize = g_app_lookup_gconf_finalize;
}

#define GCONF_PATH_PREFIX "/desktop/gnome/url-handlers/"

static GAppInfo *
get_default_for_uri_scheme (GDesktopAppInfoLookup *lookup,
                            const char  *uri_scheme)
{
  GAppInfo *appinfo;
  GConfClient *client;
  char *command_key, *enabled_key, *terminal_key, *command;
  gboolean enabled, needs_terminal;
  GAppInfoCreateFlags flags;

  appinfo = NULL;
  
  client = gconf_client_get_default ();
  
  command_key = g_strconcat (GCONF_PATH_PREFIX,
                             uri_scheme,
                             "/command",
                             NULL);
  command = gconf_client_get_string (client,
                                     command_key,
                                     NULL);
  g_free (command_key);
  if (command)
    {
      enabled_key = g_strconcat (GCONF_PATH_PREFIX,
                                 uri_scheme,
                                 "/enabled",
                                 NULL);
      enabled = gconf_client_get_bool (client,
                                       enabled_key,
                                       NULL);
      g_free (enabled_key);
      
      terminal_key = g_strconcat (GCONF_PATH_PREFIX,
                                  uri_scheme,
                                  "/needs_terminal",
                                  NULL);
      needs_terminal = gconf_client_get_bool (client,
                                              terminal_key,
                                              NULL);
      g_free (terminal_key);

      if (enabled)
        {
          if (g_str_has_suffix (command, "\"%s\"") ||
              g_str_has_suffix (command, "\'%s\'"))
            command[strlen (command) - 4] = 0;
          else if (g_str_has_suffix (command, "%s"))
            command[strlen (command) - 2] = 0;

          flags = G_APP_INFO_CREATE_SUPPORTS_URIS;
          if (needs_terminal)
            flags |= G_APP_INFO_CREATE_NEEDS_TERMINAL;
          appinfo = g_app_info_create_from_commandline (command,
                                                        NULL,
                                                        flags,
                                                        NULL);
        }
    }
  
  g_object_unref (client);
  g_free (command);
  
  return appinfo;
}

static void
lookup_iface_init (GDesktopAppInfoLookupIface *iface)
{
  iface->get_default_for_uri_scheme = get_default_for_uri_scheme;
}

void 
g_app_lookup_gconf_register (GIOModule *module)
{
  g_app_lookup_gconf_register_type (G_TYPE_MODULE (module));
  g_io_extension_point_implement (G_DESKTOP_APP_INFO_LOOKUP_EXTENSION_POINT_NAME,
				  G_TYPE_APP_LOOKUP_GCONF,
				  "gconf",
				  10);
}
