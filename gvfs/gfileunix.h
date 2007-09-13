#ifndef __G_FILE_UNIX_H__
#define __G_FILE_UNIX_H__

#include <gfile.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_UNIX         (g_file_unix_get_type ())
#define G_FILE_UNIX(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_UNIX, GFileUnix))
#define G_FILE_UNIX_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_UNIX, GFileUnixClass))
#define G_IS_FILE_UNIX(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_UNIX))
#define G_IS_FILE_UNIX_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_UNIX))
#define G_FILE_UNIX_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_UNIX, GFileUnixClass))

typedef struct _GFileUnix        GFileUnix;
typedef struct _GFileUnixClass   GFileUnixClass;

struct _GFileUnixClass
{
  GObjectClass parent_class;
};

GType g_file_unix_get_type (void) G_GNUC_CONST;
  
GFile * g_file_unix_new (const char *filename,
			 const char *mountpoint);

G_END_DECLS

#endif /* __G_FILE_UNIX_H__ */
