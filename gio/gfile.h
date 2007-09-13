#ifndef __G_FILE_H__
#define __G_FILE_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gfileinfo.h>
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

typedef enum {
  G_FILE_COPY_OVERWRITE = (1<<0),
  G_FILE_COPY_BACKUP = (1<<1),
  G_FILE_COPY_NOFOLLOW_SYMLINKS = (1<<2),
  G_FILE_COPY_ALL_METADATA = (1<<3),
} GFileCopyFlags;

typedef struct _GFile         		GFile; /* Dummy typedef */
typedef struct _GFileIface    		GFileIface;
typedef struct _GDirectoryMonitor       GDirectoryMonitor;
typedef struct _GFileMonitor            GFileMonitor;
typedef enum _GFileMonitorFlags GFileMonitorFlags;

typedef void (*GFileProgressCallback) (goffset current_num_bytes,
				       goffset total_num_bytes,
				       gpointer user_data);

struct _GFileIface
{
  GTypeInterface g_iface;

  /* Virtual Table */

  GFile *             (*dup)                (GFile                *file);
  guint               (*hash)               (GFile                *file);
  gboolean            (*equal)              (GFile                *file1,
					     GFile                *file2);
  gboolean            (*is_native)          (GFile                *file);
  char *              (*get_basename)       (GFile                *file);
  char *              (*get_path)           (GFile                *file);
  char *              (*get_uri)            (GFile                *file);
  char *              (*get_parse_name)     (GFile                *file);
  GFile *             (*get_parent)         (GFile                *file);
  GFile *             (*resolve_relative)   (GFile                *file,
					     const char           *relative_path);
  GFile *             (*get_child_for_display_name) (GFile        *file,
						     const char   *display_name,
						     GError      **error);
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
  void                (*get_info_async)     (GFile                *file,
					     const char           *attributes,
					     GFileGetInfoFlags     flags,
					     int                   io_priority,
					     GCancellable         *cancellable,
					     GAsyncReadyCallback   callback,
					     gpointer              user_data);
  GFileInfo *         (*get_info_finish)    (GFile                *file,
					     GAsyncResult         *res,
					     GError              **error);
  GFileInfo *         (*get_filesystem_info)(GFile                *file,
					     const char           *attributes,
					     GCancellable         *cancellable,
					     GError              **error);
  GFile *             (*set_display_name)   (GFile                *file,
					     const char           *display_name,
					     GCancellable         *cancellable,
					     GError              **error);
  GFileAttributeInfoList * (*query_settable_attributes) (GFile        *file,
							 GCancellable *cancellable,
							 GError      **error);
  GFileAttributeInfoList * (*query_writable_namespaces) (GFile        *file,
							 GCancellable *cancellable,
							 GError      **error);
  gboolean            (*set_attribute)      (GFile                *file,
					     const char           *attribute,
					     const GFileAttributeValue *value,
					     GFileGetInfoFlags     flags,
					     GCancellable         *cancellable,
					     GError              **error);
  gboolean            (*set_attributes_from_info) (GFile          *file,
					     GFileInfo            *info,
					     GFileGetInfoFlags     flags,
					     GCancellable         *cancellable,
					     GError              **error);
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
					     const char           *etag,
					     gboolean              make_backup,
					     GCancellable         *cancellable,
					     GError              **error);
  gboolean            (*delete_file)        (GFile                *file,
					     GCancellable         *cancellable,
					     GError              **error);
  gboolean            (*trash)              (GFile                *file,
					     GCancellable         *cancellable,
					     GError              **error);
  gboolean            (*make_directory)     (GFile                *file,
					     GCancellable         *cancellable,
					     GError              **error);
  gboolean            (*make_symbolic_link) (GFile                *file,
					     const char           *symlink_value,
					     GCancellable         *cancellable,
					     GError              **error);
  gboolean            (*copy)               (GFile                *source,
					     GFile                *destination,
					     GFileCopyFlags        flags,
					     GCancellable         *cancellable,
					     GFileProgressCallback progress_callback,
					     gpointer              progress_callback_data,
					     GError              **error);
  gboolean            (*move)               (GFile                *source,
					     GFile                *destination,
					     GFileCopyFlags        flags,
					     GCancellable         *cancellable,
					     GFileProgressCallback progress_callback,
					     gpointer              progress_callback_data,
					     GError              **error);

  void                (*read_async)         (GFile                *file,
					     int                   io_priority,
					     GCancellable         *cancellable,
					     GAsyncReadyCallback   callback,
					     gpointer              user_data);
  GFileInputStream *  (*read_finish)        (GFile                *file,
					     GAsyncResult         *res,
					     GError              **error);

  void                 (*append_to_async)   (GFile                      *file,
					     int                         io_priority,
					     GCancellable               *cancellable,
					     GAsyncReadyCallback         callback,
					     gpointer                    user_data);
  GFileOutputStream *  (*append_to_finish)  (GFile                      *file,
					     GAsyncResult               *res,
					     GError                    **error);
  void                 (*create_async)      (GFile                      *file,
					     int                         io_priority,
					     GCancellable               *cancellable,
					     GAsyncReadyCallback         callback,
					     gpointer                    user_data);
  GFileOutputStream *  (*create_finish)     (GFile                      *file,
					     GAsyncResult               *res,
					     GError                    **error);
  void                 (*replace_async)     (GFile                      *file,
					     const char                 *etag,
					     gboolean                    make_backup,
					     int                         io_priority,
					     GCancellable               *cancellable,
					     GAsyncReadyCallback         callback,
					     gpointer                    user_data);
  GFileOutputStream *  (*replace_finish)    (GFile                      *file,
					     GAsyncResult               *res,
					     GError                    **error);
  

  void                (*mount_mountable)           (GFile               *file,
						    GMountOperation     *mount_operation,
						    GCancellable         *cancellable,
						    GAsyncReadyCallback  callback,
						    gpointer             user_data);
  GFile *             (*mount_mountable_finish)    (GFile               *file,
						    GAsyncResult        *result,
						    GError             **error);
  void                (*unmount_mountable)         (GFile               *file,
						    GCancellable         *cancellable,
						    GAsyncReadyCallback  callback,
						    gpointer             user_data);
  gboolean            (*unmount_mountable_finish)  (GFile               *file,
						    GAsyncResult        *result,
						    GError             **error);
  void                (*eject_mountable)           (GFile               *file,
						    GCancellable        *cancellable,
						    GAsyncReadyCallback  callback,
						    gpointer             user_data);
  gboolean            (*eject_mountable_finish)    (GFile               *file,
						    GAsyncResult        *result,
						    GError             **error);


  void     (*mount_for_location)        (GFile *location,
					 GMountOperation *mount_operation,
					 GCancellable *cancellable,
					 GAsyncReadyCallback callback,
					 gpointer user_data);
  gboolean (*mount_for_location_finish) (GFile *location,
					 GAsyncResult *result,
					 GError **error);
  
  GDirectoryMonitor* (*monitor_dir)         (GFile                  *file,
					     GFileMonitorFlags       flags);

  GFileMonitor*      (*monitor_file)        (GFile                  *file,
					     GFileMonitorFlags       flags);
};

GType g_file_get_type (void) G_GNUC_CONST;

GFile *                 g_file_new_for_path               (const char                 *path);
GFile *                 g_file_new_for_uri                (const char                 *uri);
GFile *                 g_file_new_for_commandline_arg    (const char                 *arg);
GFile *                 g_file_parse_name                 (const char                 *parse_name);
GFile *                 g_file_dup                        (GFile                      *file);
guint                   g_file_hash                       (gconstpointer               file);
gboolean                g_file_equal                      (GFile                      *file1,
							   GFile                      *file2);
char *                  g_file_get_basename               (GFile                      *file);
char *                  g_file_get_path                   (GFile                      *file);
char *                  g_file_get_uri                    (GFile                      *file);
char *                  g_file_get_parse_name             (GFile                      *file);
GFile *                 g_file_get_parent                 (GFile                      *file);
GFile *                 g_file_get_child                  (GFile                      *file,
							   const char                 *name);
GFile *                 g_file_get_child_for_display_name (GFile                      *file,
							   const char                 *display_name,
							   GError                    **error);
GFile *                 g_file_resolve_relative           (GFile                      *file,
							   const char                 *relative_path);
gboolean                g_file_is_native                  (GFile                      *file);
GFileInputStream *      g_file_read                       (GFile                      *file,
							   GCancellable               *cancellable,
							   GError                    **error);
void                    g_file_read_async                 (GFile                      *file,
							   int                         io_priority,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
GFileInputStream *      g_file_read_finish                (GFile                      *file,
							   GAsyncResult               *res,
							   GError                    **error);
GFileOutputStream *     g_file_append_to                  (GFile                      *file,
							   GCancellable               *cancellable,
							   GError                    **error);
GFileOutputStream *     g_file_create                     (GFile                      *file,
							   GCancellable               *cancellable,
							   GError                    **error);
GFileOutputStream *     g_file_replace                    (GFile                      *file,
							   const char                 *etag,
							   gboolean                    make_backup,
							   GCancellable               *cancellable,
							   GError                    **error);
void                    g_file_append_to_async            (GFile                      *file,
							   int                         io_priority,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
GFileOutputStream *     g_file_append_to_finish           (GFile                      *file,
							   GAsyncResult               *res,
							   GError                    **error);
void                    g_file_create_async               (GFile                      *file,
							   int                         io_priority,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
GFileOutputStream *     g_file_create_finish              (GFile                      *file,
							   GAsyncResult               *res,
							   GError                    **error);
void                    g_file_replace_async              (GFile                      *file,
							   const char                 *etag,
							   gboolean                    make_backup,
							   int                         io_priority,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
GFileOutputStream *     g_file_replace_finish             (GFile                      *file,
							   GAsyncResult               *res,
							   GError                    **error);
GFileInfo *             g_file_get_info                   (GFile                      *file,
							   const char                 *attributes,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
void                    g_file_get_info_async             (GFile                      *file,
							   const char                 *attributes,
							   GFileGetInfoFlags           flags,
							   int                         io_priority,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
GFileInfo *             g_file_get_info_finish            (GFile                      *file,
							   GAsyncResult               *res,
							   GError                    **error);
GFileInfo *             g_file_get_filesystem_info        (GFile                      *file,
							   const char                 *attributes,
							   GCancellable               *cancellable,
							   GError                    **error);
GFileEnumerator *       g_file_enumerate_children         (GFile                      *file,
							   const char                 *attributes,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
GFile *                 g_file_set_display_name           (GFile                      *file,
							   const char                 *display_name,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_delete                     (GFile                      *file,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_trash                      (GFile                      *file,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_copy                       (GFile                      *source,
							   GFile                      *destination,
							   GFileCopyFlags              flags,
							   GCancellable               *cancellable,
							   GFileProgressCallback       progress_callback,
							   gpointer                    progress_callback_data,
							   GError                    **error);
gboolean                g_file_move                       (GFile                      *source,
							   GFile                      *destination,
							   GFileCopyFlags              flags,
							   GCancellable               *cancellable,
							   GFileProgressCallback       progress_callback,
							   gpointer                    progress_callback_data,
							   GError                    **error);
gboolean                g_file_make_directory             (GFile                      *file,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_make_symbolic_link         (GFile                      *file,
							   const char                 *symlink_value,
							   GCancellable               *cancellable,
							   GError                    **error);
GFileAttributeInfoList *g_file_query_settable_attributes  (GFile                      *file,
							   GCancellable               *cancellable,
							   GError                    **error);
GFileAttributeInfoList *g_file_query_writable_namespaces  (GFile                      *file,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_set_attribute              (GFile                      *file,
							   const char                 *attribute,
							   const GFileAttributeValue  *value,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_set_attributes_from_info   (GFile                      *file,
							   GFileInfo                  *info,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_set_attribute_string       (GFile                      *file,
							   const char                 *attribute,
							   const char                 *value,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_set_attribute_byte_string  (GFile                      *file,
							   const char                 *attribute,
							   const char                 *value,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_set_attribute_uint32       (GFile                      *file,
							   const char                 *attribute,
							   guint32                     value,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_set_attribute_int32        (GFile                      *file,
							   const char                 *attribute,
							   gint32                      value,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_set_attribute_uint64       (GFile                      *file,
							   const char                 *attribute,
							   guint64                     value,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
gboolean                g_file_set_attribute_int64        (GFile                      *file,
							   const char                 *attribute,
							   gint64                      value,
							   GFileGetInfoFlags           flags,
							   GCancellable               *cancellable,
							   GError                    **error);
void                    g_mount_for_location              (GFile                      *location,
							   GMountOperation            *mount_operation,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
gboolean                g_mount_for_location_finish       (GFile                      *location,
							   GAsyncResult               *result,
							   GError                    **error);
void                    g_file_mount_mountable            (GFile                      *file,
							   GMountOperation            *mount_operation,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
GFile *                 g_file_mount_mountable_finish     (GFile                      *file,
							   GAsyncResult               *result,
							   GError                    **error);
void                    g_file_unmount_mountable          (GFile                      *file,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
gboolean                g_file_unmount_mountable_finish   (GFile                      *file,
							   GAsyncResult               *result,
							   GError                    **error);
void                    g_file_eject_mountable            (GFile                      *file,
							   GCancellable               *cancellable,
							   GAsyncReadyCallback         callback,
							   gpointer                    user_data);
gboolean                g_file_eject_mountable_finish     (GFile                      *file,
							   GAsyncResult               *result,
							   GError                    **error);



GDirectoryMonitor* g_file_monitor_directory          (GFile                  *file,
						      GFileMonitorFlags       flags);
GFileMonitor*      g_file_monitor_file               (GFile                  *file,
						      GFileMonitorFlags       flags);


/* Utilities */

gboolean g_file_load_contents           (GFile                *file,
					 GCancellable         *cancellable,
					 char                **contents,
					 gsize                *length,
					 GError              **error);
void     g_file_load_contents_async     (GFile                *file,
					 GCancellable         *cancellable,
					 GAsyncReadyCallback   callback,
					 gpointer              user_data);
gboolean g_file_load_contents_finish    (GFile                *file,
					 GAsyncResult         *res,
					 char                **contents,
					 gsize                *length,
					 GError              **error);
gboolean g_file_replace_contents        (GFile                *file,
					 const char           *contents,
					 gsize                 length,
					 const char           *etag,
					 gboolean              make_backup,
					 GCancellable         *cancellable,
					 GError              **error);
void     g_file_replace_contents_async  (GFile                *file,
					 const char           *contents,
					 gsize                 length,
					 const char           *etag,
					 gboolean              make_backup,
					 GCancellable         *cancellable,
					 GAsyncReadyCallback   callback,
					 gpointer              user_data);
gboolean g_file_replace_contents_finish (GFile                *file,
					 GAsyncResult         *res,
					 GError              **error);


G_END_DECLS

#endif /* __G_FILE_H__ */
