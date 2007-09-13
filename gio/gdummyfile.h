#ifndef __G_DUMMY_FILE_H__
#define __G_DUMMY_FILE_H__

#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_DUMMY_FILE         (g_dummy_file_get_type ())
#define G_DUMMY_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DUMMY_FILE, GDummyFile))
#define G_DUMMY_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DUMMY_FILE, GDummyFileClass))
#define G_IS_DUMMY_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DUMMY_FILE))
#define G_IS_DUMMY_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DUMMY_FILE))
#define G_DUMMY_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DUMMY_FILE, GDummyFileClass))

typedef struct _GDummyFile        GDummyFile;
typedef struct _GDummyFileClass   GDummyFileClass;

struct _GDummyFileClass
{
  GObjectClass parent_class;
};

GType g_dummy_file_get_type (void) G_GNUC_CONST;
  
GFile * g_dummy_file_new (const char *uri);

G_END_DECLS

#endif /* __G_DUMMY_FILE_H__ */
