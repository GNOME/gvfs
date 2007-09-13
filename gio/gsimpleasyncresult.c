#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "gsimpleasyncresult.h"
#include "gioscheduler.h"
#include <gio/gioerror.h>
#include <glib/gi18n-lib.h>

static void g_simple_async_result_async_result_iface_init (GAsyncResultIface       *iface);

struct _GSimpleAsyncResult
{
  GObject parent_instance;

  GObject *source_object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GError *error;
  gboolean failed;
  gboolean handle_cancellation;

  gpointer source_tag;

  union {
    gpointer v_pointer;
    gboolean v_boolean;
    gssize   v_ssize;
  } op_res;

  GDestroyNotify destroy_op_res;
};

G_DEFINE_TYPE_WITH_CODE (GSimpleAsyncResult, g_simple_async_result, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_RESULT,
						g_simple_async_result_async_result_iface_init))

static void
g_simple_async_result_finalize (GObject *object)
{
  GSimpleAsyncResult *simple;

  simple = G_SIMPLE_ASYNC_RESULT (object);

  if (simple->source_object)
    g_object_unref (simple->source_object);

  if (simple->destroy_op_res)
    simple->destroy_op_res (simple->op_res.v_pointer);

  if (simple->error)
    g_error_free (simple->error);
  
  if (G_OBJECT_CLASS (g_simple_async_result_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_simple_async_result_parent_class)->finalize) (object);
}

static void
g_simple_async_result_class_init (GSimpleAsyncResultClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_simple_async_result_finalize;
}

static void
g_simple_async_result_init (GSimpleAsyncResult *simple)
{
  simple->handle_cancellation = TRUE;
}

GSimpleAsyncResult *
g_simple_async_result_new (GObject *source_object,
			   GAsyncReadyCallback callback,
			   gpointer user_data,
			   gpointer source_tag)
{
  GSimpleAsyncResult *simple;

  simple = g_object_new (G_TYPE_SIMPLE_ASYNC_RESULT, NULL);
  simple->callback = callback;
  simple->source_object = g_object_ref (source_object);
  simple->user_data = user_data;
  simple->source_tag = source_tag;
  
  return simple;
}

GSimpleAsyncResult *
g_simple_async_result_new_from_error (GObject *source_object,
				      GAsyncReadyCallback callback,
				      gpointer user_data,
				      GError *error)
{
  GSimpleAsyncResult *simple;

  simple = g_simple_async_result_new (source_object,
				      callback,
				      user_data, NULL);
  g_simple_async_result_set_from_error (simple, error);

  return simple;
}

GSimpleAsyncResult *
g_simple_async_result_new_error (GObject *source_object,
				 GAsyncReadyCallback callback,
				 gpointer user_data,
				 GQuark         domain,
				 gint           code,
				 const char    *format,
				 ...)
{
  GSimpleAsyncResult *simple;
  va_list args;

  simple = g_simple_async_result_new (source_object,
				      callback,
				      user_data, NULL);

  va_start (args, format);
  g_simple_async_result_set_error_va (simple, domain, code, format, args);
  va_end (args);
  
  return simple;
}


static gpointer
g_simple_async_result_get_user_data (GAsyncResult *res)
{
  return G_SIMPLE_ASYNC_RESULT (res)->user_data;
}

static GObject *
g_simple_async_result_get_source_object (GAsyncResult *res)
{
  if (G_SIMPLE_ASYNC_RESULT (res)->source_object)
    return g_object_ref (G_SIMPLE_ASYNC_RESULT (res)->source_object);
  return NULL;
}

static void
g_simple_async_result_async_result_iface_init (GAsyncResultIface *iface)
{
  iface->get_user_data = g_simple_async_result_get_user_data;
  iface->get_source_object = g_simple_async_result_get_source_object;
}


void
g_simple_async_result_set_handle_cancellation (GSimpleAsyncResult *simple,
					       gboolean handle_cancellation)
{
  simple->handle_cancellation = handle_cancellation;
}

gpointer
g_simple_async_result_get_source_tag (GSimpleAsyncResult *simple)
{
  return simple->source_tag;
}

gboolean
g_simple_async_result_propagate_error (GSimpleAsyncResult *simple,
				       GError **dest)
{
  if (simple->failed)
    {
      g_propagate_error (dest, simple->error);
      simple->error = NULL;
      return TRUE;
    }
  return FALSE;
}

void
g_simple_async_result_set_op_res_gpointer (GSimpleAsyncResult      *simple,
                                           gpointer                 op_res,
                                           GDestroyNotify           destroy_op_res)
{
  simple->op_res.v_pointer = op_res;
  simple->destroy_op_res = destroy_op_res;
}

gpointer
g_simple_async_result_get_op_res_gpointer (GSimpleAsyncResult      *simple)
{
  return simple->op_res.v_pointer;
}

void
g_simple_async_result_set_op_res_gssize   (GSimpleAsyncResult      *simple,
                                           gssize                   op_res)
{
  simple->op_res.v_ssize = op_res;
}

gssize
g_simple_async_result_get_op_res_gssize   (GSimpleAsyncResult      *simple)
{
  return simple->op_res.v_ssize;
}

void
g_simple_async_result_set_op_res_gboolean (GSimpleAsyncResult      *simple,
                                           gboolean                 op_res)
{
  simple->op_res.v_boolean = op_res;
}

gboolean
g_simple_async_result_get_op_res_gboolean (GSimpleAsyncResult      *simple)
{
  return simple->op_res.v_boolean;
}

void
g_simple_async_result_set_from_error (GSimpleAsyncResult *simple,
				      GError *error)
{
  simple->error = g_error_copy (error);
  simple->failed = TRUE;
}

static GError* 
_g_error_new_valist (GQuark         domain,
                    gint           code,
                    const char    *format,
                    va_list        args)
{
  GError *error;
  
  error = g_new (GError, 1);
  
  error->domain = domain;
  error->code = code;
  error->message = g_strdup_vprintf (format, args);
  
  return error;
}

void
g_simple_async_result_set_error_va (GSimpleAsyncResult *simple,
				    GQuark         domain,
				    gint           code,
				    const char    *format,
				    va_list        args)
{
  simple->error = _g_error_new_valist (domain, code, format, args);
  simple->failed = TRUE;
}


void
g_simple_async_result_set_error (GSimpleAsyncResult *simple,
				 GQuark         domain,
				 gint           code,
				 const char    *format,
				 ...)
{
  va_list args;

  va_start (args, format);
  g_simple_async_result_set_error_va (simple, domain, code, format, args);
  va_end (args);
}


void
g_simple_async_result_complete (GSimpleAsyncResult *simple)
{
  if (simple->callback)
    simple->callback (simple->source_object,
		      G_ASYNC_RESULT (simple),
		      simple->user_data);
}

static gboolean
complete_in_idle_cb (gpointer data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (data);

  g_simple_async_result_complete (simple);

  return FALSE;
}

void
g_simple_async_result_complete_in_idle (GSimpleAsyncResult *simple)
{
  GSource *source;
  guint id;
  
  g_object_ref (simple);
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, complete_in_idle_cb, simple, g_object_unref);

  id = g_source_attach (source, NULL);
  g_source_unref (source);
}

typedef struct {
  GSimpleAsyncResult *simple;
  GSimpleAsyncThreadFunc func;
} RunInThreadData;

static void
run_in_thread (GIOJob *job,
	       GCancellable *c,
	       gpointer _data)
{
  RunInThreadData *data = _data;
  GSimpleAsyncResult *simple = data->simple;

  if (simple->handle_cancellation &&
      g_cancellable_is_cancelled (c))
    {
       g_simple_async_result_set_error (simple,
					G_IO_ERROR,
					G_IO_ERROR_CANCELLED,
                                       _("Operation was cancelled"));
    }
  else
    {
      data->func (simple,
		  simple->source_object,
		  c);
    }

  g_simple_async_result_complete_in_idle (data->simple);
  g_object_unref (data->simple);
  g_free (data);
}

void
g_simple_async_result_run_in_thread (GSimpleAsyncResult *simple,
				     GSimpleAsyncThreadFunc func,
				     int io_priority, 
				     GCancellable *cancellable)
{
  RunInThreadData *data;

  data = g_new (RunInThreadData, 1);
  data->func = func;
  data->simple = g_object_ref (simple);
  g_schedule_io_job (run_in_thread, data, NULL, io_priority, cancellable);
}

void
g_simple_async_report_error_in_idle (GObject *object,
				     GAsyncReadyCallback callback,
				     gpointer user_data,
				     GQuark         domain,
				     gint           code,
				     const char    *format,
				     ...)
{
  GSimpleAsyncResult *simple;
  va_list args;

  simple = g_simple_async_result_new (object,
				      callback,
				      user_data, NULL);

  va_start (args, format);
  g_simple_async_result_set_error_va (simple, domain, code, format, args);
  va_end (args);
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
}
