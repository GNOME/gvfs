
#ifndef __GVFS_DOCUMENT_OUTPUT_STREAM_H__
#define __GVFS_DOCUMENT_OUTPUT_STREAM_H__

#include <gio/gfileoutputstream.h>

G_BEGIN_DECLS

#define GVFS_TYPE_DOCUMENT_OUTPUT_STREAM         (_gvfs_document_output_stream_get_type ())
#define GVFS_DOCUMENT_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GVFS_TYPE_DOCUMENT_OUTPUT_STREAM, GVfsDocumentOutputStream))
#define GVFS_DOCUMENT_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), GVFS_TYPE_DOCUMENT_OUTPUT_STREAM, GVfsDocumentOutputStreamClass))
#define GVFS_IS_DOCUMENT_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GVFS_TYPE_DOCUMENT_OUTPUT_STREAM))
#define GVFS_IS_DOCUMENT_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), GVFS_TYPE_DOCUMENT_OUTPUT_STREAM))
#define GVFS_DOCUMENT_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), GVFS_TYPE_DOCUMENT_OUTPUT_STREAM, GVfsDocumentOutputStreamClass))

typedef struct _GVfsDocumentOutputStream         GVfsDocumentOutputStream;
typedef struct _GVfsDocumentOutputStreamClass    GVfsDocumentOutputStreamClass;
typedef struct _GVfsDocumentOutputStreamPrivate  GVfsDocumentOutputStreamPrivate;

struct _GVfsDocumentOutputStream
{
  GFileOutputStream parent_instance;

  /*< private >*/
  GVfsDocumentOutputStreamPrivate *priv;
};

struct _GVfsDocumentOutputStreamClass
{
  GFileOutputStreamClass parent_class;
};

GType    gvfs_document_output_stream_get_type (void) G_GNUC_CONST;

void     gvfs_document_output_stream_set_do_close (GVfsDocumentOutputStream *out,
						   gboolean do_close);
gboolean gvfs_document_output_stream_really_close (GVfsDocumentOutputStream *out,
						   GCancellable   *cancellable,
						   GError        **error);

GFileOutputStream * gvfs_document_output_stream_new  (const char *handle,
						      guint32   id,
						      int       fd);

G_END_DECLS

#endif /* __GVFS_DOCUMENT_OUTPUT_STREAM_H__ */
