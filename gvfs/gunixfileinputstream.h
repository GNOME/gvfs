#ifndef __G_UNIX_FILE_INPUT_STREAM_H__
#define __G_UNIX_FILE_INPUT_STREAM_H__

#include <gvfs/gfileinputstream.h>

G_BEGIN_DECLS

#define G_TYPE_UNIX_FILE_INPUT_STREAM         (g_unix_file_input_stream_get_type ())
#define G_UNIX_FILE_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_UNIX_FILE_INPUT_STREAM, GUnixFileInputStream))
#define G_UNIX_FILE_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_UNIX_FILE_INPUT_STREAM, GUnixFileInputStreamClass))
#define G_IS_UNIX_FILE_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_UNIX_FILE_INPUT_STREAM))
#define G_IS_UNIX_FILE_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_UNIX_FILE_INPUT_STREAM))
#define G_UNIX_FILE_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_UNIX_FILE_INPUT_STREAM, GUnixFileInputStreamClass))

typedef struct _GUnixFileInputStream         GUnixFileInputStream;
typedef struct _GUnixFileInputStreamClass    GUnixFileInputStreamClass;
typedef struct _GUnixFileInputStreamPrivate  GUnixFileInputStreamPrivate;

struct _GUnixFileInputStream
{
  GFileInputStream parent;

  /*< private >*/
  GUnixFileInputStreamPrivate *priv;
};

struct _GUnixFileInputStreamClass
{
  GFileInputStreamClass parent_class;
};

GType g_unix_file_input_stream_get_type (void) G_GNUC_CONST;

GFileInputStream *g_unix_file_input_stream_new (const char *filename,
						const char *mountpoint);

G_END_DECLS

#endif /* __G_UNIX_FILE_INPUT_STREAM_H__ */
