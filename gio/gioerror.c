#include <errno.h>
#include "gioerror.h"

/**
 * g_io_error_quark:
 *
 * Return value: The quark used as %G_IO_ERROR
 **/
GQuark
g_io_error_quark (void)
{
  return g_quark_from_static_string ("g-io-error-quark");
}

GIOErrorEnum
g_io_error_from_errno (gint err_no)
{
  switch (err_no)
    {
#ifdef EEXIST
    case EEXIST:
      return G_IO_ERROR_EXISTS;
      break;
#endif

#ifdef EISDIR
    case EISDIR:
      return G_IO_ERROR_IS_DIRECTORY;
      break;
#endif

#ifdef EACCES
    case EACCES:
      return G_IO_ERROR_PERMISSION_DENIED;
      break;
#endif

#ifdef ENAMETOOLONG
    case ENAMETOOLONG:
      return G_IO_ERROR_FILENAME_TOO_LONG;
      break;
#endif

#ifdef ENOENT
    case ENOENT:
      return G_IO_ERROR_NOT_FOUND;
      break;
#endif

#ifdef ENOTDIR
    case ENOTDIR:
      return G_IO_ERROR_NOT_DIRECTORY;
      break;
#endif

#ifdef EROFS
    case EROFS:
      return G_IO_ERROR_READ_ONLY;
      break;
#endif

#ifdef ELOOP
    case ELOOP:
      return G_IO_ERROR_TOO_MANY_LINKS;
      break;
#endif

#ifdef ENOSPC
    case ENOSPC:
      return G_IO_ERROR_NO_SPACE;
      break;
#endif

#ifdef ENOMEM
    case ENOMEM:
      return G_IO_ERROR_NO_SPACE;
      break;
#endif
      
#ifdef EINVAL
    case EINVAL:
      return G_IO_ERROR_INVALID_ARGUMENT;
      break;
#endif

#ifdef EPERM
    case EPERM:
      return G_IO_ERROR_PERMISSION_DENIED;
      break;
#endif

#ifdef ECANCELED
    case ECANCELED:
      return G_IO_ERROR_CANCELLED;
      break;
#endif

#ifdef ENOTEMPTY
    case ENOTEMPTY:
      return G_IO_ERROR_NOT_EMPTY;
      break;
#endif

#ifdef ENOTSUP
    case ENOTSUP:
      return G_IO_ERROR_NOT_SUPPORTED;
      break;
#endif

#ifdef ETIMEDOUT
    case ETIMEDOUT:
      return G_IO_ERROR_TIMED_OUT;
      break;
#endif
      
    default:
      return G_IO_ERROR_FAILED;
      break;
    }
}
