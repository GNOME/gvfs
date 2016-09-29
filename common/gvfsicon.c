/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>
#include <string.h>
#include <glib/gi18n-lib.h>

#include "gvfsicon.h"

static void g_vfs_icon_icon_iface_init          (GIconIface          *iface);

/* Because of the way dependencies are currently set up, the
 * GLoadableIcon interface is in client/gvfsiconloadable.c and is
 * added in g_io_module_load() in client/gdaemonvfs.c.
 */

enum
{
  PROP_0,
  PROP_MOUNT_SPEC,
  PROP_ICON_ID
};

G_DEFINE_TYPE_EXTENDED (GVfsIcon,
			g_vfs_icon,
			G_TYPE_OBJECT,
			0,
			G_IMPLEMENT_INTERFACE (G_TYPE_ICON,
					       g_vfs_icon_icon_iface_init))

static void
g_vfs_icon_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  GVfsIcon *icon = G_VFS_ICON (object);

  switch (prop_id)
    {
    case PROP_MOUNT_SPEC:
      g_value_set_boxed (value, icon->mount_spec);
      break;

    case PROP_ICON_ID:
      g_value_set_string (value, icon->icon_id);
      break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
g_vfs_icon_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  GVfsIcon *icon = G_VFS_ICON (object);

  switch (prop_id)
    {
    case PROP_MOUNT_SPEC:
      icon->mount_spec = g_mount_spec_ref (g_value_get_boxed (value));
      break;

    case PROP_ICON_ID:
      icon->icon_id = g_strdup (g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
g_vfs_icon_finalize (GObject *object)
{
  GVfsIcon *vfs_icon;

  vfs_icon = G_VFS_ICON (object);

  if (vfs_icon->mount_spec != NULL)
    g_mount_spec_unref (vfs_icon->mount_spec);
  g_free (vfs_icon->icon_id);

  G_OBJECT_CLASS (g_vfs_icon_parent_class)->finalize (object);
}

static void
g_vfs_icon_class_init (GVfsIconClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = g_vfs_icon_get_property;
  gobject_class->set_property = g_vfs_icon_set_property;
  gobject_class->finalize = g_vfs_icon_finalize;

  /**
   * GVfsIcon:mount-spec:
   *
   * The mount spec.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MOUNT_SPEC,
                                   g_param_spec_boxed ("mount-spec",
                                                       "Mount Spec",
                                                       "Mount Spec",
                                                       G_TYPE_MOUNT_SPEC,
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_NAME |
                                                       G_PARAM_STATIC_BLURB |
                                                       G_PARAM_STATIC_NICK));

  /**
   * GVfsIcon:icon-id:
   *
   * The id of the icon.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_ICON_ID,
                                   g_param_spec_string ("icon-id",
                                                       "Icon identifier",
                                                       "Icon identifier",
                                                       NULL,
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_NAME |
                                                       G_PARAM_STATIC_BLURB |
                                                       G_PARAM_STATIC_NICK));
}

static void
g_vfs_icon_init (GVfsIcon *file)
{
}

GMountSpec *
g_vfs_icon_get_mount_spec (GVfsIcon *vfs_icon)
{
  g_return_val_if_fail (G_VFS_IS_ICON (vfs_icon), NULL);
  return g_mount_spec_ref (vfs_icon->mount_spec);
}

const gchar *
g_vfs_icon_get_icon_id (GVfsIcon *vfs_icon)
{
  g_return_val_if_fail (G_VFS_IS_ICON (vfs_icon), NULL);
  return vfs_icon->icon_id;
}


GIcon *
g_vfs_icon_new (GMountSpec  *mount_spec,
                const gchar *icon_id)
{
  return G_ICON (g_object_new (G_VFS_TYPE_ICON,
                               "mount-spec", mount_spec,
                               "icon-id", icon_id,
                               NULL));
}

static guint
g_vfs_icon_hash (GIcon *icon)
{
  GVfsIcon *vfs_icon = G_VFS_ICON (icon);

  return g_mount_spec_hash (vfs_icon->mount_spec) ^ g_str_hash (vfs_icon->icon_id);
}

static int
safe_strcmp (const char *a,
             const char *b)
{
  if (a == NULL)
    a = "";
  if (b == NULL)
    b = "";

  return strcmp (a, b);
}

static gboolean
g_vfs_icon_equal (GIcon *icon1,
                  GIcon *icon2)
{
  GVfsIcon *vfs1 = G_VFS_ICON (icon1);
  GVfsIcon *vfs2 = G_VFS_ICON (icon2);

  return g_mount_spec_equal (vfs1->mount_spec, vfs2->mount_spec) &&
    (safe_strcmp (vfs1->icon_id, vfs2->icon_id) == 0);
}

static gboolean
g_vfs_icon_to_tokens (GIcon *icon,
		      GPtrArray *tokens,
                      gint  *out_version)
{
  GVfsIcon *vfs_icon = G_VFS_ICON (icon);
  char *s;

  g_return_val_if_fail (out_version != NULL, FALSE);

  *out_version = 0;

  s = g_mount_spec_to_string (vfs_icon->mount_spec);
  g_ptr_array_add (tokens, s);
  g_ptr_array_add (tokens, g_strdup (vfs_icon->icon_id));

  return TRUE;
}

static GIcon *
g_vfs_icon_from_tokens (gchar  **tokens,
                        gint     num_tokens,
                        gint     version,
                        GError **error)
{
  GMountSpec *mount_spec;
  GIcon *icon;

  icon = NULL;

  if (version != 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_ARGUMENT,
                   _("Can’t handle version %d of GVfsIcon encoding"),
                   version);
      goto out;
    }

  if (num_tokens != 2)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_INVALID_ARGUMENT,
                           _("Malformed input data for GVfsIcon"));
      goto out;
    }

  mount_spec = g_mount_spec_new_from_string (tokens[0], error);
  if (mount_spec == NULL)
    goto out;

  icon = g_vfs_icon_new (mount_spec, tokens[1]);
  g_mount_spec_unref (mount_spec);

 out:
  return icon;
}

static GVariant *
g_vfs_icon_serialize (GIcon *icon)
{
  GVfsIcon *vfs_icon = G_VFS_ICON (icon);

  return g_variant_new ("(@ss)",
                        g_variant_new_take_string (g_mount_spec_to_string (vfs_icon->mount_spec)),
                        vfs_icon->icon_id);
}

GIcon *
g_vfs_icon_deserialize (GVariant *value)
{
  const gchar *mount_spec_str;
  const gchar *id_str;
  GMountSpec *mount_spec;
  GIcon *icon;

  if (!g_variant_is_of_type (value, G_VARIANT_TYPE ("(ss)")))
    return NULL;

  g_variant_get (value, "(&s&s)", &mount_spec_str, &id_str);

  mount_spec = g_mount_spec_new_from_string (mount_spec_str, NULL);
  if (mount_spec == NULL)
    return NULL;

  icon = g_vfs_icon_new (mount_spec, id_str);
  g_mount_spec_unref (mount_spec);

  return icon;
}

static void
g_vfs_icon_icon_iface_init (GIconIface *iface)
{
  iface->hash = g_vfs_icon_hash;
  iface->equal = g_vfs_icon_equal;
  iface->to_tokens = g_vfs_icon_to_tokens;
  iface->from_tokens = g_vfs_icon_from_tokens;
  iface->serialize = g_vfs_icon_serialize;
}
