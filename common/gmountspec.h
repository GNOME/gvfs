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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_MOUNT_SPEC_H__
#define __G_MOUNT_SPEC_H__

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  char *key;
  char *value;
} GMountSpecItem;

typedef struct {
  volatile int ref_count;
  GArray *items;
  char *mount_prefix;
  gboolean is_unique;
} GMountSpec;

GMountSpec *g_mount_spec_new               (const char      *type);
GMountSpec *g_mount_spec_new_from_data     (GArray          *items,
					    char            *mount_prefix);
GMountSpec *g_mount_spec_ref               (GMountSpec      *spec);
void        g_mount_spec_unref             (GMountSpec      *spec);
GMountSpec *g_mount_spec_get_unique_for    (GMountSpec      *spec);
GMountSpec *g_mount_spec_copy              (GMountSpec      *spec);
GMountSpec *g_mount_spec_from_dbus         (GVariant        *value);
GVariant   *g_mount_spec_to_dbus           (GMountSpec      *spec);
GVariant   *g_mount_spec_to_dbus_with_path (GMountSpec      *spec,
					    const char      *path);
void        g_mount_spec_set_mount_prefix  (GMountSpec      *spec,
					    const char      *mount_prefix);
void        g_mount_spec_set               (GMountSpec      *spec,
					    const char      *key,
					    const char      *value);
void        g_mount_spec_take              (GMountSpec      *spec,
                                            const char      *key,
                                            char            *value);
void        g_mount_spec_set_with_len      (GMountSpec      *spec,
					    const char      *key,
					    const char      *value,
					    int              value_len);
guint       g_mount_spec_hash              (gconstpointer    mount);
gboolean    g_mount_spec_equal             (GMountSpec      *mount1,
					    GMountSpec      *mount2);
gboolean    g_mount_spec_match             (GMountSpec      *mount,
					    GMountSpec      *path);
gboolean    g_mount_spec_match_with_path   (GMountSpec      *mount,
					    GMountSpec      *spec,
					    const char      *path);
const char *g_mount_spec_get               (GMountSpec      *spec,
					    const char      *key);
const char *g_mount_spec_get_type          (GMountSpec      *spec);

char *      g_mount_spec_to_string         (GMountSpec      *spec);

GMountSpec *g_mount_spec_new_from_string   (const gchar     *str,
                                            GError         **error);

char *      g_mount_spec_canonicalize_path (const char      *path);


#define G_TYPE_MOUNT_SPEC (g_type_mount_spec_get_gtype ())
GType g_type_mount_spec_get_gtype (void) G_GNUC_CONST;

G_END_DECLS


#endif /* __G_MOUNT_SPEC_H__ */
