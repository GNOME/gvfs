#ifndef __G_OUTPUT_STREAM_H__
#define __G_OUTPUT_STREAM_H__

#include <glib-object.h>
#include <gvfstypes.h>
#include <ginputstream.h>

G_BEGIN_DECLS

#define G_TYPE_OUTPUT_STREAM         (g_output_stream_get_type ())
#define G_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_OUTPUT_STREAM, GOutputStream))
#define G_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_OUTPUT_STREAM, GOutputStreamClass))
#define G_IS_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_OUTPUT_STREAM))
#define G_IS_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_OUTPUT_STREAM))
#define G_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_OUTPUT_STREAM, GOutputStreamClass))

typedef struct _GOutputStream         GOutputStream;
typedef struct _GOutputStreamClass    GOutputStreamClass;
typedef struct _GOutputStreamPrivate  GOutputStreamPrivate;

typedef void (*GAsyncWriteCallback)  (GOutputStream *stream,
				      void               *buffer,
				      gssize             bytes_requested,
				      gssize             bytes_writen,
				      gpointer            data,
				      GError             *error);

struct _GOutputStream
{
  GObject parent;
  
  /*< private >*/
  GOutputStreamPrivate *priv;
};


struct _GOutputStreamClass
{
  GObjectClass parent_class;

  /* Sync ops: */
  
  gssize      (* write)  (GOutputStream *stream,
			  void *buffer,
			  gsize count,
			  GError **error);
  gboolean    (* flush)	 (GOutputStream *stream,
			  GError       **error);
  gboolean    (* close)	 (GOutputStream *stream,
			  GError       **error);

  /* Async ops: (optional in derived classes) */

  guint    (* write_async) (GOutputStream       *stream,
			    void                *buffer,
			    gsize                count,
			    int                  io_priority,
			    GAsyncWriteCallback  callback,
			    gpointer             data,
			    GDestroyNotify       notify);
  guint    (* close_async) (GOutputStream       *stream,
			    GAsyncCloseCallback  callback,
			    gpointer             data,
			    GDestroyNotify       notify);
  void     (* cancel)      (GOutputStream       *stream,
			    guint                tag);
};

GType g_output_stream_get_type (void) G_GNUC_CONST;
  

gssize        g_output_stream_write             (GOutputStream        *stream,
						 void                 *buffer,
						 gsize                 count,
						 GError              **error);
gboolean      g_output_stream_flush             (GOutputStream        *stream,
						 GError              **error);
gboolean      g_output_stream_close             (GOutputStream        *stream,
						 GError              **error);
void          g_output_stream_set_async_context (GOutputStream        *stream,
						 GMainContext         *context);
GMainContext *g_output_stream_get_async_context (GOutputStream        *stream);
guint         g_output_stream_write_async       (GOutputStream        *stream,
						 void                 *buffer,
						 gsize                 count,
						 int                   io_priority,
						 GAsyncWriteCallback   callback,
						 gpointer              data,
						 GDestroyNotify        notify);
guint         g_output_stream_close_async       (GOutputStream        *stream,
						 GAsyncCloseCallback   callback,
						 gpointer              data,
						 GDestroyNotify        notify);
void          g_output_stream_cancel            (GOutputStream        *stream,
						 guint                 tag);


G_END_DECLS

#endif /* __G_OUTPUT_STREAM_H__ */
