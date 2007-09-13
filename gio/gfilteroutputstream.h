#ifndef __G_FILTER_OUTPUT_STREAM_H__
#define __G_FILTER_OUTPUT_STREAM_H__

#include <glib-object.h>
#include <gio/goutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_FILTER_OUTPUT_STREAM         (g_filter_output_stream_get_type ())
#define G_FILTER_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILTER_OUTPUT_STREAM, GFilterOutputStream))
#define G_FILTER_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILTER_OUTPUT_STREAM, GFilterOutputStreamClass))
#define G_IS_FILTER_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILTER_OUTPUT_STREAM))
#define G_IS_FILTER_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILTER_OUTPUT_STREAM))
#define G_FILTER_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILTER_OUTPUT_STREAM, GFilterOutputStreamClass))

typedef struct _GFilterOutputStream         GFilterOutputStream;
typedef struct _GFilterOutputStreamClass    GFilterOutputStreamClass;
typedef struct _GFilterOutputStreamPrivate  GFilterOutputStreamPrivate;

struct _GFilterOutputStream
{
  GOutputStream parent;

  /*< protected >*/
  GOutputStream *base_stream;
};

struct _GFilterOutputStreamClass
{
 GOutputStreamClass parent_class;
};


GType           g_filter_output_stream_get_type  (void) G_GNUC_CONST;
GOutputStream  *g_filter_output_stream_get_base_stream (GFilterOutputStream *stream);
G_END_DECLS

#endif /* __G_FILTER_OUTPUT_STREAM_H__ */
