#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "gsimpleasyncresult.h"
#include <glib/gi18n-lib.h>

static void g_simple_async_result_async_result_iface_init (GAsyncResultIface       *iface);

struct _GSimpleAsyncResult
{
  GObject parent_instance;

  GObject *source_object;
  gpointer user_data;
  GError *error;
  gboolean failed;
  
  gpointer op_data;
  GDestroyNotify destroy_op_data;
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

  if (simple->destroy_op_data)
    simple->destroy_op_data (simple->op_data);

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
}


GAsyncResult *
g_simple_async_result_new (void)
{
  GSimpleAsyncResult *simple;

  simple = g_object_new (G_TYPE_SIMPLE_ASYNC_RESULT, NULL);
  
  return G_ASYNC_RESULT (simple);
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


gpointer
g_simple_async_result_get_op_data (GSimpleAsyncResult *simple)
{
  return simple->op_data;
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
g_simple_async_result_set_from_error (GSimpleAsyncResult *simple,
				      GError *error)
{
  simple->error = error;
  simple->failed = TRUE;
}

static GError* 
_g_error_new_valist (GQuark         domain,
                    gint           code,
                    const gchar   *format,
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
g_simple_async_result_set_error (GSimpleAsyncResult *simple,
				 GQuark         domain,
				 gint           code,
				 const gchar   *format,
				 ...)
{
  va_list args;

  va_start (args, format);
  simple->error = _g_error_new_valist (domain, code, format, args);
  va_end (args);
  
  simple->failed = TRUE;
}


void
g_simple_async_result_complete_in_idle (GSimpleAsyncResult *simple)
{
  /* TODO */
}

void
g_simple_async_result_run_in_thread (GSimpleAsyncResult *simple,
				     GCallback callback)
{
}
