#include <config.h>
#include "gfileenumerator.h"
#include <glib/gi18n-lib.h>
#include "gioscheduler.h"
#include "gasynchelper.h"

G_DEFINE_TYPE (GFileEnumerator, g_file_enumerator, G_TYPE_OBJECT);

struct _GFileEnumeratorPrivate {
  /* TODO: Should be public for subclasses? */
  guint stopped : 1;
  guint pending : 1;
  gpointer outstanding_callback;
  GError *outstanding_error;
};

static void g_file_enumerator_real_next_files_async (GFileEnumerator               *enumerator,
						     int                            num_files,
						     int                            io_priority,
						     GAsyncNextFilesCallback        callback,
						     gpointer                       data,
						     GCancellable                  *cancellable);
static void g_file_enumerator_real_stop_async       (GFileEnumerator               *enumerator,
						     int                            io_priority,
						     GAsyncStopEnumeratingCallback  callback,
						     gpointer                       data,
						     GCancellable                  *cancellable);

static void
g_file_enumerator_finalize (GObject *object)
{
  GFileEnumerator *enumerator;

  enumerator = G_FILE_ENUMERATOR (object);
  
  if (!enumerator->priv->stopped)
    g_file_enumerator_stop (enumerator, NULL, NULL);

  if (G_OBJECT_CLASS (g_file_enumerator_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_enumerator_parent_class)->finalize) (object);
}

static void
g_file_enumerator_class_init (GFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GFileEnumeratorPrivate));
  
  gobject_class->finalize = g_file_enumerator_finalize;

  klass->next_files_async = g_file_enumerator_real_next_files_async;
  klass->stop_async = g_file_enumerator_real_stop_async;
}

static void
g_file_enumerator_init (GFileEnumerator *enumerator)
{
  enumerator->priv = G_TYPE_INSTANCE_GET_PRIVATE (enumerator,
						  G_TYPE_FILE_ENUMERATOR,
						  GFileEnumeratorPrivate);
}

/**
 * g_file_enumerator_next_file:
 * @enumerator: a #GFileEnumerator.
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Returns information for the next file in the enumerated object.
 * Will block until the information is availible.
 *
 * On error, returns %NULL and sets @error to the error. If the
 * enumerator is at the end, %NULL will be returned and @error will
 * be unset.
 *
 * Return value: A GFileInfo or %NULL on error or end of enumerator
 **/
GFileInfo *
g_file_enumerator_next_file (GFileEnumerator *enumerator,
			     GCancellable *cancellable,
			     GError **error)
{
  GFileEnumeratorClass *class;
  GFileInfo *info;
  
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), NULL);
  g_return_val_if_fail (enumerator != NULL, NULL);
  
  if (enumerator->priv->stopped)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Enumerator is stopped"));
      return NULL;
    }

  if (enumerator->priv->pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      return NULL;
    }

  if (enumerator->priv->outstanding_error)
    {
      g_propagate_error (error, enumerator->priv->outstanding_error);
      enumerator->priv->outstanding_error = NULL;
      return NULL;
    }
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);

  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  enumerator->priv->pending = TRUE;
  info = (* class->next_file) (enumerator, cancellable, error);
  enumerator->priv->pending = FALSE;

  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  return info;
}
  
/**
 * g_file_enumerator_stop:
 * @enumerator: a #GFileEnumerator.
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Releases all resources used by this enumerator, making the
 * enumerator return %G_IO_ERROR_CLOSED on all calls.
 *
 * This will be automatically called when the last reference
 * is dropped, but you might want to call make sure resources
 * are released as early as possible.
 **/
gboolean
g_file_enumerator_stop (GFileEnumerator *enumerator,
			GCancellable *cancellable,
			GError **error)
{
  GFileEnumeratorClass *class;

  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), FALSE);
  g_return_val_if_fail (enumerator != NULL, FALSE);
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);

  if (enumerator->priv->stopped)
    return TRUE;
  
  if (enumerator->priv->pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      return FALSE;
    }

  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  enumerator->priv->pending = TRUE;
  (* class->stop) (enumerator, cancellable, error);
  enumerator->priv->pending = FALSE;
  enumerator->priv->stopped = TRUE;

  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  return TRUE;
}

typedef struct {
  GAsyncResult            generic;
  int                     num_files;
  GAsyncNextFilesCallback callback;
} NextAsyncResult;

static gboolean
call_next_async_result (gpointer data)
{
  NextAsyncResult *res = data;

  if (res->callback)
    res->callback (res->generic.async_object,
		   NULL,
		   res->num_files,
		   res->generic.data,
		   res->generic.error);

  return FALSE;
}

static void
queue_next_async_result (GFileEnumerator *enumerator,
			 int num_files,
			 GError *error,
			 GAsyncNextFilesCallback callback,
			 gpointer data)
{
  NextAsyncResult *res;

  res = g_new0 (NextAsyncResult, 1);

  res->num_files = num_files;
  res->callback = callback;
  
  _g_queue_async_result ((GAsyncResult *)res, enumerator,
			 error, data,
			 call_next_async_result);
}

static void
next_async_callback_wrapper (GFileEnumerator *enumerator,
			     GList *files,
			     int num_files,
			     gpointer data,
			     GError *error)
{
  GAsyncNextFilesCallback real_callback = enumerator->priv->outstanding_callback;

  enumerator->priv->pending = FALSE;
  (*real_callback) (enumerator, files, num_files, data, error);
  g_object_unref (enumerator);
}

/**
 * g_file_enumerator_next_files_async:
 * @enumerator: a #GFileEnumerator.
 * @num_file: the number of file info objects to request
 * @io_priority: the io priority of the request
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @cancellable: optional cancellable object
 *
 * Request information for a number of files from the enumerator asynchronously.
 * When all i/o for the operation is finished the @callback will be called with
 * the requested information.
 *
 * The callback can be called with less than @num_files files in case of error
 * or at the end of the enumerator. In case of a partial error the callback will
 * be called with any succeeding items and no error, and on the next request the
 * error will be reported. If a request is cancelled the callback will be called
 * with %G_IO_ERROR_CANCELLED.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_IO_ERROR_PENDING errors. 
 *
 * Any outstanding i/o request with higher priority (lower numerical value) will
 * be executed before an outstanding request with lower priority. Default
 * priority is %G_PRIORITY_DEFAULT.
 **/
void
g_file_enumerator_next_files_async (GFileEnumerator        *enumerator,
				    int                     num_files,
				    int                     io_priority,
				    GAsyncNextFilesCallback callback,
				    gpointer                data,
				    GCancellable           *cancellable)
{
  GFileEnumeratorClass *class;
  GError *error;

  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));
  g_return_if_fail (enumerator != NULL);
  g_return_if_fail (num_files > 0);

  if (num_files == 0)
    {
      queue_next_async_result (enumerator, 0, NULL,  callback, data);
      return;
    }
  
  if (enumerator->priv->stopped)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("File enumerator is already closed"));
      queue_next_async_result (enumerator, -1, error,
			       callback, data);
      return;
    }
  
  if (enumerator->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      queue_next_async_result (enumerator, -1, error,
			       callback, data);
      return;
    }

  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  
  enumerator->priv->pending = TRUE;
  enumerator->priv->outstanding_callback = callback;
  g_object_ref (enumerator);
  (* class->next_files_async) (enumerator, num_files, io_priority, 
			       next_async_callback_wrapper, data, cancellable);
}


typedef struct {
  GAsyncResult                  generic;
  gboolean                      result;
  GAsyncStopEnumeratingCallback callback;
} StopAsyncResult;

static gboolean
call_stop_async_result (gpointer data)
{
  StopAsyncResult *res = data;

  if (res->callback)
    res->callback (res->generic.async_object,
		   res->result,
		   res->generic.data,
		   res->generic.error);

  return FALSE;
}

static void
queue_stop_async_result (GFileEnumerator *enumerator,
			 gboolean result,
			 GError *error,
			 GAsyncStopEnumeratingCallback callback,
			 gpointer data)
{
  StopAsyncResult *res;

  res = g_new0 (StopAsyncResult, 1);

  res->result = result;
  res->callback = callback;

  _g_queue_async_result ((GAsyncResult *)res, enumerator,
			 error, data,
			 call_stop_async_result);
}

static void
stop_async_callback_wrapper (GFileEnumerator *enumerator,
			     gboolean result,
			     gpointer data,
			     GError *error)
{
  GAsyncStopEnumeratingCallback real_callback = enumerator->priv->outstanding_callback;

  enumerator->priv->pending = FALSE;
  enumerator->priv->stopped = TRUE;
  (*real_callback) (enumerator, result, data, error);
  g_object_unref (enumerator);
}

void
g_file_enumerator_stop_async (GFileEnumerator                *enumerator,
			      int                             io_priority,
			      GAsyncStopEnumeratingCallback   callback,
			      gpointer                        data,
			      GCancellable                   *cancellable)
{
  GFileEnumeratorClass *class;
  GError *error;

  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));

  if (enumerator->priv->stopped)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("File enumerator is already stopped"));
      queue_stop_async_result (enumerator, FALSE, error,
			       callback, data);
      return;
    }
  
  if (enumerator->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      queue_stop_async_result (enumerator, FALSE, error,
			       callback, data);
      return;
    }

  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  
  enumerator->priv->pending = TRUE;
  enumerator->priv->outstanding_callback = callback;
  g_object_ref (enumerator);
  (* class->stop_async) (enumerator, io_priority,
			 stop_async_callback_wrapper, data, cancellable);
}

gboolean
g_file_enumerator_is_stopped (GFileEnumerator *enumerator)
{
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), TRUE);
  g_return_val_if_fail (enumerator != NULL, TRUE);
  
  return enumerator->priv->stopped;
}
  
gboolean
g_file_enumerator_has_pending (GFileEnumerator *enumerator)
{
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), TRUE);
  g_return_val_if_fail (enumerator != NULL, TRUE);
  
  return enumerator->priv->pending;
}

void
g_file_enumerator_set_pending (GFileEnumerator              *enumerator,
			       gboolean                   pending)
{
  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));
  g_return_if_fail (enumerator != NULL);
  
  enumerator->priv->pending = pending;
}

typedef struct {
  GFileEnumerator   *enumerator;
  GError            *error;
  gpointer           data;
} FileEnumeratorOp;

static void
file_enumerator_op_free (gpointer data)
{
  FileEnumeratorOp *op = data;

  if (op->error)
    g_error_free (op->error);

  g_free (op);
}

typedef struct {
  FileEnumeratorOp  op;
  int                num_files;
  GList             *files;
  GAsyncNextFilesCallback callback;
} NextAsyncOp;

static void
next_op_report (gpointer data)
{
  NextAsyncOp *op = data;

  op->callback (op->op.enumerator,
		op->files,
		op->num_files,
		op->op.data,
		op->op.error);

  g_list_foreach (op->files, (GFunc)g_object_unref, NULL);
  g_list_free (op->files);
  op->files = NULL;
}

static void
next_op_func (GIOJob *job,
	      GCancellable *cancellable,
	      gpointer data)
{
  NextAsyncOp *op = data;
  GFileEnumeratorClass *class;
  GFileInfo *info;
  int i;

  class = G_FILE_ENUMERATOR_GET_CLASS (op->op.enumerator);

  for (i = 0; i < op->num_files; i++)
    {
      if (g_cancellable_is_cancelled (cancellable))
	{
	  info = NULL;
	  g_set_error (&op->op.error,
		       G_IO_ERROR,
		       G_IO_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	}
      else
	{
	  info = class->next_file (op->op.enumerator, cancellable, &op->op.error);
	}
      
      if (info == NULL)
	{
	  /* If we get an error after first file, return that on next operation */
	  if (op->op.error != NULL && i > 0)
	    {
	      op->op.enumerator->priv->outstanding_error = op->op.error;
	      op->op.error = NULL;
	    }
	      
	  break;
	}
    }
  
  if (op->op.error != NULL)
    op->num_files = -1;
  else
    op->num_files = i;

  g_io_job_send_to_mainloop (job, next_op_report,
			     op, file_enumerator_op_free,
			     FALSE);
}

static void
g_file_enumerator_real_next_files_async (GFileEnumerator              *enumerator,
					 int                           num_files,
					 int                           io_priority,
					 GAsyncNextFilesCallback       callback,
					 gpointer                      data,
					 GCancellable                 *cancellable)
{
  NextAsyncOp *op;

  op = g_new0 (NextAsyncOp, 1);

  op->op.enumerator = enumerator;
  op->num_files = num_files;
  op->files = NULL;
  op->callback = callback;
  op->op.data = data;
  
  g_schedule_io_job (next_op_func,
		     op,
		     NULL,
		     io_priority,
		     cancellable);
}

typedef struct {
  FileEnumeratorOp  op;
  gboolean result;
  GAsyncStopEnumeratingCallback callback;
} StopAsyncOp;

static void
stop_op_report (gpointer data)
{
  StopAsyncOp *op = data;

  op->callback (op->op.enumerator,
		op->result,
		op->op.data,
		op->op.error);
}

static void
stop_op_func (GIOJob *job,
	      GCancellable *cancellable,
	      gpointer data)
{
  StopAsyncOp *op = data;
  GFileEnumeratorClass *class;

  if (g_cancellable_is_cancelled (cancellable))
    {
      op->result = FALSE;
      g_set_error (&op->op.error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else 
    {
      class = G_FILE_ENUMERATOR_GET_CLASS (op->op.enumerator);
      op->result = class->stop (op->op.enumerator, cancellable, &op->op.error);
    }
  
  g_io_job_send_to_mainloop (job, stop_op_report,
			     op, file_enumerator_op_free,
			     FALSE);
}

static void
g_file_enumerator_real_stop_async (GFileEnumerator              *enumerator,
				   int                           io_priority,
				   GAsyncStopEnumeratingCallback callback,
				   gpointer                      data,
				   GCancellable                 *cancellable)
{
  StopAsyncOp *op;

  op = g_new0 (StopAsyncOp, 1);

  op->op.enumerator = enumerator;
  op->callback = callback;
  op->op.data = data;
  
  g_schedule_io_job (stop_op_func,
		     op,
		     NULL,
		     io_priority,
		     cancellable);
}
