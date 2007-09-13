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

void
g_vfs_error_from_errno (GError **error,
			int err_no)
{
  char *str;

  /* TODO: Lame error reporting */
  str = g_strdup_printf ("errno = %d", err_no);
  g_set_error (error,
	       G_VFS_ERROR,
	       G_VFS_ERROR_IO,
	       str);
  g_free (str);
}
