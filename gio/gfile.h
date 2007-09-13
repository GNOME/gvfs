#ifndef __G_FILE_H__
#define __G_FILE_H__

#include <glib-object.h>
#include <gio/gvfstypes.h>
#include <gio/gfileenumerator.h>
#include <gio/gfileinputstream.h>
#include <gio/gfileoutputstream.h>
#include <gio/gmountoperation.h>

G_BEGIN_DECLS

#define G_TYPE_FILE            (g_file_get_type ())
#define G_FILE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_FILE, GFile))
#define G_IS_FILE(obj)	       (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_FILE))
#define G_FILE_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_FILE, GFileIface))

typedef enum {
  G_FILE_GET_INFO_NOFOLLOW_SYMLINKS = (1<<0)
} GFileGetInfoFlags;

typedef struct _GFile         GFile; /* Dummy typedef */
typedef struct _GFileIface    GFileIface;

typedef void (*GFileReadCallback) (GFile *file,
				   GFileInputStream *stream,
				   gpointer user_data,
				   GError *error);


struct _GFileIface
{
  GTypeInterface g_iface;

  /* Virtual Table */

  GFile *             (*copy)               (GFile                *file);
  guint               (*hash)               (GFile                *file);
  gboolean            (*equal)              (GFile                *file1,
					     GFile                *file2);
  gboolean            (*is_native)          (GFile                *file);
  char *              (*get_path)           (GFile                *file);
  char *              (*get_uri)            (GFile                *file);
  char *              (*get_parse_name)     (GFile                *file);
  GFile *             (*get_parent)         (GFile                *file);
  GFile *             (*resolve_relative)   (GFile                *file,
					     const char           *relative_path);
  GFileEnumerator *   (*enumerate_children) (GFile                *file,
					     const char           *attributes,
					     GFileGetInfoFlags     flags,
					     GCancellable         *cancellable,
					     GError              **error);
  GFileInfo *         (*get_info)           (GFile                *file,
					     const char           *attributes,
					     GFileGetInfoFlags     flags,
					     GCancellable         *cancellable,
					     GError              **error);
  /*                  (*get_info_async)     (GFile                *file.. */
  GFileInputStream *  (*read)               (GFile                *file,
					     GCancellable         *cancellable,
					     GError              **error);
  GFileOutputStream * (*append_to)          (GFile                *file,
					     GCancellable         *cancellable,
					     GError               **error);
  GFileOutputStream * (*create)             (GFile                *file,
					     GCancellable         *cancellable,
					     GError               **error);
  GFileOutputStream * (*replace)            (GFile                *file,
					     time_t                mtime,
					     gboolean              make_backup,
					     GCancellable         *cancellable,
					     GError              **error);
  
  void                (*read_async)         (GFile                *file,
					     int                   io_priority,
					     GFileReadCallback     callback,
					     gpointer              callback_data,
					     GCancellable         *cancellable);

  void               (*mount)               (GFile                  *file,
					     GMountOperation        *mount_operation);

};

GType g_file_get_type (void) G_GNUC_CONST;

GFile *g_file_get_for_path            (const char *path);
GFile *g_file_get_for_uri             (const char *uri);
GFile *g_file_parse_name              (const char *parse_name);
GFile *g_file_get_for_commandline_arg (const char *arg);

GFile *            g_file_copy               (GFile                  *file);
guint              g_file_hash               (gconstpointer           file);
gboolean           g_file_equal              (GFile                  *file1,
					      GFile                  *file2);
gboolean           g_file_is_native          (GFile                  *file);
char *             g_file_get_path           (GFile                  *file);
char *             g_file_get_uri            (GFile                  *file);
char *             g_file_get_parse_name     (GFile                  *file);
GFile *            g_file_get_parent         (GFile                  *file);
GFile *            g_file_get_child          (GFile                  *file,
					      const char             *name);
GFile *            g_file_resolve_relative   (GFile                  *file,
					      const char             *relative_path);
GFileEnumerator *  g_file_enumerate_children (GFile                  *file,
					      const char             *attributes,
					      GFileGetInfoFlags       flags,
					      GCancellable           *cancellable,
					      GError                **error);
GFileInfo *        g_file_get_info           (GFile                  *file,
					      const char             *attributes,
					      GFileGetInfoFlags       flags,
					      GCancellable           *cancellable,
					      GError                **error);
GFileInputStream * g_file_read               (GFile                  *file,
					      GCancellable           *cancellable,
					      GError                **error);
GFileOutputStream *g_file_append_to          (GFile                  *file,
					      GCancellable           *cancellable,
					      GError                **error);
GFileOutputStream *g_file_create             (GFile                  *file,
					      GCancellable           *cancellable,
					      GError                **error);
GFileOutputStream *g_file_replace            (GFile                  *file,
					      time_t                  mtime,
					      gboolean                make_backup,
					      GCancellable           *cancellable,
					      GError                **error);
void               g_file_read_async         (GFile                  *file,
					      int                     io_priority,
					      GFileReadCallback       callback,
					      gpointer                callback_data,
					      GCancellable           *cancellable);


void               g_file_mount              (GFile                  *file,
					      GMountOperation        *mount_operation);

G_END_DECLS

#endif /* __G_FILE_H__ */
