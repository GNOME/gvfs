#ifndef __G_LOCAL_FILE_ENUMERATOR_H__
#define __G_LOCAL_FILE_ENUMERATOR_H__

#include <gfileenumerator.h>
#include <gfileinfo.h>
#include <gfile.h>

G_BEGIN_DECLS

#define G_TYPE_LOCAL_FILE_ENUMERATOR         (g_local_file_enumerator_get_type ())
#define G_LOCAL_FILE_ENUMERATOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_LOCAL_FILE_ENUMERATOR, GLocalFileEnumerator))
#define G_LOCAL_FILE_ENUMERATOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_LOCAL_FILE_ENUMERATOR, GLocalFileEnumeratorClass))
#define G_IS_LOCAL_FILE_ENUMERATOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_LOCAL_FILE_ENUMERATOR))
#define G_IS_LOCAL_FILE_ENUMERATOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_LOCAL_FILE_ENUMERATOR))
#define G_LOCAL_FILE_ENUMERATOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_LOCAL_FILE_ENUMERATOR, GLocalFileEnumeratorClass))

typedef struct _GLocalFileEnumerator         GLocalFileEnumerator;
typedef struct _GLocalFileEnumeratorClass    GLocalFileEnumeratorClass;
typedef struct _GLocalFileEnumeratorPrivate  GLocalFileEnumeratorPrivate;


struct _GLocalFileEnumeratorClass
{
  GFileEnumeratorClass parent_class;

};

GType g_local_file_enumerator_get_type (void) G_GNUC_CONST;

GFileEnumerator *g_local_file_enumerator_new (const char *filename,
					      const char *attributes,
					      GFileGetInfoFlags flags,
					      GCancellable *cancellable,
					      GError **error);

G_END_DECLS

#endif /* __G_FILE_LOCAL_FILE_ENUMERATOR_H__ */
