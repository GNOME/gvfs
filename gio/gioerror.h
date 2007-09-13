#ifndef __G_IO_ERROR_H__
#define __G_IO_ERROR_H__

#include <glib/gerror.h>

G_BEGIN_DECLS

GQuark          g_io_error_quark      (void);

#define G_IO_ERROR g_io_error_quark()

/* This enumeration conflicts with GIOError in giochannel.h. However,
 * that is only used as a return value in some deprecated functions.
 * So, we reuse the same prefix for the enumeration values, but call
 * the actual enumeration (which is rarely used) GIOErrorEnum.
 */

typedef enum
{
  G_IO_ERROR_FAILED,
  G_IO_ERROR_NOT_FOUND,
  G_IO_ERROR_EXISTS,
  G_IO_ERROR_IS_DIRECTORY,
  G_IO_ERROR_NOT_DIRECTORY,
  G_IO_ERROR_NOT_EMPTY,
  G_IO_ERROR_NOT_REGULAR_FILE,
  G_IO_ERROR_NOT_SYMBOLIC_LINK,
  G_IO_ERROR_NOT_MOUNTABLE,
  G_IO_ERROR_FILENAME_TOO_LONG,
  G_IO_ERROR_INVALID_FILENAME,
  G_IO_ERROR_TOO_MANY_LINKS,
  G_IO_ERROR_NO_SPACE,
  G_IO_ERROR_INVALID_ARGUMENT,
  G_IO_ERROR_PERMISSION_DENIED,
  G_IO_ERROR_NOT_SUPPORTED,
  G_IO_ERROR_NOT_MOUNTED,
  G_IO_ERROR_ALREADY_MOUNTED,
  G_IO_ERROR_CLOSED,
  G_IO_ERROR_CANCELLED,
  G_IO_ERROR_PENDING,
  G_IO_ERROR_READ_ONLY,
  G_IO_ERROR_CANT_CREATE_BACKUP,
  G_IO_ERROR_WRONG_MTIME,
  G_IO_ERROR_TIMED_OUT,
  G_IO_ERROR_WOULD_RECURSE,
  G_IO_ERROR_BUSY,
} GIOErrorEnum;

GIOErrorEnum g_io_error_from_errno (gint err_no);

#endif /* __G_IO_ERROR_H__ */
