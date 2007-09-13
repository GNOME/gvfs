#ifndef __G_FILE_SIMPLE_H__
#define __G_FILE_SIMPLE_H__

#include <gfile.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_SIMPLE         (g_file_simple_get_type ())
#define G_FILE_SIMPLE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_SIMPLE, GFileSimple))
#define G_FILE_SIMPLE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_SIMPLE, GFileSimpleClass))
#define G_IS_FILE_SIMPLE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_SIMPLE))
#define G_IS_FILE_SIMPLE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_SIMPLE))
#define G_FILE_SIMPLE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_SIMPLE, GFileSimpleClass))

typedef struct _GFileSimple        GFileSimple;
typedef struct _GFileSimpleClass   GFileSimpleClass;

struct _GFileSimpleClass
{
  GObjectClass parent_class;
};

GType g_file_simple_get_type (void) G_GNUC_CONST;
  
GFile * g_file_simple_new (const char *filename);

G_END_DECLS


#endif /* __G_FILE_SIMPLE_H__ */
