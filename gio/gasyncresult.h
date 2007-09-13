#ifndef __G_ASYNC_RESULT_H__
#define __G_ASYNC_RESULT_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define G_TYPE_ASYNC_RESULT            (g_async_result_get_type ())
#define G_ASYNC_RESULT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_ASYNC_RESULT, GAsyncResult))
#define G_IS_ASYNC_RESULT(obj)	       (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_ASYNC_RESULT))
#define G_ASYNC_RESULT_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_ASYNC_RESULT, GAsyncResultIface))

typedef struct _GAsyncResult         GAsyncResult; /* Dummy typedef */
typedef struct _GAsyncResultIface    GAsyncResultIface;

typedef void (*GAsyncReadyCallback) (GObject *source_object,
				     GAsyncResult *res,
				     gpointer user_data);

struct _GAsyncResultIface
{
  GTypeInterface g_iface;

  /* Virtual Table */

  gpointer   (*get_user_data)      (GAsyncResult                *async_result);
  GObject *  (*get_source_object)  (GAsyncResult                *async_result);
};

GType g_async_result_get_type (void) G_GNUC_CONST;

gpointer g_async_result_get_user_data     (GAsyncResult *res);
GObject *g_async_result_get_source_object (GAsyncResult *res);

G_END_DECLS

#endif /* __G_ASYNC_RESULT_H__ */
