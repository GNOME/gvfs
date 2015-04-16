
#ifndef __GVFS_DOCUMENT_INPUT_STREAM_H__
#define __GVFS_DOCUMENT_INPUT_STREAM_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define GVFS_TYPE_DOCUMENT_INPUT_STREAM         (_gvfs_document_input_stream_get_type ())
#define GVFS_DOCUMENT_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GVFS_TYPE_DOCUMENT_INPUT_STREAM, GVfsDocumentInputStream))
#define GVFS_DOCUMENT_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), GVFS_TYPE_DOCUMENT_INPUT_STREAM, GVfsDocumentInputStreamClass))
#define GVFS_IS_DOCUMENT_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GVFS_TYPE_DOCUMENT_INPUT_STREAM))
#define GVFS_IS_DOCUMENT_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), GVFS_TYPE_DOCUMENT_INPUT_STREAM))
#define GVFS_DOCUMENT_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), GVFS_TYPE_DOCUMENT_INPUT_STREAM, GVfsDocumentInputStreamClass))

typedef struct _GVfsDocumentInputStream         GVfsDocumentInputStream;
typedef struct _GVfsDocumentInputStreamClass    GVfsDocumentInputStreamClass;
typedef struct _GVfsDocumentInputStreamPrivate  GVfsDocumentInputStreamPrivate;

struct _GVfsDocumentInputStream
{
  GFileInputStream parent_instance;

  /*< private >*/
  GVfsDocumentInputStreamPrivate *priv;
};

struct _GVfsDocumentInputStreamClass
{
  GFileInputStreamClass parent_class;
};

GType              _gvfs_document_input_stream_get_type (void) G_GNUC_CONST;

GFileInputStream *_gvfs_document_input_stream_new (int fd);

G_END_DECLS

#endif /* __GVFS_DOCUMENT_INPUT_STREAM_H__ */
