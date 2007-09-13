#ifndef __G_FILE_LOCAL_H__
#define __G_FILE_LOCAL_H__

#include <gfile.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_LOCAL         (g_file_local_get_type ())
#define G_FILE_LOCAL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_LOCAL, GFileLocal))
#define G_FILE_LOCAL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_LOCAL, GFileLocalClass))
#define G_IS_FILE_LOCAL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_LOCAL))
#define G_IS_FILE_LOCAL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_LOCAL))
#define G_FILE_LOCAL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_LOCAL, GFileLocalClass))

typedef struct _GFileLocal        GFileLocal;
typedef struct _GFileLocalClass   GFileLocalClass;

struct _GFileLocalClass
{
  GObjectClass parent_class;
};

GType g_file_local_get_type (void) G_GNUC_CONST;
  
GFile * g_file_local_new (const char *filename);

G_END_DECLS


#endif /* __G_FILE_LOCAL_H__ */
