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

GMountSpec *g_mount_spec_new       (const char      *type);
GMountSpec *g_mount_spec_ref       (GMountSpec      *spec);
void        g_mount_spec_unref     (GMountSpec      *spec);
GMountSpec *g_mount_spec_from_dbus (DBusMessageIter *iter);
void        g_mount_spec_to_dbus   (DBusMessageIter *iter,
				    GMountSpec      *spec);
void        g_mount_spec_add_item  (GMountSpec      *spec,
				    const char      *key,
				    const char      *value);
gboolean    g_mount_spec_match     (GMountSpec      *mount,
				    GMountSpec      *path);

G_END_DECLS


#endif /* __G_MOUNT_SPEC_H__ */
