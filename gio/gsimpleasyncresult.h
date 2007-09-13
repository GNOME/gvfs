#ifndef __G_SIMPLE_ASYNC_RESULT_H__
#define __G_SIMPLE_ASYNC_RESULT_H__

#include <gio/gasyncresult.h>
#include <gio/gcancellable.h>

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

typedef void (*GSimpleAsyncThreadFunc) (GSimpleAsyncResult *res,
					GObject *object,
					GCancellable *cancellable);


GType g_simple_async_result_get_type (void) G_GNUC_CONST;
  
GSimpleAsyncResult *g_simple_async_result_new              (GObject                 *source_object,
							    GAsyncReadyCallback      callback,
							    gpointer                 user_data,
							    gpointer                 source_tag);
GSimpleAsyncResult *g_simple_async_result_new_error        (GObject                 *source_object,
							    GAsyncReadyCallback      callback,
							    gpointer                 user_data,
							    GQuark                   domain,
							    gint                     code,
							    const gchar             *format,
							    ...) G_GNUC_PRINTF (6, 7);
GSimpleAsyncResult *g_simple_async_result_new_from_error   (GObject                 *source_object,
							    GAsyncReadyCallback      callback,
							    gpointer                 user_data,
							    GError                  *error);

void                g_simple_async_result_set_op_res_gpointer (GSimpleAsyncResult      *simple,
                                                               gpointer                 op_res,
                                                               GDestroyNotify           destroy_op_res);
gpointer            g_simple_async_result_get_op_res_gpointer (GSimpleAsyncResult      *simple);

void                g_simple_async_result_set_op_res_gssize   (GSimpleAsyncResult      *simple,
                                                               gssize                   op_res);
gssize              g_simple_async_result_get_op_res_gssize   (GSimpleAsyncResult      *simple);

void                g_simple_async_result_set_op_res_gboolean (GSimpleAsyncResult      *simple,
                                                               gboolean                 op_res);
gboolean            g_simple_async_result_get_op_res_gboolean (GSimpleAsyncResult      *simple);



gpointer            g_simple_async_result_get_source_tag   (GSimpleAsyncResult      *simple);
void                g_simple_async_result_set_handle_cancellation (GSimpleAsyncResult      *simple,
								   gboolean          handle_cancellation);
void                g_simple_async_result_complete         (GSimpleAsyncResult      *simple);
void                g_simple_async_result_complete_in_idle (GSimpleAsyncResult      *simple);
void                g_simple_async_result_run_in_thread    (GSimpleAsyncResult      *simple,
							    GSimpleAsyncThreadFunc   func,
							    int                      io_priority,
							    GCancellable            *cancellable);
void                g_simple_async_result_set_from_error   (GSimpleAsyncResult      *simple,
							    GError                  *error);
gboolean            g_simple_async_result_propagate_error  (GSimpleAsyncResult      *simple,
							    GError                 **dest);
void                g_simple_async_result_set_error        (GSimpleAsyncResult      *simple,
							    GQuark                   domain,
							    gint                     code,
							    const gchar             *format,
							    ...) G_GNUC_PRINTF (4, 5);
void                g_simple_async_result_set_error_va     (GSimpleAsyncResult      *simple,
							    GQuark                   domain,
							    gint                     code,
							    const gchar             *format,
							    va_list                  args);


G_END_DECLS


  
#endif /* __G_SIMPLE_ASYNC_RESULT_H__ */
