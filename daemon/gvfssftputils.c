/* gvfssftputils.c
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 * SPDX-FileCopyrightText: 2026 Red Hat, Inc.
 */

#include <config.h>

#include <string.h>

#include "gvfssftputils.h"
#include "sftp.h"

typedef struct {
  const guint8 *data;
  gsize remaining;
} ReplyCursor;

static gboolean
skip_bytes (ReplyCursor *cursor, gsize size)
{
  if (size > cursor->remaining)
    return FALSE;

  cursor->data += size;
  cursor->remaining -= size;
  return TRUE;
}

static gboolean
read_uint32 (ReplyCursor *cursor, guint32 *value)
{
  if (cursor->remaining < sizeof (*value))
    return FALSE;

  memcpy (value, cursor->data, sizeof (*value));
  *value = GUINT32_FROM_BE (*value);
  return skip_bytes (cursor, sizeof (*value));
}

static gboolean
skip_string (ReplyCursor *cursor)
{
  guint32 size;

  return read_uint32 (cursor, &size) && skip_bytes (cursor, size);
}

/* With @strict_flags, attribute flags outside the v3 layout are rejected.
 * Such a flag may imply fields of unknown size, which would desync the data
 * that follows. Where the attribute block ends the packet, the unknown fields
 * are instead tolerated as trailing data.
 */
static gboolean
skip_attributes (ReplyCursor *cursor,
                 gboolean    strict_flags)
{
  guint32 flags, count, i;

  if (!read_uint32 (cursor, &flags))
    return FALSE;

  if (strict_flags &&
      (flags & ~(SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID |
                 SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME |
                 SSH_FILEXFER_ATTR_EXTENDED)))
    return FALSE;

  if ((flags & SSH_FILEXFER_ATTR_SIZE) && !skip_bytes (cursor, 8))
    return FALSE;
  if ((flags & SSH_FILEXFER_ATTR_UIDGID) && !skip_bytes (cursor, 8))
    return FALSE;
  if ((flags & SSH_FILEXFER_ATTR_PERMISSIONS) && !skip_bytes (cursor, 4))
    return FALSE;
  if ((flags & SSH_FILEXFER_ATTR_ACMODTIME) && !skip_bytes (cursor, 8))
    return FALSE;

  if (flags & SSH_FILEXFER_ATTR_EXTENDED)
    {
      /* Each extension contains at least two string lengths. Bound the
       * count before iterating, without multiplying untrusted integers.
       */
      if (!read_uint32 (cursor, &count) || count > cursor->remaining / 8)
        return FALSE;

      for (i = 0; i < count; i++)
        if (!skip_string (cursor) || !skip_string (cursor))
          return FALSE;
    }

  return TRUE;
}

/* Check the fields before constructing a reply stream. The consumers assume
 * that scalar reads succeed and string lengths/counts fit in the packet.
 * In particular, checking a short read after allocating a string is too late:
 * its length is controlled by the server, independently of the packet size.
 *
 * The layout is the v3 one defined in daemon/sftp.h; see sections 3
 * (attributes) and 5 (responses) of draft-ietf-secsh-filexfer-02. This
 * validates the wire layout only, not request-specific semantics:
 *
 *  - EXTENDED_REPLY has a request-specific layout, so it is accepted here.
 *    Its consumer must check the fields it reads; the only one implemented
 *    is the statvfs@openssh.com reply in query_fs_info_reply().
 *  - Unknown reply types are accepted here and left to the per-request
 *    callbacks, which check the type before parsing, so a single bogus
 *    packet does not have to take down the whole mount.
 *
 * Trailing data is tolerated except in VERSION, where it denotes more
 * extension pairs.
 */
gboolean
g_vfs_sftp_validate_reply (const guint8 *data, gsize size)
{
  ReplyCursor cursor = { data, size };
  guint32 count, i;

  /* Every reply starts with a type and a request id (version for VERSION). */
  if (!skip_bytes (&cursor, 5))
    return FALSE;

  switch (data[0])
    {
      case SSH_FXP_VERSION:
        while (cursor.remaining > 0)
          if (!skip_string (&cursor) || !skip_string (&cursor))
            return FALSE;
        return TRUE;

      case SSH_FXP_STATUS:
        if (!skip_bytes (&cursor, 4))
          return FALSE;

        /* Older servers omit the error message and language tag. */
        return cursor.remaining == 0 ||
               (skip_string (&cursor) && skip_string (&cursor));

      case SSH_FXP_HANDLE:
      case SSH_FXP_DATA:
        return skip_string (&cursor);

      case SSH_FXP_NAME:
        /* Each entry contains a filename, longname and attribute flags. */
        if (!read_uint32 (&cursor, &count) || count > cursor.remaining / 12)
          return FALSE;

        for (i = 0; i < count; i++)
          if (!skip_string (&cursor) || !skip_string (&cursor) ||
              !skip_attributes (&cursor, TRUE))
            return FALSE;
        return TRUE;

      case SSH_FXP_ATTRS:
        /* Ends the packet, so unknown flags are harmless trailing data. */
        return skip_attributes (&cursor, FALSE);

      case SSH_FXP_EXTENDED_REPLY:
        /* Request-specific layout; the consumer checks it, see above. */
        return TRUE;

      default:
        /* Left to the per-request callbacks, see above. */
        return TRUE;
    }
}
