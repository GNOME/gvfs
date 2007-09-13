#ifndef __G_FILE_INFO_LOCAL_H__
#define __G_FILE_INFO_LOCAL_H__

#include <gio/gfileinfo.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

GFileInfo *g_file_info_local_get         (const char             *basename,
					  const char             *path,
					  GFileAttributeMatcher  *attribute_matcher,
					  GFileGetInfoFlags       flags,
					  GError                **error);
GFileInfo *g_file_info_local_get_from_fd (int                     fd,
					  char                   *attributes,
					  GError                **error);

G_END_DECLS

#endif /* __G_FILE_FILE_INFO_LOCAL_H__ */


