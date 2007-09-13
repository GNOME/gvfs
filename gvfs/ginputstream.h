#ifndef __G_INPUT_STREAM_H__
#define __G_INPUT_STREAM_H__

#include <glib-object.h>
#include <gvfs/gvfstypes.h>
#include <gvfs/gvfserror.h>

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
 * On error @count_read is set to -1, and @error is set accordingly.
 * 
 * The callback is always called, even if the operation was cancelled.
 * If the operation was cancelled @count_read will be -1, and @error
 * will be %G_VFS_ERROR_CANCELLED.
 **/
typedef void (*GAsyncReadCallback)  (GInputStream *stream,
				     void         *buffer,
				     gsize         count_requested,
				     gssize        count_read,
				     gpointer      data,
				     GError       *error);

typedef void (*GAsyncSkipCallback)  (GInputStream *stream,
				     gsize         count_requested,
				     gssize        count_skipped,
				     gpointer      data,
				     GError       *error);

/**
 * GAsyncCloseInputCallback:
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
typedef void (*GAsyncCloseInputCallback)  (GInputStream *stream,
					   gboolean      result,
					   gpointer      data,
					   GError       *error);

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
  void    (* read_async)  (GInputStream        *stream,
			   void               *buffer,
			   gsize               count,
			   int                 io_priority,
			   GAsyncReadCallback  callback,
			   gpointer            data,
			   GDestroyNotify      notify);
  void    (* skip_async)  (GInputStream        *stream,
			   gsize               count,
			   int                 io_priority,
			   GAsyncSkipCallback  callback,
			   gpointer            data,
			   GDestroyNotify      notify);
  void    (* close_async) (GInputStream        *stream,
			   int                  io_priority,
			   GAsyncCloseInputCallback callback,
			   gpointer            data,
			   GDestroyNotify      notify);
  void     (* cancel)     (GInputStream       *stream);

  /* Optional cancel wakeup if using default async ops */
  void     (* cancel_sync) (GInputStream  *stream);

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType g_input_stream_get_type (void) G_GNUC_CONST;

gssize        g_input_stream_read              (GInputStream              *stream,
						void                      *buffer,
						gsize                      count,
						GError                   **error);
gssize        g_input_stream_read_all          (GInputStream              *stream,
						void                      *buffer,
						gsize                      count,
						GError                   **error);
gssize        g_input_stream_skip              (GInputStream              *stream,
						gsize                      count,
						GError                   **error);
gboolean      g_input_stream_close             (GInputStream              *stream,
						GError                   **error);
void          g_input_stream_set_async_context (GInputStream              *stream,
						GMainContext              *context);
GMainContext *g_input_stream_get_async_context (GInputStream              *stream);
void          g_input_stream_read_async        (GInputStream              *stream,
						void                      *buffer,
						gsize                      count,
						int                        io_priority,
						GAsyncReadCallback         callback,
						gpointer                   data,
						GDestroyNotify             notify);
void          g_input_stream_skip_async        (GInputStream              *stream,
						gsize                      count,
						int                        io_priority,
						GAsyncSkipCallback         callback,
						gpointer                   data,
						GDestroyNotify             notify);
void          g_input_stream_close_async       (GInputStream              *stream,
						int                        io_priority,
						GAsyncCloseInputCallback   callback,
						gpointer                   data,
						GDestroyNotify             notify);
void          g_input_stream_cancel            (GInputStream              *stream);
gboolean      g_input_stream_is_cancelled      (GInputStream              *stream);
gboolean      g_input_stream_is_closed         (GInputStream              *stream);
gboolean      g_input_stream_has_pending       (GInputStream              *stream);
void          g_input_stream_set_pending       (GInputStream              *stream,
						gboolean                   pending);


G_END_DECLS

#endif /* __G_INPUT_STREAM_H__ */
