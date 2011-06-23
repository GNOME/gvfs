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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#ifndef _GVFSAFPCONNECTION_H_
#define _GVFSAFPCONNECTION_H_

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum
{
  AFP_PATH_TYPE_SHORT_NAME = 1,
  AFP_PATH_TYPE_LONG_NAME  = 2,
  AFP_PATH_TYPE_UTF8_NAME  = 3
} AfpPathType;

enum
{
  AFP_ACCESS_MODE_READ_BIT       = (1 << 0),
  AFP_ACCESS_MODE_WRITE_BIT      = (1 << 1),
  AFP_ACCESS_MODE_DENY_READ_BIT  = (1 << 4),
  AFP_ACCESS_MODE_DENY_WRITE_BIT = (1 << 5)
};

enum
{
  AFP_FILEDIR_ATTRIBUTES_BITMAP_INVISIBLE_BIT      = 0x1,
  AFP_FILEDIR_ATTRIBUTES_BITMAP_SYSTEM_BIT         = 0x4,
  AFP_FILEDIR_ATTRIBUTES_BITMAP_WRITE_INHIBIT_BIT  = 0x20,
  AFP_FILEDIR_ATTRIBUTES_BITMAP_BACKUP_NEEDED_BIT  = 0x40,
  AFP_FILEDIR_ATTRIBTUES_BITMAP_RENAME_INHIBIT_BIT = 0x80,
  AFP_FILEDIR_ATTRIBTUES_BITMAP_DELETE_INHIBIT_BIT = 0x100,
  AFP_FILEDIR_ATTRIBTUES_BITMAP_COPY_PROTECT_BIT   = 0x400,
  AFP_FILEDIR_ATTRIBTUES_BITMAP_SET_CLEAR_BIT      = 0x8000
};

enum
{
  AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT          = 0x1,
  AFP_FILEDIR_BITMAP_PARENT_DIR_ID_BIT      = 0x2,
  AFP_FILEDIR_BITMAP_CREATE_DATE_BIT        = 0x4,
  AFP_FILEDIR_BITMAP_MOD_DATE_BIT           = 0x8,
  AFP_FILEDIR_BITMAP_BACKUP_DATE_BIT        = 0x10,
  AFP_FILEDIR_BITMAP_FINDER_INFO_BIT        = 0x20,
  AFP_FILEDIR_BITMAP_LONG_NAME_BIT          = 0x40,
  AFP_FILEDIR_BITMAP_SHORT_NAME_BIT         = 0x80,
  AFP_FILEDIR_BITMAP_NODE_ID_BIT            = 0x100,
  AFP_FILEDIR_BITMAP_UTF8_NAME_BIT          = 0x2000,
  AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT         = 0x8000,
};

enum
{
  AFP_DIR_BITMAP_ATTRIBUTE_BIT          = 0x1,
  AFP_DIR_BITMAP_PARENT_DIR_ID_BIT      = 0x2,
  AFP_DIR_BITMAP_CREATE_DATE_BIT        = 0x4,
  AFP_DIR_BITMAP_MOD_DATE_BIT           = 0x8,
  AFP_DIR_BITMAP_BACKUP_DATE_BIT        = 0x10,
  AFP_DIR_BITMAP_FINDER_INFO_BIT        = 0x20,
  AFP_DIR_BITMAP_LONG_NAME_BIT          = 0x40,
  AFP_DIR_BITMAP_SHORT_NAME_BIT         = 0x80,
  AFP_DIR_BITMAP_NODE_ID_BIT            = 0x100,
  AFP_DIR_BITMAP_OFFSPRING_COUNT_BIT    = 0x200,
  AFP_DIR_BITMAP_OWNER_ID_BIT           = 0x400,
  AFP_DIR_BITMAP_GROUP_ID_BIT           = 0x800,
  AFP_DIR_BITMAP_ACCESS_RIGHTS_BIT      = 0x1000,
  AFP_DIR_BITMAP_UTF8_NAME_BIT          = 0x2000,
  AFP_DIR_BITMAP_UNIX_PRIVS_BIT         = 0x8000,
  AFP_DIR_BITMAP_UUID_BIT               = 0x10000 // AFP version 3.2 and later (with ACL support)
};

enum
{
  AFP_FILE_BITMAP_ATTRIBUTE_BIT          = 0x1,
  AFP_FILE_BITMAP_PARENT_DIR_ID_BIT      = 0x2,
  AFP_FILE_BITMAP_CREATE_DATE_BIT        = 0x4,
  AFP_FILE_BITMAP_MOD_DATE_BIT           = 0x8,
  AFP_FILE_BITMAP_BACKUP_DATE_BIT        = 0x10,
  AFP_FILE_BITMAP_FINDER_INFO_BIT        = 0x20,
  AFP_FILE_BITMAP_LONG_NAME_BIT          = 0x40,
  AFP_FILE_BITMAP_SHORT_NAME_BIT         = 0x80,
  AFP_FILE_BITMAP_NODE_ID_BIT            = 0x100,
  AFP_FILE_BITMAP_DATA_FORK_LEN_BIT      = 0x200,
  AFP_FILE_BITMAP_RSRC_FORK_LEN_BIT      = 0x400,
  AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT  = 0x800,
  AFP_FILE_BITMAP_LAUNCH_LIMIT_BIT       = 0x1000,
  AFP_FILE_BITMAP_UTF8_NAME_BIT          = 0x2000,
  AFP_FILE_BITMAP_EXT_RSRC_FORK_LEN_BIT  = 0x4000,
  AFP_FILE_BITMAP_UNIX_PRIVS_BIT         = 0x8000
};

enum
{
  AFP_VOLUME_BITMAP_ATTRIBUTE_BIT       = 0x1,
  AFP_VOLUME_BITMAP_SIGNATURE_BIT       = 0x2,
  AFP_VOLUME_BITMAP_CREATE_DATE_BIT     = 0x4,
  AFP_VOLUME_BITMAP_MOD_DATE_BIT        = 0x8,
  AFP_VOLUME_BITMAP_BACKUP_DATE_BIT     = 0x10,
  AFP_VOLUME_BITMAP_VOL_ID_BIT          = 0x20,
  AFP_VOLUME_BITMAP_BYTES_FREE_BIT      = 0x40,
  AFP_VOLUME_BITMAP_BYTES_TOTAL_BIT     = 0x80,
  AFP_VOLUME_BITMAP_NAME_BIT            = 0x100,
  AFP_VOLUME_BITMAP_EXT_BYTES_FREE_BIT  = 0x200,
  AFP_VOLUME_BITMAP_EXT_BYTES_TOTAL_BIT = 0x400,
  AFP_VOLUME_BITMAP_BLOCK_SIZE_BIT      = 0x800  
};

typedef enum
{
  AFP_COMMAND_CLOSE_FORK         = 4,
  AFP_COMMAND_GET_FORK_PARMS     = 14,
  AFP_COMMAND_GET_SRVR_INFO      = 15,
  AFP_COMMAND_GET_SRVR_PARMS     = 16,
  AFP_COMMAND_GET_VOL_PARMS      = 17,
  AFP_COMMAND_LOGIN              = 18,
  AFP_COMMAND_LOGIN_CONT         = 19,
  AFP_COMMAND_OPEN_VOL           = 24,
  AFP_COMMAND_OPEN_FORK          = 26,
  AFP_COMMAND_WRITE              = 33,
  AFP_COMMAND_GET_FILE_DIR_PARMS = 34,
  AFP_COMMAND_READ_EXT           = 60,
  AFP_COMMAND_WRITE_EXT          = 61,
  AFP_COMMAND_ENUMERATE_EXT      = 66,
  AFP_COMMAND_ENUMERATE_EXT2     = 68
} AfpCommandType;

typedef enum
{
  AFP_RESULT_NO_ERROR            = 0,
  AFP_RESULT_NO_MORE_SESSIONS    = -1068,
  AFP_RESULT_ACCESS_DENIED       = -5000, 
  AFP_RESULT_AUTH_CONTINUE       = -5001,
  AFP_RESULT_EOF_ERR             = -5009,
  AFP_RESULT_LOCK_ERR            = -5013,
  AFP_RESULT_OBJECT_NOT_FOUND    = -5018,
  AFP_RESULT_PARAM_ERR           = -5019,
  AFP_RESULT_USER_NOT_AUTH       = -5023,
  AFP_RESULT_OBJECT_TYPE_ERR     = -5025,
  AFP_RESULT_TOO_MANY_FILES_OPEN = -5026
} AfpResultCode;

/*
 * GVfsAfpName
 */
typedef struct _GVfsAfpName GVfsAfpName;

struct _GVfsAfpName
{
  guint32 text_encoding;
  gchar *str;
  gsize len;

  gint ref_count;
};

GVfsAfpName* g_vfs_afp_name_new              (guint32 text_encoding, gchar *str, gsize len);

void         g_vfs_afp_name_unref            (GVfsAfpName *afp_name);
void         g_vfs_afp_name_ref              (GVfsAfpName *afp_name);

char*        g_vfs_afp_name_get_string       (GVfsAfpName *afp_name);

/*
 * GVfsAfpReply
 */
#define G_VFS_TYPE_AFP_REPLY             (g_vfs_afp_reply_get_type ())
#define G_VFS_AFP_REPLY(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_AFP_REPLY, GVfsAfpReply))
#define G_VFS_AFP_REPLY_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_AFP_REPLY, GVfsAfpReplyClass))
#define G_VFS_IS_AFP_REPLY(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_AFP_REPLY))
#define G_VFS_IS_AFP_REPLY_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_AFP_REPLY))
#define G_VFS_AFP_REPLY_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_AFP_REPLY, GVfsAfpReplyClass))

typedef struct _GVfsAfpReplyClass GVfsAfpReplyClass;
typedef struct _GVfsAfpReply      GVfsAfpReply;

gboolean        g_vfs_afp_reply_read_byte         (GVfsAfpReply *reply, guint8 *byte);

gboolean        g_vfs_afp_reply_read_int64        (GVfsAfpReply *reply, gint64 *val);
gboolean        g_vfs_afp_reply_read_int32        (GVfsAfpReply *reply, gint32 *val);
gboolean        g_vfs_afp_reply_read_int16        (GVfsAfpReply *reply, gint16 *val);

gboolean        g_vfs_afp_reply_read_uint64       (GVfsAfpReply *reply, guint64 *val);
gboolean        g_vfs_afp_reply_read_uint32       (GVfsAfpReply *reply, guint32 *val);
gboolean        g_vfs_afp_reply_read_uint16       (GVfsAfpReply *reply, guint16 *val);

gboolean        g_vfs_afp_reply_get_data          (GVfsAfpReply *reply, gsize size, guint8 **data);
gboolean        g_vfs_afp_reply_dup_data          (GVfsAfpReply *reply, gsize size, guint8 **data);

gboolean        g_vfs_afp_reply_read_pascal       (GVfsAfpReply *reply, char **str);
gboolean        g_vfs_afp_reply_read_afp_name     (GVfsAfpReply *reply, gboolean read_text_encoding, GVfsAfpName **afp_name);

gboolean        g_vfs_afp_reply_seek              (GVfsAfpReply *reply, gint offset, GSeekType type);
gboolean        g_vfs_afp_reply_skip_to_even      (GVfsAfpReply *reply);

AfpResultCode   g_vfs_afp_reply_get_result_code   (GVfsAfpReply *reply);
goffset         g_vfs_afp_reply_get_pos           (GVfsAfpReply *reply);
gsize           g_vfs_afp_reply_get_size          (GVfsAfpReply *reply);

GType           g_vfs_afp_reply_get_type         (void) G_GNUC_CONST;


/*
 * GVfsAfpCommand
 */
#define G_VFS_TYPE_AFP_COMMAND             (g_vfs_afp_command_get_type ())
#define G_VFS_AFP_COMMAND(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_AFP_COMMAND, GVfsAfpCommand))
#define G_VFS_AFP_COMMAND_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_AFP_COMMAND, GVfsAfpCommandClass))
#define G_VFS_IS_AFP_COMMAND(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_AFP_COMMAND))
#define G_VFS_IS_AFP_COMMAND_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_AFP_COMMAND))
#define G_VFS_AFP_COMMAND_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_AFP_COMMAND, GVfsAfpCommandClass))

typedef struct _GVfsAfpCommandClass GVfsAfpCommandClass;
typedef struct _GVfsAfpCommand GVfsAfpCommand;


GVfsAfpCommand* g_vfs_afp_command_new          (AfpCommandType type);

void            g_vfs_afp_command_put_pascal   (GVfsAfpCommand *command, const char *str);
void            g_vfs_afp_command_put_afp_name (GVfsAfpCommand *command, GVfsAfpName *afp_name);

void            g_vfs_afp_command_pad_to_even  (GVfsAfpCommand *command);

gsize           g_vfs_afp_command_get_size     (GVfsAfpCommand *command);
char*           g_vfs_afp_command_get_data     (GVfsAfpCommand *command);

GType           g_vfs_afp_command_get_type (void) G_GNUC_CONST;




/*
 * GVfsAfpConnection
 */
#define G_VFS_TYPE_AFP_CONNECTION             (g_vfs_afp_connection_get_type ())
#define G_VFS_AFP_CONNECTION(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_AFP_CONNECTION, GVfsAfpConnection))
#define G_VFS_AFP_CONNECTION_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_AFP_CONNECTION, GVfsAfpConnectionClass))
#define G_IS_VFS_AFP_CONNECTION(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_AFP_CONNECTION))
#define G_IS_VFS_AFP_CONNECTION_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_AFP_CONNECTION))
#define G_VFS_AFP_CONNECTION_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_AFP_CONNECTION, GVfsAfpConnectionClass))

typedef struct _GVfsAfpConnectionClass GVfsAfpConnectionClass;
typedef struct _GVfsAfpConnection GVfsAfpConnection;
typedef struct _GVfsAfpConnectionPrivate GVfsAfpConnectionPrivate;

struct _GVfsAfpConnectionClass
{
	GObjectClass parent_class;
};

struct _GVfsAfpConnection
{
	GObject parent_instance;

  GVfsAfpConnectionPrivate *priv;
};

typedef void (*GVfsAfpConnectionReplyCallback) (GVfsAfpConnection *afp_connection,
                                                GVfsAfpReply      *reply,
                                                GError            *error,
                                                gpointer           user_data);


GType g_vfs_afp_connection_get_type (void) G_GNUC_CONST;

GVfsAfpConnection* g_vfs_afp_connection_new               (GSocketConnectable *addr);

GVfsAfpReply*      g_vfs_afp_connection_get_server_info   (GVfsAfpConnection *afp_connection,
                                                           GCancellable *cancellable,
                                                           GError **error);

gboolean           g_vfs_afp_connection_open              (GVfsAfpConnection *afp_connection,
                                                           GCancellable      *cancellable,
                                                           GError            **error);

gboolean           g_vfs_afp_connection_close             (GVfsAfpConnection *afp_connection,
                                                           GCancellable      *cancellable,
                                                           GError            **error);

gboolean           g_vfs_afp_connection_send_command_sync (GVfsAfpConnection *afp_connection,
                                                           GVfsAfpCommand    *afp_command,
                                                           GCancellable      *cancellable,
                                                           GError            **error);

GVfsAfpReply*      g_vfs_afp_connection_read_reply_sync   (GVfsAfpConnection *afp_connection,
                                                           GCancellable *cancellable,
                                                           GError **error);

void               g_vfs_afp_connection_queue_command     (GVfsAfpConnection *afp_connection,
                                                           GVfsAfpCommand    *command,
                                                           GVfsAfpConnectionReplyCallback reply_cb,
                                                           GCancellable      *cancellable,                                                           
                                                           gpointer user_data);
G_END_DECLS

#endif /* _GVFSAFPCONNECTION_H_ */
