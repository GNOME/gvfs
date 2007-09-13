#ifndef __G_CONTENT_TYPE_H__
#define __G_CONTENT_TYPE_H__

#include <glib.h>
#include <gio/gicon.h>

G_BEGIN_DECLS

gboolean g_content_type_equals            (const char   *type1,
					   const char   *type2);
gboolean g_content_type_is_a              (const char   *type,
					   const char   *supertype);
gboolean g_content_type_is_unknown        (const char   *type);
char *   g_content_type_get_description   (const char   *type);
char *   g_content_type_get_mime_type     (const char   *type);
GIcon *  g_content_type_get_icon          (const char   *type);
gboolean g_content_type_can_be_executable (const char   *type);

char *   g_content_type_guess             (const char   *filename,
					   const guchar *data,
					   gsize         data_size);

GList *  g_get_registered_content_types   (void);

G_END_DECLS

#endif /* __G_CONTENT_TYPE_H__ */
