#ifndef __G_MOUNT_SPEC_H__
#define __G_MOUNT_SPEC_H__

#include <glib.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

#define G_MOUNT_SPEC_ITEM_INNER_TYPE_AS_STRING         \
    DBUS_TYPE_STRING_AS_STRING                         \
    DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING

#define G_MOUNT_SPEC_ITEM_TYPE_AS_STRING  \
  DBUS_STRUCT_BEGIN_CHAR_AS_STRING        \
   G_MOUNT_SPEC_ITEM_INNER_TYPE_AS_STRING \
  DBUS_STRUCT_END_CHAR_AS_STRING 

#define G_MOUNT_SPEC_INNER_TYPE_AS_STRING                     \
   DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING         \
   DBUS_TYPE_ARRAY_AS_STRING G_MOUNT_SPEC_ITEM_TYPE_AS_STRING 

#define G_MOUNT_SPEC_TYPE_AS_STRING  \
  DBUS_STRUCT_BEGIN_CHAR_AS_STRING   \
   G_MOUNT_SPEC_INNER_TYPE_AS_STRING \
  DBUS_STRUCT_END_CHAR_AS_STRING 

typedef struct {
  char *key;
  char *value;
} GMountSpecItem;

typedef struct {
  volatile int ref_count;
  GArray *items;
  char *mount_prefix;
} GMountSpec;

GMountSpec *g_mount_spec_new               (const char      *type);
GMountSpec *g_mount_spec_ref               (GMountSpec      *spec);
void        g_mount_spec_unref             (GMountSpec      *spec);
GMountSpec *g_mount_spec_get_unique_for    (GMountSpec      *spec);
GMountSpec *g_mount_spec_copy              (GMountSpec      *spec);
GMountSpec *g_mount_spec_from_dbus         (DBusMessageIter *iter);
void        g_mount_spec_to_dbus           (DBusMessageIter *iter,
					    GMountSpec      *spec);
void        g_mount_spec_to_dbus_with_path (DBusMessageIter *iter,
					    GMountSpec      *spec,
					    const char      *path);
void        g_mount_spec_set_mount_prefix  (GMountSpec      *spec,
					    const char      *mount_prefix);
void        g_mount_spec_set               (GMountSpec      *spec,
					    const char      *key,
					    const char      *value);
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

/* For debugging */
char *      g_mount_spec_to_string         (GMountSpec      *spec);

G_END_DECLS


#endif /* __G_MOUNT_SPEC_H__ */
