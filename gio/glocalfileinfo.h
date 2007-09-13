#ifndef __G_LOCAL_FILE_INFO_H__
#define __G_LOCAL_FILE_INFO_H__

#include <gio/gfileinfo.h>
#include <gio/gfile.h>
#include <sys/stat.h>

G_BEGIN_DECLS

typedef struct {
  gboolean writable;
  gboolean is_sticky;
  int owner;
} GLocalParentFileInfo;

void       _g_local_file_info_get_parent_info (const char             *dir,
					       GFileAttributeMatcher  *attribute_matcher,
					       GLocalParentFileInfo   *parent_info);
GFileInfo *_g_local_file_info_get             (const char             *basename,
					       const char             *path,
					       GFileAttributeMatcher  *attribute_matcher,
					       GFileGetInfoFlags       flags,
					       GLocalParentFileInfo   *parent_info,
					       GError                **error);
GFileInfo *_g_local_file_info_get_from_fd     (int                     fd,
					       char                   *attributes,
					       GError                **error);
char *     _g_local_file_info_create_etag     (struct stat            *statbuf);

G_END_DECLS

#endif /* __G_FILE_LOCAL_FILE_INFO_H__ */


