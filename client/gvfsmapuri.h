#ifndef __G_VFS_MAP_URI_H__
#define __G_VFS_MAP_URI_H__

#include <glib.h>
#include <gvfsuriutils.h>
#include <gmountspec.h>

G_BEGIN_DECLS

typedef void (*g_mountspec_from_uri_func) (GDecodedUri *uri,
					   GMountSpec **spec_out,
					   char **path_out);
typedef void (*g_mountspec_to_uri_func)   (GMountSpec *spec,
					   char *path,
					   GDecodedUri *uri);


typedef struct {
  char *scheme;
  g_mountspec_from_uri_func func;
} GVfsMapFromUri;

typedef struct {
  char *mount_type;
  g_mountspec_to_uri_func func;
} GVfsMapToUri;

#define G_VFS_MAP_FROM_URI_TABLE_NAME g_vfs_map_from_uri_table
#define G_VFS_MAP_TO_URI_TABLE_NAME g_vfs_map_to_uri_table

G_END_DECLS

#endif /* __G_VFS_MAP_URI_H__ */
