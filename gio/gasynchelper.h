#ifndef __G_ASYNC_HELPER_H__
#define __G_ASYNC_HELPER_H__

#include <glib-object.h>
#include "gcancellable.h"

G_BEGIN_DECLS

typedef struct {
  gpointer       async_object;
  GError *       error;
  gpointer       user_data;
} GAsyncResultData;

typedef gboolean (*GFDSourceFunc) (gpointer user_data,
				   GIOCondition condition,
				   int fd);

void     _g_queue_async_result (GAsyncResultData *result,
				gpointer         async_object,
				GError          *error,
				gpointer         user_data,
				GSourceFunc      source_func);

GSource *_g_fd_source_new      (int              fd,
				gushort          events,
				GCancellable    *cancellable);

G_END_DECLS

#endif /* __G_ASYNC_HELPER_H__ */
