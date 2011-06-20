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

enum {
  kTextEncodingMacRoman = 0,
  kTextEncodingMacJapanese = 1,
  kTextEncodingMacChineseTrad = 2,
  kTextEncodingMacKorean = 3,
  kTextEncodingMacArabic = 4,
  kTextEncodingMacHebrew = 5,
  kTextEncodingMacGreek = 6,
  kTextEncodingMacCyrillic = 7,
  kTextEncodingMacDevanagari = 9,
  kTextEncodingMacGurmukhi = 10,
  kTextEncodingMacGujarati = 11,
  kTextEncodingMacOriya = 12,
  kTextEncodingMacBengali = 13,
  kTextEncodingMacTamil = 14,
  kTextEncodingMacTelugu = 15,
  kTextEncodingMacKannada = 16,
  kTextEncodingMacMalayalam = 17,
  kTextEncodingMacSinhalese = 18,
  kTextEncodingMacBurmese = 19,
  kTextEncodingMacKhmer = 20,
  kTextEncodingMacThai = 21,
  kTextEncodingMacLaotian = 22,
  kTextEncodingMacGeorgian = 23,
  kTextEncodingMacArmenian = 24,
  kTextEncodingMacChineseSimp = 25,
  kTextEncodingMacTibetan = 26,
  kTextEncodingMacMongolian = 27,
  kTextEncodingMacEthiopic = 28,
  kTextEncodingMacCentralEurRoman = 29,
  kTextEncodingMacVietnamese = 30,
  kTextEncodingMacExtArabic = 31,
  kTextEncodingMacSymbol = 33,
  kTextEncodingMacDingbats = 34,
  kTextEncodingMacTurkish = 35,
  kTextEncodingMacCroatian = 36,
  kTextEncodingMacIcelandic = 37,
  kTextEncodingMacRomanian = 38,
  kTextEncodingMacCeltic = 39,
  kTextEncodingMacGaelic = 40,
  kTextEncodingMacKeyboardGlyphs = 41,
  kTextEncodingMacUnicode = 126,
  kTextEncodingMacFarsi = 140,
  kTextEncodingMacUkrainian = 152,
  kTextEncodingMacInuit = 236,
  kTextEncodingMacVT100 = 252,
  kTextEncodingMacHFS = 255,
  kTextEncodingUnicodeDefault = 256,
  kTextEncodingUnicodeV1_1 = 257,
  kTextEncodingISO10646_1993 = 257,
  kTextEncodingUnicodeV2_0 = 259,
  kTextEncodingUnicodeV2_1 = 259,
  kTextEncodingUnicodeV3_0 = 260,
  kTextEncodingISOLatin1 = 513,
  kTextEncodingISOLatin2 = 514,
  kTextEncodingISOLatin3 = 515,
  kTextEncodingISOLatin4 = 516,
  kTextEncodingISOLatinCyrillic = 517,
  kTextEncodingISOLatinArabic = 518,
  kTextEncodingISOLatinGreek = 519,
  kTextEncodingISOLatinHebrew = 520,
  kTextEncodingISOLatin5 = 521,
  kTextEncodingISOLatin6 = 522,
  kTextEncodingISOLatin7 = 525,
  kTextEncodingISOLatin8 = 526,
  kTextEncodingISOLatin9 = 527,
  kTextEncodingDOSLatinUS = 1024,
  kTextEncodingDOSGreek = 1029,
  kTextEncodingDOSBalticRim = 1030,
  kTextEncodingDOSLatin1 = 1040,
  kTextEncodingDOSGreek1 = 1041,
  kTextEncodingDOSLatin2 = 1042,
  kTextEncodingDOSCyrillic = 1043,
  kTextEncodingDOSTurkish = 1044,
  kTextEncodingDOSPortuguese = 1045,
  kTextEncodingDOSIcelandic = 1046,
  kTextEncodingDOSHebrew = 1047,
  kTextEncodingDOSCanadianFrench = 1048,
  kTextEncodingDOSArabic = 1049,
  kTextEncodingDOSNordic = 1050,
  kTextEncodingDOSRussian = 1051,
  kTextEncodingDOSGreek2 = 1052,
  kTextEncodingDOSThai = 1053,
  kTextEncodingDOSJapanese = 1056,
  kTextEncodingDOSChineseSimplif = 1057,
  kTextEncodingDOSKorean = 1058,
  kTextEncodingDOSChineseTrad = 1059,
  kTextEncodingWindowsLatin1 = 1280,
  kTextEncodingWindowsANSI = 1280,
  kTextEncodingWindowsLatin2 = 1281,
  kTextEncodingWindowsCyrillic = 1282,
  kTextEncodingWindowsGreek = 1283,
  kTextEncodingWindowsLatin5 = 1284,
  kTextEncodingWindowsHebrew = 1285,
  kTextEncodingWindowsArabic = 1286,
  kTextEncodingWindowsBalticRim = 1287,
  kTextEncodingWindowsVietnamese = 1288,
  kTextEncodingWindowsKoreanJohab = 1296,
  kTextEncodingUS_ASCII = 1536,
  kTextEncodingJIS_X0201_76 = 1568,
  kTextEncodingJIS_X0208_83 = 1569,
  kTextEncodingJIS_X0208_90 = 1570
};

typedef enum
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
  AFP_DIR_BITMAP_OFFSPRING_COUNT_BIT    = 0x0200,
  AFP_DIR_BITMAP_OWNER_ID_BIT           = 0x0400,
  AFP_DIR_BITMAP_GROUP_ID_BIT           = 0x0800,
  AFP_DIR_BITMAP_ACCESS_RIGHTS_BIT      = 0x1000,
  AFP_DIR_BITMAP_UTF8_NAME_BIT          = 0x2000,
  AFP_DIR_BITMAP_UNIX_PRIVS_BIT         = 0x8000,
  AFP_DIR_BITMAP_UUID_BIT               = 0x10000 // AFP version 3.2 and later (with ACL support)
} AfpDirBitmap;

typedef enum
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
  AFP_FILE_BITMAP_DATA_FORK_LEN_BIT      = 0x0200,
  AFP_FILE_BITMAP_RSRC_FORK_LEN_BI       = 0x0400,
  AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT  = 0x0800,
  AFP_FILE_BITMAP_LAUNCH_LIMIT_BIT       = 0x1000,
  AFP_FILE_BITMAP_UTF8_NAME_BIT          = 0x2000,
  AFP_FILE_BITMAP_EXT_RSRC_FORK_LEN_BIT  = 0x4000,
  AFP_FILE_BITMAP_UNIX_PRIVS_BIT         = 0x8000
} AfpFileBitmap;

typedef enum
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
} AfpVolumeBitmap;

typedef enum
{
  AFP_COMMAND_GET_SRVR_INFO = 15,
  AFP_COMMAND_GET_SRVR_PARMS = 16,
  AFP_COMMAND_GET_VOL_PARMS = 17,
  AFP_COMMAND_LOGIN = 18,
  AFP_COMMAND_LOGIN_CONT = 19,
  AFP_COMMAND_OPEN_VOL = 24,
  AFP_COMMAND_WRITE = 33,
  AFP_COMMAND_WRITE_EXT = 61,
  AFP_COMMAND_ENUMERATE_EXT = 66,
  AFP_COMMAND_ENUMERATE_EXT2 = 68
} AfpCommandType;

typedef enum
{
  AFP_RESULT_NO_ERROR = 0,
  AFP_RESULT_USER_NOT_AUTH = -5023,
  AFP_RESULT_AUTH_CONTINUE = -5001,
  AFP_RESULT_NO_MORE_SESSIONS = -1068
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

GVfsAfpName* g_vfs_afp_name_new              (guint32 text_encoding, const gchar *str, gsize len);
GVfsAfpName* g_vfs_afp_name_new_from_gstring (guint32 text_encoding, GString *string);

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

gboolean        g_vfs_afp_reply_read_int32        (GVfsAfpReply *reply, gint32 *val);
gboolean        g_vfs_afp_reply_read_int16        (GVfsAfpReply *reply, gint16 *val);

gboolean        g_vfs_afp_reply_read_uint32       (GVfsAfpReply *reply, guint32 *val);
gboolean        g_vfs_afp_reply_read_uint16       (GVfsAfpReply *reply, guint16 *val);

gboolean        g_vfs_afp_reply_get_data          (GVfsAfpReply *reply, guint size, guint8 **data);
gboolean        g_vfs_afp_reply_dup_data          (GVfsAfpReply *reply, guint size, guint8 **data);

gboolean        g_vfs_afp_reply_read_pascal       (GVfsAfpReply *reply, char **str);
gboolean        g_vfs_afp_reply_read_afp_name     (GVfsAfpReply *reply, gboolean read_text_encoding, GVfsAfpName **afp_name);

gboolean        g_vfs_afp_reply_seek              (GVfsAfpReply *reply, gint offset, GSeekType type);
gboolean        g_vfs_afp_reply_skip_to_even      (GVfsAfpReply *reply);

AfpResultCode   g_vfs_afp_reply_get_result_code   (GVfsAfpReply *reply);

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


GVfsAfpCommand* g_vfs_afp_command_new         (AfpCommandType type);

void            g_vfs_afp_command_put_pascal  (GVfsAfpCommand *command, const char *str);
void            g_vfs_afp_command_pad_to_even (GVfsAfpCommand *command);

gsize           g_vfs_afp_command_get_size    (GVfsAfpCommand *command);
char*           g_vfs_afp_command_get_data    (GVfsAfpCommand *command);

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
