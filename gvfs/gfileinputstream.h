#ifndef __G_FILE_INPUT_STREAM_H__
#define __G_FILE_INPUT_STREAM_H__

#include <gvfs/ginputstream.h>
#include <gvfs/gfileinfo.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_INPUT_STREAM         (g_file_input_stream_get_type ())
#define G_FILE_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_INPUT_STREAM, GFileInputStream))
#define G_FILE_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_INPUT_STREAM, GFileInputStreamClass))
#define G_IS_FILE_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_INPUT_STREAM))
#define G_IS_FILE_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_INPUT_STREAM))
#define G_FILE_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_INPUT_STREAM, GFileInputStreamClass))

typedef struct _GFileInputStream         GFileInputStream;
typedef struct _GFileInputStreamClass    GFileInputStreamClass;
typedef struct _GFileInputStreamPrivate  GFileInputStreamPrivate;

struct _GFileInputStream
{
  GInputStream parent;

  /*< private >*/
  GFileInputStreamPrivate *priv;
};

struct _GFileInputStreamClass
{
  GInputStreamClass parent_class;

  GFileInfo *(*get_file_info) (GFileInputStream     *stream,
			       GFileInfoRequestFlags requested,
			       char                 *attributes,
			       GCancellable         *cancellable,
			       GError              **error);
    
  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType g_file_input_stream_get_type (void) G_GNUC_CONST;

GFileInfo *g_file_input_stream_get_file_info (GFileInputStream     *stream,
					      GFileInfoRequestFlags requested,
					      char                 *attributes,
					      GCancellable         *cancellable,
					      GError              **error);

G_END_DECLS

#endif /* __G_FILE_FILE_INPUT_STREAM_H__ */
