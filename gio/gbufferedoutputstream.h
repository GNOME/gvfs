#ifndef __G_BUFFERED_OUTPUT_STREAM_H__
#define __G_BUFFERED_OUTPUT_STREAM_H__

#include <glib-object.h>
#include <gio/gfilteroutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_BUFFERED_OUTPUT_STREAM         (g_buffered_output_stream_get_type ())
#define G_BUFFERED_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_BUFFERED_OUTPUT_STREAM, GBufferedOutputStream))
#define G_BUFFERED_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_BUFFERED_OUTPUT_STREAM, GBufferedOutputStreamClass))
#define G_IS_BUFFERED_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_BUFFERED_OUTPUT_STREAM))
#define G_IS_BUFFERED_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_BUFFERED_OUTPUT_STREAM))
#define G_BUFFERED_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_BUFFERED_OUTPUT_STREAM, GBufferedOutputStreamClass))

typedef struct _GBufferedOutputStream         GBufferedOutputStream;
typedef struct _GBufferedOutputStreamClass    GBufferedOutputStreamClass;
typedef struct _GBufferedOutputStreamPrivate  GBufferedOutputStreamPrivate;

struct _GBufferedOutputStream
{
  GFilterOutputStream parent;

  /*< protected >*/
  GBufferedOutputStreamPrivate *priv;
};

struct _GBufferedOutputStreamClass
{
 GOutputStreamClass parent_class;
};


GType           g_buffered_output_stream_get_type  (void) G_GNUC_CONST;
GOutputStream*  g_buffered_output_stream_new       (GOutputStream *base_stream);
GOutputStream*  g_buffered_output_stream_new_sized (GOutputStream *base_stream,
						    guint          size);
G_END_DECLS

#endif /* __G_BUFFERED_OUTPUT_STREAM_H__ */
