#ifndef __G_FILE_INFO_H__
#define __G_FILE_INFO_H__

#include <glib-object.h>
#include <gio/giotypes.h>

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

typedef enum {
  G_FILE_ACCESS_CAN_READ = 1 << 1,
  G_FILE_ACCESS_CAN_WRITE = 1 << 2,
  G_FILE_ACCESS_CAN_EXECUTE = 1 << 3,
  G_FILE_ACCESS_CAN_DELETE = 1 << 4,
  G_FILE_ACCESS_CAN_RENAME = 1 << 5,
} GFileAccessRights;

typedef enum {
  G_FILE_ATTRIBUTE_TYPE_INVALID = 0,
  G_FILE_ATTRIBUTE_TYPE_STRING,
  G_FILE_ATTRIBUTE_TYPE_BYTE_STRING,
  G_FILE_ATTRIBUTE_TYPE_UINT32,
  G_FILE_ATTRIBUTE_TYPE_INT32,
  G_FILE_ATTRIBUTE_TYPE_UINT64,
  G_FILE_ATTRIBUTE_TYPE_INT64
} GFileAttributeType;

typedef enum {
  G_FILE_FLAG_HIDDEN  = (1<<0),
  G_FILE_FLAG_SYMLINK = (1<<1),
  G_FILE_FLAG_LOCAL   = (1<<2),
  G_FILE_FLAG_VIRTUAL = (1<<3),
} GFileFlags;

struct _GFileInfoClass
{
  GObjectClass parent_class;
};

/* Common Attributes:  */

#define G_FILE_ATTRIBUTE_STD_TYPE "std:type"
#define G_FILE_ATTRIBUTE_STD_FLAGS "std:flags"
#define G_FILE_ATTRIBUTE_STD_NAME "std:name"
#define G_FILE_ATTRIBUTE_STD_DISPLAY_NAME "std:display_name"
#define G_FILE_ATTRIBUTE_STD_EDIT_NAME "std:edit_name"
#define G_FILE_ATTRIBUTE_STD_ICON "std:icon"
#define G_FILE_ATTRIBUTE_STD_MIME_TYPE "std:mime_type"
#define G_FILE_ATTRIBUTE_STD_SIZE "std:size"
#define G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET "std:symlink_target"
#define G_FILE_ATTRIBUTE_STD_ACCESS_RIGHTS "std:access_rights"
#define G_FILE_ATTRIBUTE_STD_ACCESS_RIGHTS_MASK "std:access_rights_mask"
#define G_FILE_ATTRIBUTE_STD_MTIME "std:mtime"
#define G_FILE_ATTRIBUTE_STD_MTIME_USEC "std:mtime_usec"

/* Unix specific attributes */

#define G_FILE_ATTRIBUTE_UNIX_DEVICE "unix:device"
#define G_FILE_ATTRIBUTE_UNIX_INODE "unix:inode"
#define G_FILE_ATTRIBUTE_UNIX_MODE "unix:mode"
#define G_FILE_ATTRIBUTE_UNIX_NLINK "unix:nlink"
#define G_FILE_ATTRIBUTE_UNIX_UID "unix:uid"
#define G_FILE_ATTRIBUTE_UNIX_GID "unix:gid"
#define G_FILE_ATTRIBUTE_UNIX_RDEV "unix:rdev"
#define G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE "unix:block_size"
#define G_FILE_ATTRIBUTE_UNIX_BLOCKS "unix:blocks"
#define G_FILE_ATTRIBUTE_UNIX_ATIME "unix:atime"
#define G_FILE_ATTRIBUTE_UNIX_ATIME_USEC "unix:atime_usec"
#define G_FILE_ATTRIBUTE_UNIX_CTIME "unix:ctime"
#define G_FILE_ATTRIBUTE_UNIX_CTIME_USEC "unix:ctime_usec"

/* Owner attributes */

#define G_FILE_ATTRIBUTE_OWNER_USER "owner:user"
#define G_FILE_ATTRIBUTE_OWNER_GROUP "owner:group"

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
char *             g_file_info_get_attribute_as_string   (GFileInfo  *info,
							  const char *attribute);
const char *       g_file_info_get_attribute_string      (GFileInfo  *info,
							  const char *attribute);
const char *       g_file_info_get_attribute_byte_string (GFileInfo  *info,
							  const char *attribute);
guint32            g_file_info_get_attribute_uint32      (GFileInfo  *info,
							  const char *attribute);
gint32             g_file_info_get_attribute_int32       (GFileInfo  *info,
							  const char *attribute);
guint64            g_file_info_get_attribute_uint64      (GFileInfo  *info,
							  const char *attribute);
gint64             g_file_info_get_attribute_int64       (GFileInfo  *info,
							  const char *attribute);
void               g_file_info_set_attribute_string      (GFileInfo  *info,
							  const char *attribute,
							  const char *value);
void               g_file_info_set_attribute_byte_string (GFileInfo  *info,
							  const char *attribute,
							  const char *value);
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


/* Helper getters: */
GFileType         g_file_info_get_file_type          (GFileInfo         *info);
GFileFlags        g_file_info_get_flags              (GFileInfo         *info);
const char *      g_file_info_get_name               (GFileInfo         *info);
const char *      g_file_info_get_display_name       (GFileInfo         *info);
const char *      g_file_info_get_edit_name          (GFileInfo         *info);
const char *      g_file_info_get_icon               (GFileInfo         *info);
const char *      g_file_info_get_mime_type          (GFileInfo         *info);
goffset           g_file_info_get_size               (GFileInfo         *info);
void              g_file_info_get_modification_time  (GFileInfo         *info,
						      GTimeVal          *result);
const char *      g_file_info_get_symlink_target     (GFileInfo         *info);
GFileAccessRights g_file_info_get_access_rights      (GFileInfo         *info);
GFileAccessRights g_file_info_get_access_rights_mask (GFileInfo         *info);


/* Helper setters: */
void              g_file_info_set_file_type          (GFileInfo         *info,
						      GFileType          type);
void              g_file_info_set_flags              (GFileInfo         *info,
						      GFileFlags         flags);
void              g_file_info_set_name               (GFileInfo         *info,
						      const char        *name);
void              g_file_info_set_display_name       (GFileInfo         *info,
						      const char        *display_name);
void              g_file_info_set_edit_name          (GFileInfo         *info,
						      const char        *edit_name);
void              g_file_info_set_icon               (GFileInfo         *info,
						      const char        *icon);
void              g_file_info_set_mime_type          (GFileInfo         *info,
						      const char        *mime_type);
void              g_file_info_set_size               (GFileInfo         *info,
						      goffset            size);
void              g_file_info_set_modification_time  (GFileInfo         *info,
						      GTimeVal          *mtime);
void              g_file_info_set_symlink_target     (GFileInfo         *info,
						      const char        *symlink_target);
void              g_file_info_set_access_rights      (GFileInfo         *info,
						      GFileAccessRights  rights);
void              g_file_info_set_access_rights_mask (GFileInfo         *info,
						      GFileAccessRights  mask);


GFileAttributeMatcher *g_file_attribute_matcher_new            (const char            *attributes);
void                   g_file_attribute_matcher_free           (GFileAttributeMatcher *matcher);
gboolean               g_file_attribute_matcher_matches        (GFileAttributeMatcher *matcher,
								const char            *full_name);
gboolean               g_file_attribute_matcher_enumerate_namespace (GFileAttributeMatcher *matcher,
								     const char            *namespace);
const char *           g_file_attribute_matcher_enumerate_next (GFileAttributeMatcher *matcher);

G_END_DECLS


#endif /* __G_FILE_INFO_H__ */
