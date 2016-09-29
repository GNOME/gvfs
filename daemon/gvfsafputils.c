 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsafputils.h"

static const char *
afp_result_code_to_string (AfpResultCode res_code)
{
  struct AfpResults
  {
    AfpResultCode  res_code;
    char          *string;
  };


  struct AfpResults results[] = {
    { AFP_RESULT_NO_ERROR,             "kFPNoErr" },
    { AFP_RESULT_NO_MORE_SESSIONS,     "kFPNoMoreSessions" },
    { AFP_RESULT_ASP_SESS_CLOSED,      "kASPSessClosed" },
    { AFP_RESULT_ACCESS_DENIED,        "kFPAccessDenied" },
    { AFP_RESULT_AUTH_CONTINUE,        "kFPAuthContinue" },
    { AFP_RESULT_BAD_UAM,              "kFPBadUAM" },
    { AFP_RESULT_BAD_VERS_NUM,         "kFPBadVersNum" },
    { AFP_RESULT_CANT_MOVE,            "kFPCantMove" },
    { AFP_RESULT_DENY_CONFLICT,        "kFPDenyConflict" },
    { AFP_RESULT_DIR_NOT_EMPTY,        "kFPDirNotEmpty" },
    { AFP_RESULT_DISK_FULL,            "kFPDiskFull" },
    { AFP_RESULT_EOF_ERR,              "kFPEOFErr" },
    { AFP_RESULT_FILE_BUSY,            "kFPFileBusy" },
    { AFP_RESULT_FLAT_VOL,             "kFPFlatVol" },
    { AFP_RESULT_ITEM_NOT_FOUND,       "kFPItemNotFound" },
    { AFP_RESULT_LOCK_ERR,             "kFPLockErr" },
    { AFP_RESULT_MISC_ERR,             "kFPMiscErr" },
    { AFP_RESULT_NO_MORE_LOCKS,        "kFPNoMoreLocks" },
    { AFP_RESULT_NO_SERVER,            "kFPNoServer" },
    { AFP_RESULT_OBJECT_EXISTS,        "kFPObjectExists" },
    { AFP_RESULT_OBJECT_NOT_FOUND,     "kFPObjectNotFound" },
    { AFP_RESULT_PARAM_ERR,            "kFPParamErr" },
    { AFP_RESULT_RANGE_NOT_LOCKED,     "kFPRangeNotLocked" },
    { AFP_RESULT_RANGE_OVERLAP,        "kFPRangeOverlap" },
    { AFP_RESULT_SESS_CLOSED,          "kFPSessClosed" },
    { AFP_RESULT_USER_NOT_AUTH,        "kFPUserNotAuth" },
    { AFP_RESULT_CALL_NOT_SUPPORTED,   "kFPCallNotSupported" },
    { AFP_RESULT_OBJECT_TYPE_ERR,      "kFPObjectTypeErr" },
    { AFP_RESULT_TOO_MANY_FILES_OPEN,  "kFPTooManyFilesOpen" },
    { AFP_RESULT_SERVER_GOING_DOWN,    "kFPServerGoingDown" },
    { AFP_RESULT_CANT_RENAME,          "kFPCantRename" },
    { AFP_RESULT_DIR_NOT_FOUND,        "kFPDirNotFound" },
    { AFP_RESULT_ICON_TYPE_ERR,        "kFPIconTypeError" },
    { AFP_RESULT_VOL_LOCKED,           "kFPVolLocked" },
    { AFP_RESULT_OBJECT_LOCKED,        "kFPObjectLocked" },
    { AFP_RESULT_CONTAINS_SHARED_ERR,  "kFPContainsSharedErr" },
    { AFP_RESULT_ID_NOT_FOUND,         "kFPObjectLocked" },
    { AFP_RESULT_ID_EXISTS,            "kFPIDExists" },
    { AFP_RESULT_DIFF_VOL_ERR,         "kFPDiffVolErr" },
    { AFP_RESULT_CATALOG_CHANGED,      "kFPCatalogChanged" },
    { AFP_RESULT_SAME_OBJECT_ERR,      "kFPSameObjectErr" },
    { AFP_RESULT_BAD_ID_ERR,           "kFPBadIDErr" },
    { AFP_RESULT_PWD_SAME_ERR,         "kFPPwdSameErr" },
    { AFP_RESULT_PWD_TOO_SHORT_ERR,    "kFPPwdTooShortErr" },
    { AFP_RESULT_PWD_EXPIRED_ERR,      "kFPPwdExpiredErr" },
    { AFP_RESULT_INSIDE_SHARE_ERR,     "kFPInsideSharedErr" },
    { AFP_RESULT_INSIDE_TRASH_ERR,     "kFPInsideTrashErr" },
    { AFP_RESULT_PWD_NEEDS_CHANGE_ERR, "kFPPwdNeedsChangeErr" },
    { AFP_RESULT_PWD_POLICY_ERR,       "kFPPwdNeedsChangeErr" },
    { AFP_RESULT_DISK_QUOTA_EXCEEDED,  "kFPDiskQuotaExceeded" }
  };

  int start, end, mid;

  /* Do a "reversed" binary search,
   * the result codes are stored in declining order */
  start = 0;
  end = G_N_ELEMENTS (results) - 1;
  while (start <= end)
  {
    mid = (start + end) / 2;

    if (res_code < results[mid].res_code)
      start = mid + 1;
    
    else if (res_code > results[mid].res_code)
      end = mid - 1;

    else
      return results[mid].string;
  }

  return NULL;
}

GError *
afp_result_code_to_gerror (AfpResultCode res_code)
{
  const char   *res_string;

  g_return_val_if_fail (res_code != AFP_RESULT_NO_ERROR, NULL);
  res_string = afp_result_code_to_string (res_code);

  if (res_string)
    return g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Got error “%s” from server"), res_string);
  else
    return g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Got unknown error code %d from server"), res_code);
}

gboolean
is_root (const char *filename)
{
  const char *p;

  p = filename;
  while (*p == '/')
    p++;

  return *p == 0;
}
