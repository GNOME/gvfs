#ifndef __G_FILE_OUTPUT_STREAM_H__
#define __G_FILE_OUTPUT_STREAM_H__

#include <gio/goutputstream.h>
#include <gio/gfileinfo.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_OUTPUT_STREAM         (g_file_output_stream_get_type ())
#define G_FILE_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_OUTPUT_STREAM, GFileOutputStream))
#define G_FILE_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_OUTPUT_STREAM, GFileOutputStreamClass))
#define G_IS_FILE_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_OUTPUT_STREAM))
#define G_IS_FILE_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_OUTPUT_STREAM))
#define G_FILE_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_OUTPUT_STREAM, GFileOutputStreamClass))

typedef struct _GFileOutputStream         GFileOutputStream;
typedef struct _GFileOutputStreamClass    GFileOutputStreamClass;
typedef struct _GFileOutputStreamPrivate  GFileOutputStreamPrivate;

struct _GFileOutputStream
{
  GOutputStream parent;
  
  /*< private >*/
  GFileOutputStreamPrivate *priv;
};

struct _GFileOutputStreamClass
{
  GOutputStreamClass parent_class;

  goffset    (*tell)          (GFileOutputStream     *stream);
  gboolean   (*can_seek)      (GFileOutputStream     *stream);
  gboolean   (*seek)	      (GFileOutputStream     *stream,
			       goffset               offset,
			       GSeekType             type,
			       GCancellable         *cancellable,
			       GError              **error);
  gboolean   (*can_truncate)  (GFileOutputStream    *stream);
  gboolean   (*truncate)      (GFileOutputStream    *stream,
			       goffset               size,
			       GCancellable         *cancellable,
			       GError              **error);
  GFileInfo *(*get_file_info) (GFileOutputStream    *stream,
			       char                 *attributes,
			       GCancellable         *cancellable,
			       GError              **error);
  char      *(*get_etag)      (GFileOutputStream    *stream,
			       GCancellable         *cancellable,
			       GError              **error);
    
  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType g_file_output_stream_get_type (void) G_GNUC_CONST;


GFileInfo *g_file_output_stream_get_file_info       (GFileOutputStream  *stream,
						     char               *attributes,
						     GCancellable       *cancellable,
						     GError            **error);
char *     g_file_output_stream_get_etag            (GFileOutputStream  *stream,
						     GCancellable       *cancellable,
						     GError            **error);

G_END_DECLS

#endif /* __G_FILE_FILE_OUTPUT_STREAM_H__ */
