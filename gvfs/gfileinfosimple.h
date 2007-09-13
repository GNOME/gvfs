#ifndef __G_FILE_INFO_SIMPLE_H__
#define __G_FILE_INFO_SIMPLE_H__

#include <gfileinfo.h>

G_BEGIN_DECLS

gboolean g_file_info_simple_get (const char *basename,
				 const char *path,
				 GFileInfo *info,
				 GFileInfoRequestFlags requested,
				 GFileAttributeMatcher *attribute_matcher,
				 gboolean follow_symlinks,
				 GError **error);

G_END_DECLS

#endif /* __G_FILE_FILE_INFO_SIMPLE_H__ */


