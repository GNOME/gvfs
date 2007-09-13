#ifndef __G_FILE_ENUMERATOR_SIMPLE_H__
#define __G_FILE_ENUMERATOR_SIMPLE_H__

#include <genumeratorsimple.h>
#include <gfileinfo.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_ENUMERATOR_SIMPLE         (g_file_enumerator_simple_get_type ())
#define G_FILE_ENUMERATOR_SIMPLE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_ENUMERATOR_SIMPLE, GFileEnumeratorSimple))
#define G_FILE_ENUMERATOR_SIMPLE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_ENUMERATOR_SIMPLE, GFileEnumeratorSimpleClass))
#define G_IS_FILE_ENUMERATOR_SIMPLE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_ENUMERATOR_SIMPLE))
#define G_IS_FILE_ENUMERATOR_SIMPLE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_ENUMERATOR_SIMPLE))
#define G_FILE_ENUMERATOR_SIMPLE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_ENUMERATOR_SIMPLE, GFileEnumeratorSimpleClass))

typedef struct _GFileEnumeratorSimple         GFileEnumeratorSimple;
typedef struct _GFileEnumeratorSimpleClass    GFileEnumeratorSimpleClass;
typedef struct _GFileEnumeratorSimplePrivate  GFileEnumeratorSimplePrivate;


struct _GFileEnumeratorSimpleClass
{
  GFileEnumeratorClass parent_class;

};

GType g_file_enumerator_simple_get_type (void) G_GNUC_CONST;

GFileEnumerator *g_file_enumerator_simple_new (const char *filename,
					       GFileInfoRequestFlags requested,
					       const char *attributes,
					       gboolean follow_symlinks);

G_END_DECLS

#endif /* __G_FILE_FILE_ENUMERATOR_SIMPLE_H__ */
