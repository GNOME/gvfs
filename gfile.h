#ifndef __G_FILE_H__
#define __G_FILE_H__

#include <glib-object.h>
#include <gvfstypes.h>
#include <gfileenumerator.h>
#include <ginputstream.h>
#include <goutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_FILE            (g_file_get_type ())
#define G_FILE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_FILE, GFile))
#define G_IS_FILE(obj)	       (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_FILE))
#define G_FILE_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_FILE, GFileIface))

typedef struct _GFile         GFile; /* Dummy typedef */
typedef struct _GFileIface    GFileIface;

struct _GFileIface
{
  GTypeInterface g_iface;

  /* Virtual Table */

  gboolean         (*is_native)                 (GFile *file);
  char *           (*get_path)                  (GFile *file);
  char *           (*get_uri)                   (GFile *file);
  char *           (*get_absolute_display_name) (GFile *file);
  void             (*set_keep_open)             (GFile *file,
						 gboolean keep_open);
  GFile *          (*get_parent)                (GFile *file);
  GFile *          (*get_child)                 (GFile *file,
						 char *name);
  GFileEnumerator *(*enumerate_children)        (GFile *file
						 //flags, attributes...
						 );
  GFileInfo *      (*get_info)                  (GFile *file);
  /*               (*get_info_async)            (GFile *file.. */
  GInputStream *   (*read)                      (GFile *file);
  GOutputStream *  (*append_to)                 (GFile *file);
  GOutputStream *  (*create)                    (GFile *file);
  GOutputStream *  (*replace)                   (GFile *file,
						 time_t mtime,
						 gboolean make_backup);
  
/* permissions are all set minus umask, except replace which
   saves old permissions */
    
};

GType g_file_get_type (void) G_GNUC_CONST;

GFile *g_file_get_for_path                (const char *path);
GFile *g_file_get_for_uri                 (const char *uri);
GFile *g_file_parse_absolute_display_name (const char *display_name);

gboolean         g_file_is_native                 (GFile                 *file);
char *           g_file_get_path                  (GFile                 *file);
char *           g_file_get_uri                   (GFile                 *file);
char *           g_file_get_absolute_display_name (GFile                 *file);
void             g_file_set_keep_open             (GFile                 *file,
						   gboolean               keep_open);
GFile *          g_file_get_parent                (GFile                 *file);
GFile *          g_file_get_child                 (GFile                 *file,
						   char                  *name);
GFileEnumerator *g_file_enumerate_children        (GFile                 *file
						   // flags + attributes
						   );
GFileInfo *      g_file_get_info                  (GFile                 *file);
GInputStream *   g_file_read                      (GFile                 *file);
GOutputStream *  g_file_append_to                 (GFile                 *file);
GOutputStream *  g_file_create                    (GFile                 *file);
GOutputStream *  g_file_replace                   (GFile                 *file,
						   time_t                 mtime,
						   gboolean               make_backup);


G_END_DECLS

#endif /* __G_FILE_H__ */
