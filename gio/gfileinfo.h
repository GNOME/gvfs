#ifndef __G_FILE_INFO_H__
#define __G_FILE_INFO_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gfileattribute.h>
#include <gio/gicon.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_INFO         (g_file_info_get_type ())
#define G_FILE_INFO(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_INFO, GFileInfo))
#define G_FILE_INFO_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_INFO, GFileInfoClass))
#define G_IS_FILE_INFO(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_INFO))
#define G_IS_FILE_INFO_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_INFO))
#define G_FILE_INFO_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_INFO, GFileInfoClass))

typedef struct _GFileInfo        GFileInfo;
typedef struct _GFileInfoClass   GFileInfoClass;
typedef struct _GFileAttributeMatcher GFileAttributeMatcher;

typedef enum {
  G_FILE_TYPE_UNKNOWN = 0,
  G_FILE_TYPE_REGULAR,
  G_FILE_TYPE_DIRECTORY,
  G_FILE_TYPE_SYMBOLIC_LINK,
  G_FILE_TYPE_SPECIAL, /* socket, fifo, blockdev, chardev */
  G_FILE_TYPE_SHORTCUT,
  G_FILE_TYPE_MOUNTABLE
} GFileType;

struct _GFileInfoClass
{
  GObjectClass parent_class;
};

/* Common Attributes:  */

#define G_FILE_ATTRIBUTE_STD_TYPE "std:type"                     /* uint32 (GFileType) */
#define G_FILE_ATTRIBUTE_STD_IS_HIDDEN "std:is_hidden"           /* boolean */
#define G_FILE_ATTRIBUTE_STD_IS_SYMLINK "std:is_symlink"         /* boolean */
#define G_FILE_ATTRIBUTE_STD_IS_VIRTUAL "std:is_virtual"         /* boolean */
#define G_FILE_ATTRIBUTE_STD_NAME "std:name"                     /* byte string */
#define G_FILE_ATTRIBUTE_STD_DISPLAY_NAME "std:display_name"     /* string */
#define G_FILE_ATTRIBUTE_STD_EDIT_NAME "std:edit_name"           /* string */
#define G_FILE_ATTRIBUTE_STD_ICON "std:icon"                     /* object (GIcon) */
#define G_FILE_ATTRIBUTE_STD_CONTENT_TYPE "std:content_type"     /* string */
#define G_FILE_ATTRIBUTE_STD_SIZE "std:size"                     /* uint64 */
#define G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET "std:symlink_target" /* byte string */
#define G_FILE_ATTRIBUTE_STD_TARGET_URI "std:target_uri"         /* string */

/* Entity tags, used to avoid missing updates on save */

#define G_FILE_ATTRIBUTE_ETAG_VALUE "etag:value"                 /* string */

/* Calculated Access Rights for current user */

#define G_FILE_ATTRIBUTE_ACCESS_CAN_READ "access:can_read"       /* boolean */
#define G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE "access:can_write"     /* boolean */
#define G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE "access:can_execute" /* boolean */
#define G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE "access:can_delete"   /* boolean */
#define G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME "access:can_rename"   /* boolean */ 

/* Mountable attributes */

#define G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT "mountable:can_mount"     /* boolean */
#define G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT "mountable:can_unmount" /* boolean */
#define G_FILE_ATTRIBUTE_MOUNTABLE_CAN_EJECT "mountable:can_eject"     /* boolean */
#define G_FILE_ATTRIBUTE_MOUNTABLE_UNIX_DEVICE "mountable:unix_device" /* uint32 */
#define G_FILE_ATTRIBUTE_MOUNTABLE_HAL_UDI "mountable:hal_udi"         /* string */

/* Time attributes */

#define G_FILE_ATTRIBUTE_TIME_MODIFIED "time:modified"              /* uint64 */
#define G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC "time:modified_usec"    /* uint32 */
#define G_FILE_ATTRIBUTE_TIME_ACCESS "time:access"               /* uint64 */
#define G_FILE_ATTRIBUTE_TIME_ACCESS_USEC "time:access_usec"     /* uint32 */
#define G_FILE_ATTRIBUTE_TIME_CHANGED "time:changed"             /* uint64 */
#define G_FILE_ATTRIBUTE_TIME_CHANGED_USEC "time:changed_usec"   /* uint32 */
#define G_FILE_ATTRIBUTE_TIME_CREATED "time:created"             /* uint64 */
#define G_FILE_ATTRIBUTE_TIME_CREATED_USEC "time:created_usec"   /* uint32 */

/* Unix specific attributes */

#define G_FILE_ATTRIBUTE_UNIX_DEVICE "unix:device"               /* uint32 */
#define G_FILE_ATTRIBUTE_UNIX_INODE "unix:inode"                 /* uint64 */
#define G_FILE_ATTRIBUTE_UNIX_MODE "unix:mode"                   /* uint32 */
#define G_FILE_ATTRIBUTE_UNIX_NLINK "unix:nlink"                 /* uint32 */
#define G_FILE_ATTRIBUTE_UNIX_UID "unix:uid"                     /* uint32 */
#define G_FILE_ATTRIBUTE_UNIX_GID "unix:gid"                     /* uint32 */
#define G_FILE_ATTRIBUTE_UNIX_RDEV "unix:rdev"                   /* uint32 */
#define G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE "unix:block_size"       /* uint32 */
#define G_FILE_ATTRIBUTE_UNIX_BLOCKS "unix:blocks"               /* uint64 */

/* DOS specific attributes */

#define G_FILE_ATTRIBUTE_DOS_ARCHIVE "dos:archive"               /* boolean */
#define G_FILE_ATTRIBUTE_DOS_SYSTEM "dos:system"                 /* boolean */

/* Owner attributes */

#define G_FILE_ATTRIBUTE_OWNER_USER "owner:user"                 /* string */
#define G_FILE_ATTRIBUTE_OWNER_GROUP "owner:group"               /* string */

/* File system info (for g_file_get_filesystem_info) */

#define G_FILE_ATTRIBUTE_FS_SIZE "fs:size"                       /* uint64 */
#define G_FILE_ATTRIBUTE_FS_FREE "fs:free"                       /* uint64 */
#define G_FILE_ATTRIBUTE_FS_TYPE "fs:type"                       /* string */

#define G_FILE_ATTRIBUTE_GVFS_BACKEND "gvfs:backend"             /* string */

GType g_file_info_get_type (void) G_GNUC_CONST;

GFileInfo *        g_file_info_new                       (void);
GFileInfo *        g_file_info_copy                      (GFileInfo  *info);
gboolean           g_file_info_has_attribute             (GFileInfo  *info,
							  const char *attribute);
char **            g_file_info_list_attributes           (GFileInfo  *info,
							  const char *name_space);
GFileAttributeType g_file_info_get_attribute_type        (GFileInfo  *info,
							  const char *attribute);
void               g_file_info_remove_attribute          (GFileInfo  *info,
							  const char *attribute);
const GFileAttributeValue * g_file_info_get_attribute         (GFileInfo  *info,
							  const char *attribute);
const char *       g_file_info_get_attribute_string      (GFileInfo  *info,
							  const char *attribute);
const char *       g_file_info_get_attribute_byte_string (GFileInfo  *info,
							  const char *attribute);
gboolean           g_file_info_get_attribute_boolean     (GFileInfo  *info,
							  const char *attribute);
guint32            g_file_info_get_attribute_uint32      (GFileInfo  *info,
							  const char *attribute);
gint32             g_file_info_get_attribute_int32       (GFileInfo  *info,
							  const char *attribute);
guint64            g_file_info_get_attribute_uint64      (GFileInfo  *info,
							  const char *attribute);
gint64             g_file_info_get_attribute_int64       (GFileInfo  *info,
							  const char *attribute);
GObject *          g_file_info_get_attribute_object      (GFileInfo  *info,
							  const char *attribute);

void               g_file_info_set_attribute             (GFileInfo  *info,
							  const char *attribute,
							  const GFileAttributeValue *value);
void               g_file_info_set_attribute_string      (GFileInfo  *info,
							  const char *attribute,
							  const char *value);
void               g_file_info_set_attribute_byte_string (GFileInfo  *info,
							  const char *attribute,
							  const char *value);
void               g_file_info_set_attribute_boolean     (GFileInfo  *info,
							  const char *attribute,
							  gboolean    value);
void               g_file_info_set_attribute_uint32      (GFileInfo  *info,
							  const char *attribute,
							  guint32     value);
void               g_file_info_set_attribute_int32       (GFileInfo  *info,
							  const char *attribute,
							  gint32      value);
void               g_file_info_set_attribute_uint64      (GFileInfo  *info,
							  const char *attribute,
							  guint64     value);
void               g_file_info_set_attribute_int64       (GFileInfo  *info,
							  const char *attribute,
							  gint64      value);
void               g_file_info_set_attribute_object      (GFileInfo  *info,
							  const char *attribute,
							  GObject    *value);

/* Helper getters: */
GFileType         g_file_info_get_file_type          (GFileInfo         *info);
gboolean          g_file_info_get_is_hidden          (GFileInfo         *info);
gboolean          g_file_info_get_is_symlink         (GFileInfo         *info);
const char *      g_file_info_get_name               (GFileInfo         *info);
const char *      g_file_info_get_display_name       (GFileInfo         *info);
const char *      g_file_info_get_edit_name          (GFileInfo         *info);
GIcon *           g_file_info_get_icon               (GFileInfo         *info);
const char *      g_file_info_get_content_type       (GFileInfo         *info);
goffset           g_file_info_get_size               (GFileInfo         *info);
void              g_file_info_get_modification_time  (GFileInfo         *info,
						      GTimeVal          *result);
const char *      g_file_info_get_symlink_target     (GFileInfo         *info);

/* Helper setters: */
void              g_file_info_set_file_type          (GFileInfo         *info,
						      GFileType          type);
void              g_file_info_set_is_hidden          (GFileInfo         *info,
						      gboolean           is_hidden);
void              g_file_info_set_is_symlink         (GFileInfo         *info,
						      gboolean           is_symlink);
void              g_file_info_set_name               (GFileInfo         *info,
						      const char        *name);
void              g_file_info_set_display_name       (GFileInfo         *info,
						      const char        *display_name);
void              g_file_info_set_edit_name          (GFileInfo         *info,
						      const char        *edit_name);
void              g_file_info_set_icon               (GFileInfo         *info,
						      GIcon             *icon);
void              g_file_info_set_content_type       (GFileInfo         *info,
						      const char        *content_type);
void              g_file_info_set_size               (GFileInfo         *info,
						      goffset            size);
void              g_file_info_set_modification_time  (GFileInfo         *info,
						      GTimeVal          *mtime);
void              g_file_info_set_symlink_target     (GFileInfo         *info,
						      const char        *symlink_target);

GFileAttributeMatcher *g_file_attribute_matcher_new            (const char            *attributes);
void                   g_file_attribute_matcher_free           (GFileAttributeMatcher *matcher);
gboolean               g_file_attribute_matcher_matches        (GFileAttributeMatcher *matcher,
								const char            *full_name);
gboolean               g_file_attribute_matcher_matches_only   (GFileAttributeMatcher *matcher,
								const char            *full_name);
gboolean               g_file_attribute_matcher_enumerate_namespace (GFileAttributeMatcher *matcher,
								     const char            *namespace);
const char *           g_file_attribute_matcher_enumerate_next (GFileAttributeMatcher *matcher);

G_END_DECLS


#endif /* __G_FILE_INFO_H__ */
