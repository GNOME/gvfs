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
  AFP_COMMAND_GET_SRVR_INFO = 15,
  AFP_COMMAND_LOGIN = 18,
  AFP_COMMAND_WRITE = 33,
  AFP_COMMAND_WRITE_EXT = 61
} AfpCommandType;

typedef enum
{
  AFP_ERROR_NONE = 0,
  AFP_ERROR_NO_MORE_SESSIONS = -1068
} AfpErrorCode;

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

char *          g_vfs_afp_reply_read_pascal      (GVfsAfpReply *reply);
gboolean        g_vfs_afp_reply_seek             (GVfsAfpReply *reply, goffset offset, GSeekType type);

AfpErrorCode    g_vfs_afp_reply_get_error_code   (GVfsAfpReply *reply);

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


GVfsAfpCommand* g_vfs_afp_command_new        (AfpCommandType type);

void            g_vfs_afp_command_put_pascal (GVfsAfpCommand *command, char *str);


GType           g_vfs_afp_command_get_type (void) G_GNUC_CONST;




/*
 * GVfsAfpConnection
 */
#define G_VFS_TYPE_AFP_CONNECTION             (g_vfs_afp_connection_get_type ())
#define G_VFS_AFP_CONNECTION(obj)             (G_VFS_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_VFS_AFP_CONNECTION, GVfsAfpConnection))
#define G_VFS_AFP_CONNECTION_CLASS(klass)     (G_VFS_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_VFS_AFP_CONNECTION, GVfsAfpConnectionClass))
#define G_IS_VFS_AFP_CONNECTION(obj)          (G_VFS_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_VFS_AFP_CONNECTION))
#define G_IS_VFS_AFP_CONNECTION_CLASS(klass)  (G_VFS_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_VFS_AFP_CONNECTION))
#define G_VFS_AFP_CONNECTION_GET_CLASS(obj)   (G_VFS_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_VFS_AFP_CONNECTION, GVfsAfpConnectionClass))

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
  
GType g_vfs_afp_connection_get_type (void) G_GNUC_CONST;

GVfsAfpConnection* g_vfs_afp_connection_new               (GSocketConnectable *addr,
                                                           GCancellable       *cancellable,
                                                           GError             **error);

gboolean           g_vfs_afp_connection_send_command_sync (GVfsAfpConnection *afp_connection,
                                                           GVfsAfpCommand    *afp_command,
                                                           GCancellable      *cancellable,
                                                           GError            **error);

GVfsAfpReply*      g_vfs_afp_connection_read_reply_sync   (GVfsAfpConnection *afp_connection,
                                                           GCancellable *cancellable,
                                                           GError **error);
G_END_DECLS

#endif /* _GVFSAFPCONNECTION_H_ */
