#ifndef __G_SIMPLE_ASYNC_RESULT_H__
#define __G_SIMPLE_ASYNC_RESULT_H__

#include <gio/gasyncresult.h>

G_BEGIN_DECLS

#define G_TYPE_SIMPLE_ASYNC_RESULT         (g_simple_async_result_get_type ())
#define G_SIMPLE_ASYNC_RESULT(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_SIMPLE_ASYNC_RESULT, GSimpleAsyncResult))
#define G_SIMPLE_ASYNC_RESULT_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_SIMPLE_ASYNC_RESULT, GSimpleAsyncResultClass))
#define G_IS_SIMPLE_ASYNC_RESULT(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_SIMPLE_ASYNC_RESULT))
#define G_IS_SIMPLE_ASYNC_RESULT_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_SIMPLE_ASYNC_RESULT))
#define G_SIMPLE_ASYNC_RESULT_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_SIMPLE_ASYNC_RESULT, GSimpleAsyncResultClass))

typedef struct _GSimpleAsyncResult        GSimpleAsyncResult;
typedef struct _GSimpleAsyncResultClass   GSimpleAsyncResultClass;

struct _GSimpleAsyncResultClass
{
  GObjectClass parent_class;
};

GType g_simple_async_result_get_type (void) G_GNUC_CONST;
  
GAsyncResult * g_simple_async_result_new (void);

gpointer g_simple_async_result_get_op_data      (GSimpleAsyncResult  *simple);
void     g_simple_async_result_complete_in_idle (GSimpleAsyncResult  *simple);
void     g_simple_async_result_run_in_thread    (GSimpleAsyncResult  *simple,
						 GCallback            callback);
void     g_simple_async_result_set_error        (GSimpleAsyncResult  *simple,
						 GQuark               domain,
						 gint                 code,
						 const gchar         *format,
						 ...)  G_GNUC_PRINTF (4, 5);
void     g_simple_async_result_set_from_error   (GSimpleAsyncResult  *simple,
						 GError              *error);
gboolean g_simple_async_result_propagate_error  (GSimpleAsyncResult  *simple,
						 GError             **dest);


G_END_DECLS


#endif /* __G_SIMPLE_ASYNC_RESULT_H__ */
