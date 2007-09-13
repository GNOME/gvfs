#ifndef __G_MEMORY_OUTPUT_STREAM_H__
#define __G_MEMORY_OUTPUT_STREAM_H__

#include <glib-object.h>
#include <gio/goutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_MEMORY_OUTPUT_STREAM         (g_memory_output_stream_get_type ())
#define G_MEMORY_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MEMORY_OUTPUT_STREAM, GMemoryOutputStream))
#define G_MEMORY_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MEMORY_OUTPUT_STREAM, GMemoryOutputStreamClass))
#define G_IS_MEMORY_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MEMORY_OUTPUT_STREAM))
#define G_IS_MEMORY_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MEMORY_OUTPUT_STREAM))
#define G_MEMORY_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_MEMORY_OUTPUT_STREAM, GMemoryOutputStreamClass))

typedef struct _GMemoryOutputStream         GMemoryOutputStream;
typedef struct _GMemoryOutputStreamClass    GMemoryOutputStreamClass;
typedef struct _GMemoryOutputStreamPrivate  GMemoryOutputStreamPrivate;

struct _GMemoryOutputStream
{
  GOutputStream parent;

  /*< private >*/
  GMemoryOutputStreamPrivate *priv;
};

struct _GMemoryOutputStreamClass
{
 GOutputStreamClass parent_class;
};


GType           g_memory_output_stream_get_type     (void) G_GNUC_CONST;
GOutputStream * g_memory_output_stream_new          (GByteArray *data);
void            g_memory_output_stream_set_max_size (GMemoryOutputStream *ostream,
						     guint                max_size);

G_END_DECLS

#endif /* __G_MEMORY_OUTPUT_STREAM_H__ */
