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
 * @files: array of #GFileInfo objects
 * @num_files: size of @files array, or -1 on error
 * @has_more_files: %TRUE if there are more files in the enumerator
 * @error: the error, if num_files is -1, otherwise %NULL
 *
 * This callback is called when an asychronous close operation
 * is finished. 
 *
 * The callback is always called, even if the operation was cancelled.
 * If the operation was cancelled @result will be %FALSE, and @error
 * will be %G_VFS_ERROR_CANCELLED.
 **/
typedef void (*GAsyncNextFilesCallback) (GFileEnumerator *enumerator,
					 GFileInfo **files,
					 int num_files,
					 gboolean has_more_files,
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

  gboolean   (*has_more_files)   (GFileEnumerator       *enumerator);
  GFileInfo *(*next_file)        (GFileEnumerator       *enumerator,
				  GError               **error);
  void       (*stop)             (GFileEnumerator        *enumerator);

  guint      (*next_files_async) (GFileEnumerator        *enumerator,
				  int                     num_files,
				  int                     io_priority,
				  GMainContext           *context,
				  GAsyncNextFilesCallback callback,
				  gpointer                data,
				  GDestroyNotify          notify);
  void       (* cancel)          (GFileEnumerator        *enumerator,
				  guint                   tag);
};


GType g_file_enumerator_get_type (void) G_GNUC_CONST;

gboolean   g_file_enumerator_has_more_files          (GFileEnumerator          *enumerator);
GFileInfo *g_file_enumerator_next_file               (GFileEnumerator          *enumerator,
						      GError                  **error);
void       g_file_enumerator_stop                    (GFileEnumerator          *enumerator);
guint      g_file_enumerator_request_next_files      (GFileEnumerator          *enumerator,
						      int                       num_files,
						      GAsyncNextFilesCallback   callback,
						      gpointer                  data,
						      GDestroyNotify            notify);
guint      g_file_enumerator_request_next_files_full (GFileEnumerator          *enumerator,
						      int                       num_files,
						      int                       io_priority,
						      GMainContext             *context,
						      GAsyncNextFilesCallback   callback,
						      gpointer                  data,
						      GDestroyNotify            notify);
void       g_file_enumerator_cancel                  (GFileEnumerator          *enumerator,
						      guint                     tag);

G_END_DECLS

#endif /* __G_FILE_ENUMERATOR_H__ */
