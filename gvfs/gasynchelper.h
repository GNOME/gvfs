#ifndef __G_ASYNC_HELPER_H__
#define __G_ASYNC_HELPER_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct {
  gpointer       async_object;
  GError *       error;
  gpointer       data;
  GDestroyNotify destroy_notify;
} GAsyncResult;

void
_g_queue_async_result (GAsyncResult   *result,
		       gpointer        async_object,
		       GError         *error,
		       gpointer        data,
		       GDestroyNotify  destroy_notify,
		       GMainContext   *context,
		       GSourceFunc     source_func);
 

G_END_DECLS

#endif /* __G_ASYNC_HELPER_H__ */
