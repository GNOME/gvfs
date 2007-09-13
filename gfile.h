#ifndef __G_FILE_H__
#define __G_FILE_H__

#include <glib-object.h>
#include <gvfstypes.h>
#include <gfileenumerator.h>
#include <gfileinputstream.h>
#include <gfileoutputstream.h>

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

  GFile *             (*copy)               (GFile                *file);
  gboolean            (*is_native)          (GFile                *file);
  char *              (*get_path)           (GFile                *file);
  char *              (*get_uri)            (GFile                *file);
  char *              (*get_parse_name)     (GFile                *file);
  GFile *             (*get_parent)         (GFile                *file);
  GFile *             (*get_child)          (GFile                *file,
					     const char           *name);
  GFileEnumerator *   (*enumerate_children) (GFile                *file,
					     GFileInfoRequestFlags requested,
					     const char           *attributes);
  GFileInfo *         (*get_info)           (GFile                *file,
					     GFileInfoRequestFlags requested,
					     const char           *attributes);
  /*                  (*get_info_async)     (GFile                *file.. */
  GFileInputStream *  (*read)               (GFile                *file);
  GFileOutputStream * (*append_to)          (GFile                *file);
  GFileOutputStream * (*create)             (GFile                *file);
  GFileOutputStream * (*replace)            (GFile                *file,
					     time_t                mtime,
					     gboolean              make_backup);
};

GType g_file_get_type (void) G_GNUC_CONST;

GFile *g_file_get_for_path  (const char *path);
GFile *g_file_get_for_uri   (const char *uri);
GFile *g_file_parse_name    (const char *parse_name);

GFile *            g_file_copy              (GFile                 *file);
gboolean           g_file_is_native          (GFile                 *file);
char *             g_file_get_path           (GFile                 *file);
char *             g_file_get_uri            (GFile                 *file);
char *             g_file_get_parse_name     (GFile                 *file);
GFile *            g_file_get_parent         (GFile                 *file);
GFile *            g_file_get_child          (GFile                 *file,
					      const char            *name);
GFileEnumerator *  g_file_enumerate_children (GFile                 *file,
					      GFileInfoRequestFlags  requested,
					      const char            *attributes);
GFileInfo *        g_file_get_info           (GFile                 *file,
					      GFileInfoRequestFlags  requested,
					      const char            *attributes);
GFileInputStream * g_file_read               (GFile                 *file);
GFileOutputStream *g_file_append_to          (GFile                 *file);
GFileOutputStream *g_file_create             (GFile                 *file);
GFileOutputStream *g_file_replace            (GFile                 *file,
					      time_t                 mtime,
					      gboolean               make_backup);

G_END_DECLS

#endif /* __G_FILE_H__ */
