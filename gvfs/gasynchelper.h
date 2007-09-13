#ifndef __G_ASYNC_HELPER_H__
#define __G_ASYNC_HELPER_H__

#include <glib-object.h>
#include "gcancellable.h"

G_BEGIN_DECLS

typedef struct {
  gpointer       async_object;
  GError *       error;
  gpointer       data;
} GAsyncResult;

typedef void (*GFDSourceFunc) (gpointer data,
			       GIOCondition condition,
			       int fd);

void     _g_queue_async_result (GAsyncResult    *result,
				gpointer         async_object,
				GError          *error,
				gpointer         data,
				GMainContext    *context,
				GSourceFunc      source_func);

GSource *_g_fd_source_new      (int              fd,
				gushort          events,
				GMainContext    *context,
				GCancellable    *cancellable);

G_END_DECLS

#endif /* __G_ASYNC_HELPER_H__ */
