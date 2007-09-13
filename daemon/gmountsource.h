#ifndef __G_MOUNT_SOURCE_H__
#define __G_MOUNT_SOURCE_H__

#include <glib-object.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_TYPE_MOUNT_SOURCE         (g_mount_source_get_type ())
#define G_MOUNT_SOURCE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MOUNT_SOURCE, GMountSource))
#define G_MOUNT_SOURCE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MOUNT_SOURCE, GMountSourceClass))
#define G_IS_MOUNT_SOURCE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MOUNT_SOURCE))
#define G_IS_MOUNT_SOURCE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MOUNT_SOURCE))
#define G_MOUNT_SOURCE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_MOUNT_SOURCE, GMountSourceClass))

typedef struct _GMountSource        GMountSource;
typedef struct _GMountSourceClass   GMountSourceClass;

struct _GMountSourceClass
{
  GObjectClass parent_class;
};

typedef void   (*RequestMountSpecCallback)   (GMountSource *source,
					      GMountSpec *mount_spec,
					      GError *error,
					      gpointer data);


GType g_mount_source_get_type (void) G_GNUC_CONST;

GMountSource *g_mount_source_new_dbus                 (const char                *dbus_id,
						       const char                *obj_path,
						       GMountSpec                *spec);
GMountSource *g_mount_source_new_null                 (GMountSpec                *spec);
GMountSpec *  g_mount_source_request_mount_spec       (GMountSource              *source,
						       GError                   **error);
void          g_mount_source_request_mount_spec_async (GMountSource              *source,
						       RequestMountSpecCallback   callback,
						       gpointer                   data);
void          g_mount_source_done                     (GMountSource              *source);
void          g_mount_source_failed                   (GMountSource              *source,
						       GError                    *error);
void          g_mount_source_set_is_automount         (GMountSource              *source,
						       gboolean                   is_automount);
gboolean      g_mount_source_get_is_automount         (GMountSource              *source);

G_END_DECLS

#endif /* __G_MOUNT_SOURCE_H__ */
