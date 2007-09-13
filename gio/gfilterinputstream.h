#ifndef __G_FILTER_INPUT_STREAM_H__
#define __G_FILTER_INPUT_STREAM_H__

#include <glib-object.h>
#include <gio/ginputstream.h>

G_BEGIN_DECLS

#define G_TYPE_FILTER_INPUT_STREAM         (g_filter_input_stream_get_type ())
#define G_FILTER_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILTER_INPUT_STREAM, GFilterInputStream))
#define G_FILTER_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILTER_INPUT_STREAM, GFilterInputStreamClass))
#define G_IS_FILTER_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILTER_INPUT_STREAM))
#define G_IS_FILTER_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILTER_INPUT_STREAM))
#define G_FILTER_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILTER_INPUT_STREAM, GFilterInputStreamClass))

typedef struct _GFilterInputStream         GFilterInputStream;
typedef struct _GFilterInputStreamClass    GFilterInputStreamClass;
typedef struct _GFilterInputStreamPrivate  GFilterInputStreamPrivate;

struct _GFilterInputStream
{
  GInputStream parent;

  /*<protected >*/
  GInputStream *base_stream;
};

struct _GFilterInputStreamClass
{
 GInputStreamClass parent_class;
};


GType          g_filter_input_stream_get_type  (void) G_GNUC_CONST;
GInputStream  *g_filter_input_stream_get_base_stream (GFilterInputStream *stream);
G_END_DECLS

#endif /* __G_FILTER_INPUT_STREAM_H__ */
