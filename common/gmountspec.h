#ifndef __G_MOUNT_SPEC_H__
#define __G_MOUNT_SPEC_H__

#include <glib.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

typedef struct {
  char *key;
  char *value;
} GMountSpecItem;

typedef struct {
  GArray *items;
  char *mount_prefix;
} GMountSpec;

GMountSpec *g_mount_spec_new       (void);
void        g_mount_spec_free      (GMountSpec      *spec);
GMountSpec *g_mount_spec_from_dbus (DBusMessageIter *iter);
void        g_mount_spec_to_dbus   (DBusMessageIter *iter,
				    GMountSpec      *spec);
void        g_mount_spec_add_item  (GMountSpec      *spec,
				    const char      *key,
				    const char      *value);

G_END_DECLS


#endif /* __G_MOUNT_SPEC_H__ */
