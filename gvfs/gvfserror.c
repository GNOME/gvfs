#include "gvfserror.h"

/**
 * g_vfs_error_quark:
 *
 * Return value: The quark used as %G_VFS_ERROR
 **/
GQuark
g_vfs_error_quark (void)
{
  return g_quark_from_static_string ("g-vfs-error-quark");
}
