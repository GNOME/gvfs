#ifndef __G_LOCAL_FILE_INPUT_STREAM_H__
#define __G_LOCAL_FILE_INPUT_STREAM_H__

#include <gvfs/gfileinputstream.h>

G_BEGIN_DECLS

#define G_TYPE_LOCAL_FILE_INPUT_STREAM         (g_local_file_input_stream_get_type ())
#define G_LOCAL_FILE_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_LOCAL_FILE_INPUT_STREAM, GLocalFileInputStream))
#define G_LOCAL_FILE_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_LOCAL_FILE_INPUT_STREAM, GLocalFileInputStreamClass))
#define G_IS_LOCAL_FILE_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_LOCAL_FILE_INPUT_STREAM))
#define G_IS_LOCAL_FILE_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_LOCAL_FILE_INPUT_STREAM))
#define G_LOCAL_FILE_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_LOCAL_FILE_INPUT_STREAM, GLocalFileInputStreamClass))

typedef struct _GLocalFileInputStream         GLocalFileInputStream;
typedef struct _GLocalFileInputStreamClass    GLocalFileInputStreamClass;
typedef struct _GLocalFileInputStreamPrivate  GLocalFileInputStreamPrivate;

struct _GLocalFileInputStream
{
  GFileInputStream parent;

  /*< private >*/
  GLocalFileInputStreamPrivate *priv;
};

struct _GLocalFileInputStreamClass
{
  GFileInputStreamClass parent_class;
};

GType g_local_file_input_stream_get_type (void) G_GNUC_CONST;

GFileInputStream *g_local_file_input_stream_new (const char *filename);

G_END_DECLS

#endif /* __G_LOCAL_FILE_INPUT_STREAM_H__ */
