#ifndef __G_FILE_UNIX_SIMPLE_H__
#define __G_FILE_UNIX_SIMPLE_H__

#include <gfile.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_UNIX_SIMPLE         (g_file_unix_simple_get_type ())
#define G_FILE_UNIX_SIMPLE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_UNIX_SIMPLE, GFileUnixSimple))
#define G_FILE_UNIX_SIMPLE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_UNIX_SIMPLE, GFileUnixSimpleClass))
#define G_IS_FILE_UNIX_SIMPLE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_UNIX_SIMPLE))
#define G_IS_FILE_UNIX_SIMPLE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_UNIX_SIMPLE))
#define G_FILE_UNIX_SIMPLE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_UNIX_SIMPLE, GFileUnixSimpleClass))

typedef struct _GFileUnixSimple        GFileUnixSimple;
typedef struct _GFileUnixSimpleClass   GFileUnixSimpleClass;

struct _GFileUnixSimpleClass
{
  GObjectClass parent_class;
};

GType g_file_unix_simple_get_type (void) G_GNUC_CONST;
  
GFile * g_file_unix_simple_new (GFile *wrapped);

G_END_DECLS


#endif /* __G_FILE_UNIX_SIMPLE_H__ */
