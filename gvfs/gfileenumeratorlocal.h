#ifndef __G_FILE_ENUMERATOR_LOCAL_H__
#define __G_FILE_ENUMERATOR_LOCAL_H__

#include <gfileenumerator.h>
#include <gfileinfo.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_ENUMERATOR_LOCAL         (g_file_enumerator_local_get_type ())
#define G_FILE_ENUMERATOR_LOCAL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_ENUMERATOR_LOCAL, GFileEnumeratorLocal))
#define G_FILE_ENUMERATOR_LOCAL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_ENUMERATOR_LOCAL, GFileEnumeratorLocalClass))
#define G_IS_FILE_ENUMERATOR_LOCAL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_ENUMERATOR_LOCAL))
#define G_IS_FILE_ENUMERATOR_LOCAL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_ENUMERATOR_LOCAL))
#define G_FILE_ENUMERATOR_LOCAL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_ENUMERATOR_LOCAL, GFileEnumeratorLocalClass))

typedef struct _GFileEnumeratorLocal         GFileEnumeratorLocal;
typedef struct _GFileEnumeratorLocalClass    GFileEnumeratorLocalClass;
typedef struct _GFileEnumeratorLocalPrivate  GFileEnumeratorLocalPrivate;


struct _GFileEnumeratorLocalClass
{
  GFileEnumeratorClass parent_class;

};

GType g_file_enumerator_local_get_type (void) G_GNUC_CONST;

GFileEnumerator *g_file_enumerator_local_new (const char *filename,
					      GFileInfoRequestFlags requested,
					      const char *attributes,
					      gboolean follow_symlinks);

G_END_DECLS

#endif /* __G_FILE_FILE_ENUMERATOR_LOCAL_H__ */
