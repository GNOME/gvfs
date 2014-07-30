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

#ifndef _GVFSAFPUTILS_H_
#define _GVFSAFPUTILS_H_

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  AFP_RESULT_NO_ERROR             = 0,
  AFP_RESULT_NO_MORE_SESSIONS     = -1068,
  AFP_RESULT_ASP_SESS_CLOSED      = -1072,
  AFP_RESULT_ACCESS_DENIED        = -5000, 
  AFP_RESULT_AUTH_CONTINUE        = -5001,
  AFP_RESULT_BAD_UAM              = -5002,
  AFP_RESULT_BAD_VERS_NUM         = -5003,
  AFP_RESULT_BITMAP_ERR           = -5004,
  AFP_RESULT_CANT_MOVE            = -5005,
  AFP_RESULT_DENY_CONFLICT        = -5006,
  AFP_RESULT_DIR_NOT_EMPTY        = -5007,
  AFP_RESULT_DISK_FULL            = -5008,
  AFP_RESULT_EOF_ERR              = -5009,
  AFP_RESULT_FILE_BUSY            = -5010,
  AFP_RESULT_FLAT_VOL             = -5011,
  AFP_RESULT_ITEM_NOT_FOUND       = -5012,
  AFP_RESULT_LOCK_ERR             = -5013,
  AFP_RESULT_MISC_ERR             = -5014,
  AFP_RESULT_NO_MORE_LOCKS        = -5015,
  AFP_RESULT_NO_SERVER            = -5016,
  AFP_RESULT_OBJECT_EXISTS        = -5017,
  AFP_RESULT_OBJECT_NOT_FOUND     = -5018,
  AFP_RESULT_PARAM_ERR            = -5019,
  AFP_RESULT_RANGE_NOT_LOCKED     = -5020,
  AFP_RESULT_RANGE_OVERLAP        = -5021,
  AFP_RESULT_SESS_CLOSED          = -5022,
  AFP_RESULT_USER_NOT_AUTH        = -5023,
  AFP_RESULT_CALL_NOT_SUPPORTED   = -5024,
  AFP_RESULT_OBJECT_TYPE_ERR      = -5025,
  AFP_RESULT_TOO_MANY_FILES_OPEN  = -5026,
  AFP_RESULT_SERVER_GOING_DOWN    = -5027,
  AFP_RESULT_CANT_RENAME          = -5028,
  AFP_RESULT_DIR_NOT_FOUND        = -5029,
  AFP_RESULT_ICON_TYPE_ERR        = -5030,
  AFP_RESULT_VOL_LOCKED           = -5031,
  AFP_RESULT_OBJECT_LOCKED        = -5032,
  AFP_RESULT_CONTAINS_SHARED_ERR  = -5033,
  AFP_RESULT_ID_NOT_FOUND         = -5034,
  AFP_RESULT_ID_EXISTS            = -5035,
  AFP_RESULT_DIFF_VOL_ERR         = -5036,
  AFP_RESULT_CATALOG_CHANGED      = -5037,
  AFP_RESULT_SAME_OBJECT_ERR      = -5038,
  AFP_RESULT_BAD_ID_ERR           = -5039,
  AFP_RESULT_PWD_SAME_ERR         = -5040,
  AFP_RESULT_PWD_TOO_SHORT_ERR    = -5041,
  AFP_RESULT_PWD_EXPIRED_ERR      = -5042,
  AFP_RESULT_INSIDE_SHARE_ERR     = -5043,
  AFP_RESULT_INSIDE_TRASH_ERR     = -5044,
  AFP_RESULT_PWD_NEEDS_CHANGE_ERR = -5045,
  AFP_RESULT_PWD_POLICY_ERR       = -5046,
  AFP_RESULT_DISK_QUOTA_EXCEEDED  = -5047
} AfpResultCode;

#define G_FILE_ATTRIBUTE_AFP_NODE_ID        "afp::node-id"
#define G_FILE_ATTRIBUTE_AFP_PARENT_DIR_ID  "afp::parent-dir-id"
#define G_FILE_ATTRIBUTE_AFP_CHILDREN_COUNT "afp::children-count"
#define G_FILE_ATTRIBUTE_AFP_UA_PERMISSIONS "afp::ua-permisssions"

GError *afp_result_code_to_gerror (AfpResultCode res_code);

gboolean is_root (const char *filename);

#define REPLY_READ_BYTE(reply, val)			    	\
G_STMT_START {										\
    if (!g_vfs_afp_reply_read_byte (reply, val))    \
        goto invalid_reply;							\
} G_STMT_END

#define REPLY_READ_UINT16(reply, val)				\
G_STMT_START {										\
    if (!g_vfs_afp_reply_read_uint16 (reply, val))	\
        goto invalid_reply;							\
} G_STMT_END

#define REPLY_READ_UINT32(reply, val)				\
G_STMT_START {										\
    if (!g_vfs_afp_reply_read_uint32 (reply, val))	\
        goto invalid_reply;							\
} G_STMT_END

#define REPLY_READ_UINT64(reply, val)				\
G_STMT_START {										\
    if (!g_vfs_afp_reply_read_uint64 (reply, val))	\
        goto invalid_reply;							\
} G_STMT_END

#define REPLY_READ_INT32(reply, val)				\
G_STMT_START {										\
    if (!g_vfs_afp_reply_read_int32 (reply, val))	\
        goto invalid_reply;							\
} G_STMT_END

#define REPLY_GET_DATA(reply, size, val)				\
G_STMT_START {											\
    if (!g_vfs_afp_reply_get_data (reply, size, val))	\
        goto invalid_reply;								\
} G_STMT_END

#define REPLY_READ_PASCAL(reply, is_utf8, val)				\
G_STMT_START {										\
    if (!g_vfs_afp_reply_read_pascal (reply, is_utf8, val))	\
        goto invalid_reply;							\
} G_STMT_END

#define REPLY_READ_AFP_NAME(reply, read_text_encoding, val)					\
G_STMT_START {																\
    if (!g_vfs_afp_reply_read_afp_name (reply, read_text_encoding, val))	\
        goto invalid_reply;													\
} G_STMT_END

#define REPLY_SKIP_TO_EVEN(reply)				\
G_STMT_START {								    \
    if (!g_vfs_afp_reply_skip_to_even (reply))	\
        goto invalid_reply;						\
} G_STMT_END

#define REPLY_SEEK(reply, offset, type)		        	\
G_STMT_START {								    		\
    if (!g_vfs_afp_reply_seek (reply, offset, type))	\
        goto invalid_reply;								\
} G_STMT_END

G_END_DECLS

#endif /* _GVFSAFPUTILS_H_ */
