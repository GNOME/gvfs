#ifndef __G_LOCAL_FILE_H__
#define __G_LOCAL_FILE_H__

#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_LOCAL_FILE         (g_local_file_get_type ())
#define G_LOCAL_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_LOCAL_FILE, GLocalFile))
#define G_LOCAL_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_LOCAL_FILE, GLocalFileClass))
#define G_IS_LOCAL_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_LOCAL_FILE))
#define G_IS_LOCAL_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_LOCAL_FILE))
#define G_LOCAL_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_LOCAL_FILE, GLocalFileClass))

typedef struct _GLocalFile        GLocalFile;
typedef struct _GLocalFileClass   GLocalFileClass;

struct _GLocalFileClass
{
  GObjectClass parent_class;
};

GType g_local_file_get_type (void) G_GNUC_CONST;
  
GFile * g_local_file_new (const char *filename);

G_END_DECLS

#endif /* __G_LOCAL_FILE_H__ */
