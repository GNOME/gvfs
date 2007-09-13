#ifndef __G_INPUT_STREAM_H__
#define __G_INPUT_STREAM_H__

#include <glib-object.h>
#include <gvfstypes.h>
#include <gvfserror.h>

G_BEGIN_DECLS

#define G_TYPE_INPUT_STREAM         (g_input_stream_get_type ())
#define G_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_INPUT_STREAM, GInputStream))
#define G_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_INPUT_STREAM, GInputStreamClass))
#define G_IS_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_INPUT_STREAM))
#define G_IS_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_INPUT_STREAM))
#define G_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_INPUT_STREAM, GInputStreamClass))

typedef struct _GInputStream         GInputStream;
typedef struct _GInputStreamClass    GInputStreamClass;
typedef struct _GInputStreamPrivate  GInputStreamPrivate;

/**
 * GAsyncReadCallback:
 * @stream: a #GAsyncInputStream
 * @buffer: the buffer passed to the read call
 * @count_requested: the number of bytes requested to be read
 * @count_read: the number of bytes actually read
 * @data: the @data pointer passed to the read call
 * @error: the error, if count_read is -1, otherwise %NULL
 *
 * This callback is called when an asychronous read operation
 * is finished. 
 *
 * On success, the number of bytes read into the buffer is passed in @count_read.
 * It is not an error if this is not the same as the requested size, as it
 * can happen e.g. near the end of a file, but generally we try to read
 * as many bytes as requested. Zero is passed on end of file
 * (or if @count_requested is zero), but never otherwise.
 * 
 * The callback is always called, even if the operation was cancelled.
 * If the operation was cancelled @count_read will be -1, and @error
 * will be %G_VFS_ERROR_CANCELLED.
 **/
typedef void (*GAsyncReadCallback)  (GInputStream *stream,
				     void              *buffer,
				     gsize              count_requested,
				     gsize              count_read,
				     gpointer           data,
				     GError            *error);
/**
 * GAsyncCloseCallback:
 * @stream: a #GInputStream
 * @result: %TRUE on success, %FALSE otherwis
 * @error: the error, if result is %FALSE, otherwise %NULL
 *
 * This callback is called when an asychronous close operation
 * is finished. 
 *
 * The callback is always called, even if the operation was cancelled.
 * If the operation was cancelled @result will be %FALSE, and @error
 * will be %G_VFS_ERROR_CANCELLED.
 **/
typedef void (*GAsyncCloseCallback)  (GInputStream *stream,
				      gboolean           result,
				      GError            *error);

struct _GInputStream
{
  GObject parent;

  /*< private >*/
  GInputStreamPrivate *priv;
};

struct _GInputStreamClass
{
  GObjectClass parent_class;

  /* Sync ops: */
  
  gssize   (* read)        (GInputStream *stream,
			    void         *buffer,
			    gsize         count,
			    GError      **error);
  gssize   (* skip)        (GInputStream *stream,
			    gsize         count,
			    GError      **error);
  gboolean (* close)	   (GInputStream *stream,
			    GError      **error);

  /* Async ops: (optional in derived classes) */
  guint    (* read_async)  (GInputStream  *stream,
			    void               *buffer,
			    gsize               count,
			    int                 io_priority,
			    GAsyncReadCallback  callback,
			    gpointer            data,
			    GDestroyNotify      notify);
  guint    (* close_async) (GInputStream  *stream,
			    GAsyncCloseCallback callback,
			    gpointer            data,
			    GDestroyNotify      notify);
  void     (* cancel)      (GInputStream  *stream,
			    guint               tag);

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType g_input_stream_get_type (void) G_GNUC_CONST;

gssize        g_input_stream_read              (GInputStream         *stream,
						void                 *buffer,
						gsize                 count,
						GError              **error);
gssize        g_input_stream_skip              (GInputStream         *stream,
						gsize                 count,
						GError              **error);
gboolean      g_input_stream_close             (GInputStream         *stream,
						GError              **error);

void          g_input_stream_set_async_context (GInputStream         *stream,
						GMainContext         *context);
GMainContext *g_input_stream_get_async_context (GInputStream         *stream);
guint         g_input_stream_read_async        (GInputStream         *stream,
						void                 *buffer,
						gsize                 count,
						int                   io_priority,
						GAsyncReadCallback    callback,
						gpointer              data,
						GDestroyNotify        notify);
guint         g_input_stream_close_async       (GInputStream         *stream,
						GAsyncCloseCallback   callback,
						gpointer              data,
						GDestroyNotify        notify);
void          g_input_stream_cancel            (GInputStream         *stream,
						guint                 tag);


G_END_DECLS

#endif /* __G_INPUT_STREAM_H__ */
