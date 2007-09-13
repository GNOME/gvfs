#ifndef __G_FILE_ENUMERATOR_H__
#define __G_FILE_ENUMERATOR_H__

#include <glib-object.h>
#include <gvfstypes.h>
#include <gvfserror.h>
#include <gfileinfo.h>

G_BEGIN_DECLS


#define G_TYPE_FILE_ENUMERATOR         (g_file_enumerator_get_type ())
#define G_FILE_ENUMERATOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_ENUMERATOR, GFileEnumerator))
#define G_FILE_ENUMERATOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_ENUMERATOR, GFileEnumeratorClass))
#define G_IS_FILE_ENUMERATOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_ENUMERATOR))
#define G_IS_FILE_ENUMERATOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_ENUMERATOR))
#define G_FILE_ENUMERATOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_ENUMERATOR, GFileEnumeratorClass))


typedef struct _GFileEnumerator         GFileEnumerator;
typedef struct _GFileEnumeratorClass    GFileEnumeratorClass;
typedef struct _GFileEnumeratorPrivate  GFileEnumeratorPrivate;

/**
 * GAsyncNextFilesCallback:
 * @enumerator: a #GFileEnumerator
 * @files: list of #GFileInfo objects
 * @num_files: size of @files list, or -1 on error
 * @data: the @data pointer passed to the async call
 * @error: the error, if num_files is -1, otherwise %NULL
 *
 * This callback is called when an asychronous next_file operation
 * is finished. When there are no more files to enumerate @num_files
 * is set to 0.
 *
 * The callback is always called, even if the operation was cancelled.
 * If the operation was cancelled @result will be %FALSE, and @error
 * will be %G_VFS_ERROR_CANCELLED.
 **/
typedef void (*GAsyncNextFilesCallback) (GFileEnumerator *enumerator,
					 GList *files,
					 int num_files,
					 gpointer data,
					 GError *error);

typedef void (*GAsyncStopEnumeratingCallback) (GFileEnumerator *enumerator,
					       gboolean result,
					       gpointer data,
					       GError *error);


struct _GFileEnumerator
{
  GObject parent;
  
  /*< private >*/
  GFileEnumeratorPrivate *priv;
};

struct _GFileEnumeratorClass
{
  GObjectClass parent_class;

  /* Virtual Table */

  GFileInfo *(*next_file)        (GFileEnumerator              *enumerator,
				  GError                      **error);
  gboolean   (*stop)             (GFileEnumerator              *enumerator,
				  GError                      **error);

  void       (*next_files_async) (GFileEnumerator              *enumerator,
				  int                           num_files,
				  int                           io_priority,
				  GAsyncNextFilesCallback       callback,
				  gpointer                      data,
				  GDestroyNotify                notify);
  void       (*stop_async)       (GFileEnumerator              *enumerator,
				  int                           io_priority,
				  GAsyncStopEnumeratingCallback callback,
				  gpointer                      data,
				  GDestroyNotify                notify);
  void       (*cancel)           (GFileEnumerator              *enumerator);

  /* Optional cancel wakeup if using default async ops */
  void     (* cancel_sync)      (GFileEnumerator        *enumerator);
  
};


GType g_file_enumerator_get_type (void) G_GNUC_CONST;

GFileInfo *   g_file_enumerator_next_file         (GFileEnumerator                *enumerator,
						   GError                        **error);
gboolean      g_file_enumerator_stop              (GFileEnumerator                *enumerator,
						   GError                        **error);
void          g_file_enumerator_set_async_context (GFileEnumerator                *enumerator,
						   GMainContext                   *context);
GMainContext *g_file_enumerator_get_async_context (GFileEnumerator                *enumerator);
void          g_file_enumerator_next_files_async  (GFileEnumerator                *enumerator,
						   int                             num_files,
						   int                             io_priority,
						   GAsyncNextFilesCallback         callback,
						   gpointer                        data,
						   GDestroyNotify                  notify);
void          g_file_enumerator_stop_async        (GFileEnumerator                *enumerator,
						   int                             io_priority,
						   GAsyncStopEnumeratingCallback   callback,
						   gpointer                        data,
						   GDestroyNotify                  notify);
void          g_file_enumerator_cancel            (GFileEnumerator                *enumerator);
gboolean      g_file_enumerator_is_cancelled      (GFileEnumerator                *enumerator);
gboolean      g_file_enumerator_is_stopped        (GFileEnumerator                *enumerator);
gboolean      g_file_enumerator_has_pending       (GFileEnumerator                *enumerator);
void          g_file_enumerator_set_pending       (GFileEnumerator                *enumerator,
						   gboolean                        pending);

G_END_DECLS

#endif /* __G_FILE_ENUMERATOR_H__ */
