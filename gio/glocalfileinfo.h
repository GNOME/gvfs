#ifndef __G_LOCAL_FILE_INFO_H__
#define __G_LOCAL_FILE_INFO_H__

#include <gio/gfileinfo.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

GFileInfo *g_local_file_info_get         (const char             *basename,
					  const char             *path,
					  GFileAttributeMatcher  *attribute_matcher,
					  GFileGetInfoFlags       flags,
					  GError                **error);
GFileInfo *g_local_file_info_get_from_fd (int                     fd,
					  char                   *attributes,
					  GError                **error);

G_END_DECLS

#endif /* __G_FILE_LOCAL_FILE_INFO_H__ */


