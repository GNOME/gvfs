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

#include <config.h>

#include <stdlib.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#ifdef HAVE_GCRYPT
#include <gcrypt.h>
#endif

#include "gvfsjobmount.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobsetattribute.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobcloseread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobdelete.h"
#include "gvfsjobmakedirectory.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobmove.h"
#include "gvfsjobcopy.h"

#include "gvfsafpserver.h"
#include "gvfsafpconnection.h"

#include "gvfsbackendafp.h"

#define G_FILE_ATTRIBUTE_AFP_NODE_ID        "afp::node-id"
#define G_FILE_ATTRIBUTE_AFP_PARENT_DIR_ID  "afp::parent-dir-id"
#define G_FILE_ATTRIBUTE_AFP_CHILDREN_COUNT "afp::children-count"
#define G_FILE_ATTRIBUTE_AFP_UA_PERMISSIONS "afp::ua-permisssions"

static const gint16 ENUMERATE_REQ_COUNT           = G_MAXINT16;
static const gint16 ENUMERATE_EXT_MAX_REPLY_SIZE  = G_MAXINT16; 
static const gint32 ENUMERATE_EXT2_MAX_REPLY_SIZE = G_MAXINT32;

struct _GVfsBackendAfpClass
{
  GVfsBackendClass parent_class;
};

struct _GVfsBackendAfp
{
  GVfsBackend parent_instance;

  GNetworkAddress    *addr;
  char               *volume;
  char               *user;

  GVfsAfpServer      *server;
  
  guint16             vol_attrs_bitmap;
  guint16             volume_id;

  guint32             user_id;
  guint32             group_id;
};


G_DEFINE_TYPE (GVfsBackendAfp, g_vfs_backend_afp, G_VFS_TYPE_BACKEND);


/*
 * Utility functions
 */
static void
job_failed_from_afp_result_code (GVfsJob *job, AfpResultCode res_code)
{
  GError *err;
  
  err = afp_result_code_to_gerror (res_code);
  g_vfs_job_failed_from_error (job, err);
  g_error_free (err);
}

static gboolean
is_root (const char *filename)
{
  const char *p;

  p = filename;
  while (*p == '/')
    p++;

  return *p == 0;
}

static void
copy_file_info_into (GFileInfo *src, GFileInfo *dest)
{
  char **attrs;
  gint i;

  attrs = g_file_info_list_attributes (src, NULL);

  for (i = 0; attrs[i]; i++)
  {
    GFileAttributeType type;
    gpointer value;
    
    g_file_info_get_attribute_data (src, attrs[i], &type, &value, NULL);
    g_file_info_set_attribute (dest, attrs[i], type, value);
  }

  g_strfreev (attrs);
}

static GVfsAfpName *
filename_to_afp_pathname (const char *filename)
{
  gsize len;
  char *str;
  gint i;

  while (*filename == '/')
    filename++;
  
  len = strlen (filename);
  
  str = g_malloc (len); 

  for (i = 0; i < len; i++)
  {
    if (filename[i] == '/')
      str[i] = '\0';
    else
      str[i] = filename[i];
  }
  

  return g_vfs_afp_name_new (0x08000103, str, len);
}

static void
put_pathname (GVfsAfpCommand *comm, const char *filename)
{
  GVfsAfpName *pathname;
  
  /* PathType */
  g_vfs_afp_command_put_byte (comm, AFP_PATH_TYPE_UTF8_NAME);

  /* Pathname */
  pathname = filename_to_afp_pathname (filename);
  g_vfs_afp_command_put_afp_name (comm, pathname);
  g_vfs_afp_name_unref (pathname);
}

typedef enum
{
  AFP_HANDLE_TYPE_READ_FILE,
  AFP_HANDLE_TYPE_CREATE_FILE,
  AFP_HANDLE_TYPE_REPLACE_FILE_TEMP,
  AFP_HANDLE_TYPE_REPLACE_FILE_DIRECT,
  AFP_HANDLE_TYPE_APPEND_TO_FILE
} AfpHandleType;

typedef struct
{
  AfpHandleType type;
  gint16 fork_refnum;
  gint64 offset;
  
  /* Used if type == AFP_HANDLE_TYPE_REPLACE_FILE_DIRECT */
  gint64 size;
  
  char *filename;
  char *tmp_filename;
} AfpHandle;

static AfpHandle *
afp_handle_new (gint16 fork_refnum)
{
  AfpHandle *afp_handle;

  afp_handle = g_slice_new0 (AfpHandle);
  afp_handle->fork_refnum = fork_refnum;

  return afp_handle;
}

static void
afp_handle_free (AfpHandle *afp_handle)
{
  g_free (afp_handle->filename);
  g_free (afp_handle->tmp_filename);
  
  g_slice_free (AfpHandle, afp_handle);
}

static void
set_access_attributes_trusted (GFileInfo *info,
                               guint32 perm)
{
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
				     perm & 0x4);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
				     perm & 0x2);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				     perm & 0x1);
}

/* For files we don't own we can't trust a negative response to this check, as
   something else could allow us to do the operation, for instance an ACL
   or some sticky bit thing */
static void
set_access_attributes (GFileInfo *info,
                       guint32 perm)
{
  if (perm & 0x4)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
				       TRUE);
  if (perm & 0x2)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
				       TRUE);
  if (perm & 0x1)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				       TRUE);
}

static void fill_info (GVfsBackendAfp *afp_backend,
                       GFileInfo *info, GVfsAfpReply *reply,
                       gboolean directory, guint16 bitmap)
{
  goffset start_pos;

  if (directory)
  {
    GIcon *icon;
    
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, "inode/directory");

    icon = g_themed_icon_new ("folder");
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
  }
  else
    g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);

  
  start_pos = g_vfs_afp_reply_get_pos (reply);

  if (bitmap & AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT)
  {
    guint16 attributes;

    g_vfs_afp_reply_read_uint16 (reply, &attributes);
    
    if (attributes & AFP_FILEDIR_ATTRIBUTES_BITMAP_INVISIBLE_BIT)
      g_file_info_set_is_hidden (info, TRUE);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_PARENT_DIR_ID_BIT)
  {
    guint32 parent_dir_id;

    g_vfs_afp_reply_read_uint32 (reply, &parent_dir_id);
    g_file_info_set_attribute_uint32 (info, "afp::parent-dir-id", parent_dir_id);
  }
  
  if (bitmap & AFP_FILEDIR_BITMAP_CREATE_DATE_BIT)
  {
    gint32 create_date;
    gint64 create_date_local;

    g_vfs_afp_reply_read_int32 (reply, &create_date);
    
    create_date_local = g_vfs_afp_server_time_to_local_time (afp_backend->server,
                                                             create_date);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED,
                                      create_date_local);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_MOD_DATE_BIT)
  {
    gint32 mod_date;
    guint64 mod_date_unix;
    char *etag;

    g_vfs_afp_reply_read_int32 (reply, &mod_date);
    mod_date_unix = g_vfs_afp_server_time_to_local_time (afp_backend->server,
                                                         mod_date);
    
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                      mod_date_unix);

    etag = g_strdup_printf ("%"G_GUINT64_FORMAT, mod_date_unix);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
    g_free (etag);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_NODE_ID_BIT)
  {
    guint32 node_id;

    g_vfs_afp_reply_read_uint32 (reply, &node_id);
    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_NODE_ID, node_id);
  }
  
  /* Directory specific attributes */
  if (directory)
  {
    if (bitmap & AFP_DIR_BITMAP_OFFSPRING_COUNT_BIT)
    {
      guint16 offspring_count;

      g_vfs_afp_reply_read_uint16 (reply, &offspring_count);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_CHILDREN_COUNT,
                                        offspring_count);
    }
  }
  
  /* File specific attributes */
  else
  {
    if (bitmap & AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT)
    {
      guint64 fork_len;

      g_vfs_afp_reply_read_uint64 (reply, &fork_len);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        fork_len);
    }
  }
  
  if (bitmap & AFP_FILEDIR_BITMAP_UTF8_NAME_BIT)
  {
    guint16 UTF8Name_offset;
    goffset old_pos;
    GVfsAfpName *afp_name;
    char *utf8_name;

    g_vfs_afp_reply_read_uint16 (reply, &UTF8Name_offset);
    /* Pad */
    g_vfs_afp_reply_read_uint32 (reply, NULL);

    old_pos = g_vfs_afp_reply_get_pos (reply);
    g_vfs_afp_reply_seek (reply, start_pos + UTF8Name_offset, G_SEEK_SET);

    g_vfs_afp_reply_read_afp_name (reply, TRUE, &afp_name);
    utf8_name = g_vfs_afp_name_get_string (afp_name);    
    g_vfs_afp_name_unref (afp_name);

    g_file_info_set_name (info, utf8_name);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                      utf8_name);

    /* Set file as hidden if it begins with a dot */
    if (utf8_name[0] == '.')
      g_file_info_set_is_hidden (info, TRUE);

    if (!directory)
    {
      char *content_type;
      GIcon *icon;

      content_type = g_content_type_guess (utf8_name, NULL, 0, NULL);
      g_file_info_set_content_type (info, content_type);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
                                        content_type);

      icon = g_content_type_get_icon (content_type);
      g_file_info_set_icon (info, icon);

      g_object_unref (icon);
      g_free (content_type);
    }
    
    g_free (utf8_name);

    g_vfs_afp_reply_seek (reply, old_pos, G_SEEK_SET);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT)
  {
    guint32 uid, gid, permissions, ua_permissions;

    g_vfs_afp_reply_read_uint32 (reply, &uid);
    g_vfs_afp_reply_read_uint32 (reply, &gid);
    g_vfs_afp_reply_read_uint32 (reply, &permissions);
    /* ua_permissions */
    g_vfs_afp_reply_read_uint32 (reply, &ua_permissions);

    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, permissions);
    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, uid);
    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, gid);

    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_UA_PERMISSIONS,
                                      ua_permissions);
    
    if (uid == afp_backend->user_id)
      set_access_attributes_trusted (info, (permissions >> 6) & 0x7);
    else if (gid == afp_backend->group_id)
      set_access_attributes (info, (permissions >> 3) & 0x7);
    else
      set_access_attributes (info, (permissions >> 0) & 0x7);
  }
}

typedef struct
{
  gint16 fork_refnum;
  GFileInfo *info;
} OpenForkData;

static void
open_fork_data_free (OpenForkData *data)
{
  g_object_unref (data->info);

  g_slice_free (OpenForkData, data);
}

static void
open_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  
  GVfsBackendAfp *afp_backend;
  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  OpenForkData *data;
  guint16 file_bitmap;

  afp_backend = G_VFS_BACKEND_AFP (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));
  
  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("File doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_TYPE_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                         _("File is directory"));
        break;
      case AFP_RESULT_TOO_MANY_FILES_OPEN:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                                         _("Too many files open"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

  data = g_slice_new (OpenForkData);
  
  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_int16  (reply, &data->fork_refnum);

  data->info = g_file_info_new ();
  fill_info (afp_backend, data->info, reply, FALSE, file_bitmap);
  g_object_unref (reply);

  g_simple_async_result_set_op_res_gpointer (simple, data,
                                             (GDestroyNotify)open_fork_data_free);

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
open_fork (GVfsBackendAfp     *afp_backend,
           const char         *filename,
           guint16             access_mode,
           guint16             bitmap,
           GCancellable       *cancellable,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  if (is_root (filename))
  {
    g_simple_async_report_error_in_idle (G_OBJECT (afp_backend), callback,
                                         user_data, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                         _("File is directory"));
    return;
  }

  comm = g_vfs_afp_command_new (AFP_COMMAND_OPEN_FORK);
  /* data fork */
  g_vfs_afp_command_put_byte (comm, 0);

  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* Directory ID */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* Bitmap */
  g_vfs_afp_command_put_uint16 (comm, bitmap);

  /* AccessMode */
  g_vfs_afp_command_put_uint16 (comm, access_mode);

  /* Pathname */
  put_pathname (comm, filename);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback,
                                      user_data, open_fork);
  
  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     open_fork_cb, cancellable, simple);
  g_object_unref (comm);
}

static gboolean
open_fork_finish (GVfsBackendAfp *afp_backend,
                  GAsyncResult   *res,
                  gint16         *fork_refnum,
                  GFileInfo      **info,
                  GError         **error)
{
  GSimpleAsyncResult *simple;
  OpenForkData *data;

  g_return_val_if_fail (g_simple_async_result_is_valid (res,
                                                        G_OBJECT (afp_backend),
                                                        open_fork),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  if (fork_refnum)
    *fork_refnum = data->fork_refnum;
  if (info)
    *info = g_object_ref (data->info);

  return TRUE;
}

static void
close_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  
  if (res_code != AFP_RESULT_NO_ERROR)
    g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
close_fork (GVfsBackendAfp      *afp_backend,
            gint16               fork_refnum,
            GCancellable        *cancellable,
            GAsyncReadyCallback  callback,
            gpointer             user_data)
{
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  comm = g_vfs_afp_command_new (AFP_COMMAND_CLOSE_FORK);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, fork_refnum);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback, user_data,
                                      close_fork);
  
  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                      close_fork_cb, cancellable,
                                      simple);
  g_object_unref (comm);
}

static gboolean
close_fork_finish (GVfsBackendAfp *afp_backend,
                   GAsyncResult   *result,
                   GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (afp_backend),
                                                        close_fork),
                        FALSE);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static void
get_fork_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  guint16 file_bitmap;
  GFileInfo *info;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
    goto done;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);

  info = g_file_info_new ();
  fill_info (afp_backend, info, reply, FALSE, file_bitmap);

  g_object_unref (reply);

  g_simple_async_result_set_op_res_gpointer (simple, info, g_object_unref);

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
get_fork_parms (GVfsBackendAfp      *afp_backend,
                gint16               fork_refnum,
                guint16              file_bitmap,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_FORK_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, fork_refnum);
  /* Bitmap */  
  g_vfs_afp_command_put_uint16 (comm, file_bitmap);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback, user_data,
                                      get_fork_parms);
                                      
  
  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                      get_fork_parms_cb, cancellable,
                                      simple);
  g_object_unref (comm);
}

static GFileInfo *
get_fork_parms_finish (GVfsBackendAfp *afp_backend,
                       GAsyncResult   *result,
                       GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (afp_backend),
                                                        get_fork_parms),
                        NULL);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  return g_object_ref (g_simple_async_result_get_op_res_gpointer (simple));
}

static void
get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  guint16 file_bitmap, dir_bitmap, bitmap;
  guint8 FileDir;
  gboolean directory;
  GFileInfo *info;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    
    switch (res_code)
    {
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("File doesn't exist"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_uint16 (reply, &dir_bitmap);

  g_vfs_afp_reply_read_byte (reply, &FileDir);
  /* Pad Byte */
  g_vfs_afp_reply_read_byte (reply, NULL);
  
  directory = (FileDir & 0x80); 
  bitmap =  directory ? dir_bitmap : file_bitmap;

  info = g_file_info_new ();
  fill_info (afp_backend, info, reply, directory, bitmap);
  
  g_object_unref (reply);

  g_simple_async_result_set_op_res_gpointer (simple, info, g_object_unref);

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
get_filedir_parms (GVfsBackendAfp      *afp_backend,
                   const char          *filename,
                   guint16              file_bitmap,
                   guint16              dir_bitmap,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_FILE_DIR_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* VolumeID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* Directory ID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);
  /* FileBitmap */  
  g_vfs_afp_command_put_uint16 (comm, file_bitmap);
  /* DirectoryBitmap */  
  g_vfs_afp_command_put_uint16 (comm, dir_bitmap);
  /* PathName */
  put_pathname (comm, filename);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback, user_data,
                                      get_filedir_parms);
                                      

  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     get_filedir_parms_cb, cancellable,
                                     simple);
  g_object_unref (comm);
}

static GFileInfo *
get_filedir_parms_finish (GVfsBackendAfp *afp_backend,
                          GAsyncResult   *result,
                          GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (afp_backend),
                                                        get_filedir_parms),
                        NULL);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  return g_object_ref (g_simple_async_result_get_op_res_gpointer (simple));
}

typedef struct
{
  char *filename;
  gboolean hard_create;
  GCancellable *cancellable;
} CreateFileData;

static void
free_create_file_data (CreateFileData *cfd)
{
  g_free (cfd->filename);
  if (cfd->cancellable)
    g_object_unref (cfd->cancellable);

  g_slice_free (CreateFileData, cfd);
}

static void
create_file_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  
  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                  _("Not enough space on volume"));
        break;
      case AFP_RESULT_FILE_BUSY:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                  _("Target file is open"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                  _("Target file already exists"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("Ancestor directory doesn't exist"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Volume is read-only"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
create_file_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  CreateFileData *cfd = g_simple_async_result_get_op_res_gpointer (simple);
  
  GFileInfo *info;
  GError *err = NULL;

  guint32 dir_id;
  char *basename;
  GVfsAfpCommand *comm;

  info = get_filedir_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_simple_async_result_take_error (simple, err);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
    return;
  }

  dir_id = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_NODE_ID);
  g_object_unref (info);

  comm = g_vfs_afp_command_new (AFP_COMMAND_CREATE_FILE);
  /* soft/hard create */
  g_vfs_afp_command_put_byte (comm, cfd->hard_create ? 0x80 : 0x00);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* Directory ID */
  g_vfs_afp_command_put_uint32 (comm, dir_id);

  /* Pathname */
  basename = g_path_get_basename (cfd->filename);
  put_pathname (comm, basename);
  g_free (basename);

  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     create_file_cb, cfd->cancellable, simple);
  g_object_unref (comm);
}

static void
create_file (GVfsBackendAfp     *afp_backend,
             const char         *filename,
             gboolean            hard_create,
             GCancellable       *cancellable,
             GAsyncReadyCallback callback,
             gpointer            user_data)
{
  CreateFileData *cfd;
  GSimpleAsyncResult *simple;
  char *dirname;

  cfd = g_slice_new0 (CreateFileData);
  cfd->filename = g_strdup (filename);
  cfd->hard_create = hard_create;
  if (cancellable)
    cfd->cancellable = g_object_ref (cancellable);
  
  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback, user_data,
                                      create_file);
  g_simple_async_result_set_op_res_gpointer (simple, cfd,
                                             (GDestroyNotify)free_create_file_data);

  dirname = g_path_get_dirname (filename);
  get_filedir_parms (afp_backend, dirname, 0, AFP_DIR_BITMAP_NODE_ID_BIT,
                     cancellable, create_file_get_filedir_parms_cb, simple);
  g_free (dirname);
}

static gboolean
create_file_finish (GVfsBackendAfp *afp_backend,
                    GAsyncResult   *result,
                    GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (afp_backend),
                                                        create_file),
                        FALSE);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static void
delete_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_FILE_BUSY:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_BUSY,
                                         _("Target file is open"));
        break;                           
      case AFP_RESULT_DIR_NOT_EMPTY:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY,
                                  _("Directory not empty"));
        break;
      case AFP_RESULT_OBJECT_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("Target object is marked as not deletable (DeleteInhibit)"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("Target object doesn't exist"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Volume is read-only"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
delete (GVfsBackendAfp      *afp_backend,
        const char          *filename,
        GCancellable        *cancellable,
        GAsyncReadyCallback  callback,
        gpointer             user_data)
{
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  comm = g_vfs_afp_command_new (AFP_COMMAND_DELETE);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* Directory ID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* Pathname */
  put_pathname (comm, filename);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback,
                                      user_data, delete);
  
  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     delete_cb, cancellable, simple);
  g_object_unref (comm);
}

static gboolean
delete_finish (GVfsBackendAfp *afp_backend,
               GAsyncResult   *result,
               GError         **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (afp_backend),
                                                        delete),
                        FALSE);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

typedef struct
{
  AfpMapIDFunction function;
  char *name;
} MapIDData;

static void
map_id_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;

  AfpResultCode res_code;
  MapIDData *map_data;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    g_simple_async_result_complete (simple);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ITEM_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("ID not found"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }

    g_simple_async_result_complete (simple);
    return;
  }

  map_data = g_simple_async_result_get_op_res_gpointer (simple);
  
  if (map_data->function == AFP_MAP_ID_FUNCTION_USER_UUID_TO_UTF8_NAME ||
      map_data->function == AFP_MAP_ID_FUNCTION_GROUP_UUID_TO_UTF8_NAME)
  {
    /* objType */
    g_vfs_afp_reply_read_uint32 (reply, NULL);
    /* id */
    g_vfs_afp_reply_read_uint32 (reply, NULL);
  }

  if (map_data->function == AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME ||
      map_data->function == AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME)
  {
    g_vfs_afp_reply_read_pascal (reply, &map_data->name);
  }
  else
  {
    GVfsAfpName *afp_name;

    g_vfs_afp_reply_read_afp_name (reply, FALSE, &afp_name);
    map_data->name = g_vfs_afp_name_get_string (afp_name);
    g_vfs_afp_name_unref (afp_name);
  }

  g_simple_async_result_complete (simple);
}

static void
map_id (GVfsBackendAfp      *afp_backend,
        AfpMapIDFunction     map_function,
        gint64               id,
        GCancellable        *cancellable,
        GAsyncReadyCallback  callback,
        gpointer             user_data)
{
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;
  MapIDData *map_data;

  comm = g_vfs_afp_command_new (AFP_COMMAND_MAP_ID);

  /* SubFunction*/
  g_vfs_afp_command_put_byte (comm, map_function);

  /* ID */
  if (map_function == AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME ||
      map_function == AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME)
    g_vfs_afp_command_put_int32 (comm, id);
  else
    g_vfs_afp_command_put_int64 (comm, id);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback,
                                      user_data, map_id);

  map_data = g_new (MapIDData, 1);
  map_data->function = map_function;
  g_simple_async_result_set_op_res_gpointer (simple, map_data, g_free);
  
  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     map_id_cb, cancellable, simple);
  g_object_unref (comm);
}

static char *
map_id_finish (GVfsBackendAfp   *afp_backend,
               GAsyncResult     *res,
               AfpMapIDFunction *map_function,
               GError          **error)
{
  GSimpleAsyncResult *simple;
  MapIDData *map_data;

  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (afp_backend),
                                                        map_id),
                        NULL);

  simple = (GSimpleAsyncResult *)res;

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  map_data = g_simple_async_result_get_op_res_gpointer (simple);

  if (map_function)
    *map_function = map_data->function;
  
  return map_data->name;
}

static void
move_and_rename_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;

  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_CANT_MOVE:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                                         _("Can't move directory into one of its descendants"));
        break;
      case AFP_RESULT_INSIDE_SHARE_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Can't move sharepoint into a shared directory"));
        break;
      case AFP_RESULT_INSIDE_TRASH_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Can't move a shared directory into the Trash"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                         _("Target file already exists"));
        break;
      case AFP_RESULT_OBJECT_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Object being moved is marked as not renameable (RenameInhibit)"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Object being moved doesn't exist"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
move_and_rename (GVfsBackendAfp     *afp_backend,
                 const char         *source,
                 const char         *destination,
                 GCancellable       *cancellable,
                 GAsyncReadyCallback callback,
                 gpointer            user_data)
{
  GVfsAfpCommand *comm;
  char *dirname, *basename;
  GSimpleAsyncResult *simple;

  comm = g_vfs_afp_command_new (AFP_COMMAND_MOVE_AND_RENAME);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* VolumeID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);

  /* SourceDirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);
  /* DestDirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* SourcePathname */
  put_pathname (comm, source);

  /* DestPathname */
  dirname = g_path_get_dirname (destination);
  put_pathname (comm, dirname);
  g_free (dirname);

  /* NewName */
  basename = g_path_get_basename (destination);
  put_pathname (comm, basename);
  g_free (basename);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback,
                                      user_data, move_and_rename);
  
  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     move_and_rename_cb, cancellable, simple);
  g_object_unref (comm);
}

static gboolean
move_and_rename_finish (GVfsBackendAfp *afp_backend,
                        GAsyncResult   *res,
                        GError        **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (afp_backend),
                                                        move_and_rename),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static void
copy_file_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;

  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);

  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_CALL_NOT_SUPPORTED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                         _("Server doesn't support the FPCopyFile operation"));
        break;
      case AFP_RESULT_DENY_CONFLICT:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Unable to open source file for reading"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                         _("Not enough space on volume"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                         _("Target file already exists"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Source file and/or destination directory doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_TYPE_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                         _("Source file is a directory"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
  }
        
done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
copy_file (GVfsBackendAfp     *afp_backend,
           const char         *source,
           const char         *destination,
           GCancellable       *cancellable,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
  GVfsAfpCommand *comm;
  char *dirname, *basename;
  GSimpleAsyncResult *simple;

  comm = g_vfs_afp_command_new (AFP_COMMAND_COPY_FILE);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* SourceVolumeID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* SourceDirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* DestVolumeID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* DestDirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* SourcePathname */
  put_pathname (comm, source);

  /* DestPathname */
  dirname = g_path_get_dirname (destination);
  put_pathname (comm, dirname);
  g_free (dirname);

  /* NewName */
  basename = g_path_get_basename (destination);
  put_pathname (comm, basename);
  g_free (basename);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback,
                                      user_data, copy_file);

  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     copy_file_cb, cancellable, simple);
  g_object_unref (comm);
}

static gboolean
copy_file_finish (GVfsBackendAfp *afp_backend, GAsyncResult *res, GError **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (afp_backend),
                                                        copy_file),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static void
set_unix_privs_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Target object doesn't exist"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Volume is read-only"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
set_unix_privs (GVfsBackendAfp      *afp_backend,
                const char          *filename,
                guint32              uid,
                guint32              gid,
                guint32              permissions,
                guint32              ua_permissions,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  comm = g_vfs_afp_command_new (AFP_COMMAND_SET_FILEDIR_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* VolumeID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* DirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);
  /* Bitmap */
  g_vfs_afp_command_put_uint16 (comm, AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT);
  /* Pathname */
  put_pathname (comm, filename);
  /* pad to even */
  g_vfs_afp_command_pad_to_even (comm);

  /* UID */
  g_vfs_afp_command_put_uint32 (comm, uid);
  /* GID */
  g_vfs_afp_command_put_uint32 (comm, gid);
  /* Permissions */
  g_vfs_afp_command_put_uint32 (comm, permissions);
  /* UAPermissions */
  g_vfs_afp_command_put_uint32 (comm, ua_permissions);

  simple = g_simple_async_result_new (G_OBJECT (afp_backend), callback,
                                      user_data, set_unix_privs);

  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     set_unix_privs_cb, cancellable, simple);
  g_object_unref (comm);
}

static gboolean
set_unix_privs_finish (GVfsBackendAfp *afp_backend,
                       GAsyncResult   *res,
                       GError        **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (afp_backend),
                                                        set_unix_privs),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

/*
 * Backend code
 */

typedef struct
{
  GVfsJobCopy  *job;
  GAsyncResult *source_parms_res;
  GAsyncResult *dest_parms_res;
} CopyData;

static void
copy_data_free (CopyData *copy_data)
{
  g_object_unref (copy_data->source_parms_res);
  g_object_unref (copy_data->dest_parms_res);

  g_slice_free (CopyData, copy_data);
}

static void
copy_copy_file_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobCopy *job = G_VFS_JOB_COPY (user_data);

  GError *err = NULL;
  
  if (!copy_file_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
copy_delete_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobCopy *job = G_VFS_JOB_COPY (user_data);

  GError *err = NULL;
  
  if (!delete_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  copy_file (afp_backend, job->source, job->destination,
             G_VFS_JOB (job)->cancellable, copy_copy_file_cb, job);
}

static void
do_copy (CopyData *copy_data)
{
  GVfsJobCopy *job = copy_data->job;
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  
  GFileInfo *info;
  GError *err = NULL;

  gboolean source_is_dir;
  gboolean dest_exists;
  gboolean dest_is_dir;
  
  info = get_filedir_parms_finish (afp_backend, copy_data->source_parms_res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    goto done;
  }

  /* If the source is a directory, don't fail with WOULD_RECURSE immediately,
   * as that is less useful to the app. Better check for errors on the
   * target instead.
   */
  source_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? TRUE : FALSE;
  g_object_unref (info);

  info = get_filedir_parms_finish (afp_backend, copy_data->dest_parms_res, &err);
  if (!info)
  {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&err);
      dest_exists = FALSE;
    }
    else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
      g_error_free (err);
      goto done;
    }
  }
  else
  {
    dest_exists = TRUE;
    dest_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? TRUE : FALSE;
    g_object_unref (info);
  }

  /* Check target errors */
  if (dest_exists)
  {
    if ((job->flags & G_FILE_COPY_OVERWRITE))
    {
      /* Always fail on dirs, even with overwrite */
      if (dest_is_dir)
      {
        if (source_is_dir)
          g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                    _("Can't copy directory over directory"));
        else
          g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                    _("File is directory"));
        goto done;
      }
    }
    else
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
      goto done;
    }
  }

  /* Now we fail if the source is a directory */
  if (source_is_dir)
  {
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                      _("Can't recursively copy directory"));
    goto done;
  }

  if (dest_exists)
  {
    delete (afp_backend, job->destination, G_VFS_JOB (job)->cancellable,
            copy_delete_cb, job);
  }
  else
  {
    copy_file (afp_backend, job->source, job->destination,
               G_VFS_JOB (job)->cancellable, copy_copy_file_cb, job);
  }
  
done:
  copy_data_free (copy_data);
  return;
}

static void
copy_get_dest_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  CopyData *copy_data = (CopyData *)user_data;
  
  copy_data->dest_parms_res = g_object_ref (res);
  if (copy_data->source_parms_res)
    do_copy (copy_data);
}

static void
copy_get_source_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  CopyData *copy_data = (CopyData *)user_data;

  copy_data->source_parms_res = g_object_ref (res);
  if (copy_data->dest_parms_res)
    do_copy (copy_data);
}

static gboolean
try_copy (GVfsBackend *backend,
          GVfsJobCopy *job,
          const char *source,
          const char *destination,
          GFileCopyFlags flags,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  CopyData *copy_data;

  copy_data = g_slice_new0 (CopyData);
  copy_data->job = job;

  get_filedir_parms (afp_backend, source, AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                     AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                     G_VFS_JOB (job)->cancellable, copy_get_source_parms_cb,
                     copy_data);

  get_filedir_parms (afp_backend, destination, AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                     AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                     G_VFS_JOB (job)->cancellable, copy_get_dest_parms_cb,
                     copy_data);  

  return TRUE;
}

static void
move_move_and_rename_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobMove *job = G_VFS_JOB_MOVE (user_data);

  GError *err = NULL;
  
  if (!move_and_rename_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
move_delete_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobMove *job = G_VFS_JOB_MOVE (user_data);

  GError *err = NULL;
  
  if (!delete_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  move_and_rename (afp_backend, job->source, job->destination,
                   G_VFS_JOB (job)->cancellable, move_move_and_rename_cb,
                   job);
}

typedef struct
{
  GVfsJobMove *job;
  GAsyncResult *source_parms_res;
  GAsyncResult *dest_parms_res;
} MoveData;

static void
free_move_data (MoveData *move_data)
{
  g_object_unref (move_data->source_parms_res);
  g_object_unref (move_data->dest_parms_res);

  g_slice_free (MoveData, move_data);
}

static void
do_move (MoveData *move_data)
{
  GVfsJobMove *job = move_data->job;
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  
  GFileInfo *info;
  GError *err = NULL;

  gboolean source_is_dir;
  gboolean dest_exists;
  gboolean dest_is_dir;
  
  info = get_filedir_parms_finish (afp_backend, move_data->source_parms_res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    goto done;
  }

  source_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? TRUE : FALSE;
  g_object_unref (info);

  info = get_filedir_parms_finish (afp_backend, move_data->dest_parms_res, &err);
  if (!info)
  {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&err);
      dest_exists = FALSE;
    }
    else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
      g_error_free (err);
      goto done;
    }
  }
  else
  {
    dest_exists = TRUE;
    dest_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? TRUE : FALSE;
    g_object_unref (info);
  }

  if (dest_exists)
  {
    if ((job->flags & G_FILE_COPY_OVERWRITE))
    {
      /* Always fail on dirs, even with overwrite */
      if (dest_is_dir)
      {
        if (source_is_dir)
          g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                    _("Can't move directory over directory"));
        else
          g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                    _("File is directory"));
        goto done;
      }
    }
    else
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
      goto done;
    }

    delete (afp_backend, job->destination, G_VFS_JOB (job)->cancellable,
            move_delete_cb, job);
  }
  else
    move_and_rename (afp_backend, job->source, job->destination,
                     G_VFS_JOB (job)->cancellable, move_move_and_rename_cb,
                     job);

done:
  free_move_data (move_data);
  return;
}

static void
move_get_dest_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  MoveData *move_data = (MoveData *)user_data;
  
  move_data->dest_parms_res = g_object_ref (res);
  if (move_data->source_parms_res)
    do_move (move_data);
}

static void
move_get_source_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  MoveData *move_data = (MoveData *)user_data;

  move_data->source_parms_res = g_object_ref (res);
  if (move_data->dest_parms_res)
    do_move (move_data);
}

static gboolean
try_move (GVfsBackend *backend,
          GVfsJobMove *job,
          const char *source,
          const char *destination,
          GFileCopyFlags flags,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  MoveData *move_data;
  
  move_data = g_slice_new0 (MoveData);
  move_data->job = job;
  
  get_filedir_parms (afp_backend, source, AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                     AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                     G_VFS_JOB (job)->cancellable, move_get_source_parms_cb,
                     move_data);

  get_filedir_parms (afp_backend, destination, AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                     AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                     G_VFS_JOB (job)->cancellable, move_get_dest_parms_cb,
                     move_data);  

  return TRUE;
}

static void
rename_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GVfsJobSetDisplayName *job = G_VFS_JOB_SET_DISPLAY_NAME (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  char *dirname, *newpath;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);

  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_CANT_RENAME:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                                  _("Can't rename volume"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS,
                                  _("Object with that name already exists"));
        break;
      case AFP_RESULT_OBJECT_LOCKED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("Target object is marked as not renameable (RenameInhibit)"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("Target object doesn't exist"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Volume is read-only"));
        break;
      default:
        job_failed_from_afp_result_code (G_VFS_JOB (job), res_code);
        break;
    }
    return;
  }

  dirname = g_path_get_dirname (job->filename);
  newpath = g_build_filename (dirname, job->display_name, NULL);
  g_vfs_job_set_display_name_set_new_path (job, newpath);

  g_free (dirname);
  g_free (newpath);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
set_display_name_get_filedir_parms_cb (GObject      *source_object,
                                       GAsyncResult *res,
                                       gpointer      user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobSetDisplayName *job = G_VFS_JOB_SET_DISPLAY_NAME (user_data);

  GFileInfo *info;
  GError *err = NULL;

  guint32 dir_id;
  GVfsAfpCommand *comm;
  char *basename;

  info = get_filedir_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  dir_id = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_PARENT_DIR_ID);
  g_object_unref (info);

  comm = g_vfs_afp_command_new (AFP_COMMAND_RENAME);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* Directory ID */
  g_vfs_afp_command_put_uint32 (comm, dir_id);

  /* Pathname */
  basename = g_path_get_basename (job->filename);
  put_pathname (comm, basename);
  g_free (basename);

  /* NewName */
  put_pathname (comm, job->display_name);

  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     rename_cb, G_VFS_JOB (job)->cancellable, job);
  g_object_unref (comm);
}
  
static gboolean
try_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  
  if (is_root (filename))
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                              _("Can't rename volume"));
    return TRUE;
  }

  get_filedir_parms (afp_backend, filename, AFP_FILEDIR_BITMAP_PARENT_DIR_ID_BIT,
                     AFP_FILEDIR_BITMAP_PARENT_DIR_ID_BIT,
                     G_VFS_JOB (job)->cancellable,
                     set_display_name_get_filedir_parms_cb, job);
  return TRUE;
}

static void
make_directory_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GVfsJobMakeDirectory *job = G_VFS_JOB_MAKE_DIRECTORY (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);

  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                  _("Not enough space on volume"));
        break;
      case AFP_RESULT_FLAT_VOL:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                  _("Volume is flat and doesn't support directories"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("Ancestor directory doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS,
                                  _("Target directory already exists"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Volume is read-only"));
        break;
      default:
        job_failed_from_afp_result_code (G_VFS_JOB (job), res_code);
        break;
    }
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
make_directory_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobMakeDirectory *job = G_VFS_JOB_MAKE_DIRECTORY (user_data);

  GFileInfo *info;
  GError *err = NULL;

  guint32 dir_id;
  char *basename;
  GVfsAfpCommand *comm;
  
  info = get_filedir_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  dir_id = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_NODE_ID);
  g_object_unref (info);

  comm = g_vfs_afp_command_new (AFP_COMMAND_CREATE_DIR);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* Directory ID */
  g_vfs_afp_command_put_uint32 (comm, dir_id);

  /* Pathname */
  basename = g_path_get_basename (job->filename);
  put_pathname (comm, basename);
  g_free (basename);

  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     make_directory_cb,
                                     G_VFS_JOB (job)->cancellable, job);
  g_object_unref (comm);
}

static gboolean 
try_make_directory (GVfsBackend *backend,
                    GVfsJobMakeDirectory *job,
                    const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  char *dirname;
  
  dirname = g_path_get_dirname (filename);
  get_filedir_parms (afp_backend, dirname, 0, AFP_DIR_BITMAP_NODE_ID_BIT,
                     G_VFS_JOB (job)->cancellable, make_directory_get_filedir_parms_cb,
                     job);
  g_free (dirname);
  
  return TRUE;
}

static void
delete_delete_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobDelete *job = G_VFS_JOB_DELETE (user_data);

  GError *err = NULL;

  if (!delete_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean 
try_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  delete (afp_backend, filename, G_VFS_JOB (job)->cancellable,
          delete_delete_cb, job);
  
  return TRUE;
}

static void
write_ext_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GVfsJobWrite *job = G_VFS_JOB_WRITE (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  gint64 last_written;
  gsize written_size;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (!(res_code == AFP_RESULT_NO_ERROR || res_code == AFP_RESULT_LOCK_ERR))
  {
    g_object_unref (reply);

    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("File is not open for write access"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                  _("Not enough space on volume"));
        break;
      default:
        job_failed_from_afp_result_code (G_VFS_JOB (job), res_code);
        break;
    }
    return;
  }

  g_vfs_afp_reply_read_int64 (reply, &last_written);
  g_object_unref (reply);

  written_size = last_written - afp_handle->offset;
  afp_handle->offset = last_written;

  if (afp_handle->type == AFP_HANDLE_TYPE_REPLACE_FILE_DIRECT)
    afp_handle->size = MAX (last_written, afp_handle->size);
  
  g_vfs_job_write_set_written_size (job, written_size); 
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_write (GVfsBackend *backend,
           GVfsJobWrite *job,
           GVfsBackendHandle handle,
           char *buffer,
           gsize buffer_size)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  GVfsAfpCommand *comm;
  guint32 req_count;

  comm = g_vfs_afp_command_new (AFP_COMMAND_WRITE_EXT);
  /* StartEndFlag = 0 */
  g_vfs_afp_command_put_byte (comm, 0);

  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, afp_handle->fork_refnum);
  /* Offset */
  g_vfs_afp_command_put_int64 (comm, afp_handle->offset);
  /* ReqCount */
  req_count = MIN (buffer_size, G_MAXUINT32);
  g_vfs_afp_command_put_int64 (comm, req_count);

  g_vfs_afp_command_set_buffer (comm, buffer, buffer_size);

  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                     write_ext_cb, G_VFS_JOB (job)->cancellable,
                                     job);
  g_object_unref (comm);

  return TRUE;
}

static void
seek_on_write_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobSeekWrite *job = G_VFS_JOB_SEEK_WRITE (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GError *err = NULL;
  GFileInfo *info;
  gsize size;

  info = get_fork_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }
  
  size = g_file_info_get_size (info);
  g_object_unref (info);

  switch (job->seek_type)
  {
    case G_SEEK_CUR:
      afp_handle->offset += job->requested_offset;
      break;
    case G_SEEK_SET:
      afp_handle->offset = job->requested_offset;
      break;
    case G_SEEK_END:
      afp_handle->offset = size + job->requested_offset;
      break;
  }

  if (afp_handle->offset < 0)
    afp_handle->offset = 0;
  else if (afp_handle->offset > size)
    afp_handle->offset = size;

  g_vfs_job_seek_write_set_offset (job, afp_handle->offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_seek_on_write (GVfsBackend *backend,
                   GVfsJobSeekWrite *job,
                   GVfsBackendHandle handle,
                   goffset    offset,
                   GSeekType  type)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  if (afp_handle->type == AFP_HANDLE_TYPE_REPLACE_FILE_DIRECT)
  {
    switch (job->seek_type)
    {
      case G_SEEK_CUR:
        afp_handle->offset += job->requested_offset;
        break;
      case G_SEEK_SET:
        afp_handle->offset = job->requested_offset;
        break;
      case G_SEEK_END:
        afp_handle->offset = afp_handle->size + job->requested_offset;
        break;
    }

    if (afp_handle->offset < 0)
      afp_handle->offset = 0;
    else if (afp_handle->offset > afp_handle->size)
      afp_handle->offset = afp_handle->size;

    g_vfs_job_seek_write_set_offset (job, afp_handle->offset);
    g_vfs_job_succeeded (G_VFS_JOB (job));
  }
  
  else
  {
    get_fork_parms (afp_backend, afp_handle->fork_refnum,
                    AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT,
                    G_VFS_JOB (job)->cancellable, seek_on_write_cb, job);
  }
    
  return TRUE;
}

static void
seek_on_read_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobSeekRead *job = G_VFS_JOB_SEEK_READ (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GError *err = NULL;
  GFileInfo *info;
  gsize size;

  info = get_fork_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }
  
  size = g_file_info_get_size (info);
  g_object_unref (info);

  switch (job->seek_type)
  {
    case G_SEEK_CUR:
      afp_handle->offset += job->requested_offset;
      break;
    case G_SEEK_SET:
      afp_handle->offset = job->requested_offset;
      break;
    case G_SEEK_END:
      afp_handle->offset = size + job->requested_offset;
      break;
  }

  if (afp_handle->offset < 0)
    afp_handle->offset = 0;
  else if (afp_handle->offset > size)
    afp_handle->offset = size;

  g_vfs_job_seek_read_set_offset (job, afp_handle->offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle handle,
                  goffset    offset,
                  GSeekType  type)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  get_fork_parms (afp_backend, afp_handle->fork_refnum,
                  AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT,
                  G_VFS_JOB (job)->cancellable, seek_on_read_cb, job);
  
  return TRUE;
}

static void
read_ext_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GVfsJobRead *job = G_VFS_JOB_READ (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  gsize size;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (!(res_code == AFP_RESULT_NO_ERROR || res_code == AFP_RESULT_EOF_ERR
        || res_code == AFP_RESULT_LOCK_ERR))
  {
    g_object_unref (reply);

    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("File is not open for read access"));
        break;
      default:
        job_failed_from_afp_result_code (G_VFS_JOB (job), res_code);
        break;
    }
    
    return;
  }

  size = g_vfs_afp_reply_get_size (reply);

  afp_handle->offset += size;
  g_vfs_job_read_set_size (job, size);

  g_object_unref (reply);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}
  
static gboolean 
try_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize bytes_requested)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;
  
  GVfsAfpCommand *comm;
  guint32 req_count;

  comm = g_vfs_afp_command_new (AFP_COMMAND_READ_EXT);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, afp_handle->fork_refnum);
  /* Offset */
  g_vfs_afp_command_put_int64 (comm, afp_handle->offset);
  /* ReqCount */
  req_count = MIN (bytes_requested, G_MAXUINT32);
  g_vfs_afp_command_put_int64 (comm, req_count);

  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, buffer,
                                      read_ext_cb, G_VFS_JOB (job)->cancellable,
                                      job);
  g_object_unref (comm);

  return TRUE;
}

static void
close_replace_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobCloseWrite *job = G_VFS_JOB_CLOSE_WRITE (user_data);

  GFileInfo *info;
  GError *err = NULL;

  info = get_filedir_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_close_write_set_etag (job, g_file_info_get_etag (info));
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  g_object_unref (info);
}
  
static void
close_replace_close_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  AfpHandle *afp_handle = (AfpHandle *)user_data;

  /* Delete temporary file */
  delete (afp_backend, afp_handle->tmp_filename, NULL, NULL, NULL);
  
  afp_handle_free (afp_handle);
}

static void
close_replace_exchange_files_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GVfsJobCloseWrite *job = G_VFS_JOB_CLOSE_WRITE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    afp_handle_free (afp_handle);
    return;
  }

  /* Close fork and remove the temporary file even if the exchange failed */
  close_fork (afp_backend, afp_handle->fork_refnum, G_VFS_JOB (job)->cancellable,
              close_replace_close_fork_cb, job->handle);
  
  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_ID_NOT_FOUND:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("File doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_TYPE_ERR:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                  _("File is directory"));
        break;   
      default:
        job_failed_from_afp_result_code (G_VFS_JOB (job), res_code);
        break;
    }
    return;
  }

  /* Get ETAG */
  get_filedir_parms (afp_backend, afp_handle->filename,
                     AFP_FILE_BITMAP_MOD_DATE_BIT, 0,
                     G_VFS_JOB (job)->cancellable,
                     close_replace_get_filedir_parms_cb, job);
}

static void
close_write_close_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobCloseWrite *job = G_VFS_JOB_CLOSE_WRITE (user_data);

  GError *err = NULL;

  if (!close_fork_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
close_write_get_fork_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobCloseWrite *job = G_VFS_JOB_CLOSE_WRITE (user_data);

  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GError *err = NULL;
  GFileInfo *info;

  info = get_fork_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);

    afp_handle_free (afp_handle);
    return;
  }

  g_vfs_job_close_write_set_etag (job, g_file_info_get_etag (info));

  close_fork (afp_backend, afp_handle->fork_refnum, G_VFS_JOB (job)->cancellable,
              close_write_close_fork_cb, job);
  afp_handle_free (afp_handle);
}
  
static void
close_replace_set_fork_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GVfsJobCloseWrite *job = G_VFS_JOB_CLOSE_WRITE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    afp_handle_free (afp_handle);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                  _("Not enough space on volume"));
        break;
      case AFP_RESULT_LOCK_ERR:
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("Range lock conflict exists"));
        break;
      default:
        job_failed_from_afp_result_code (G_VFS_JOB (job), res_code);
        break;
    }
    afp_handle_free (afp_handle);
    return;
  }

  /* Get ETAG */
  get_fork_parms (afp_backend, afp_handle->fork_refnum, AFP_FILE_BITMAP_MOD_DATE_BIT,
                  G_VFS_JOB (job)->cancellable, close_write_get_fork_parms_cb,
                  job);
}

static gboolean
try_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle handle)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;
  
  if (afp_handle->type == AFP_HANDLE_TYPE_REPLACE_FILE_TEMP)
  {
    GVfsAfpCommand *comm;

    comm = g_vfs_afp_command_new (AFP_COMMAND_EXCHANGE_FILES);
    /* pad byte */
    g_vfs_afp_command_put_byte (comm, 0);

    /* Volume ID */
    g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
    /* SourceDirectory ID 2 == / */
    g_vfs_afp_command_put_uint32 (comm, 2);
    /* DestDirectory ID 2 == / */
    g_vfs_afp_command_put_uint32 (comm, 2);

    /* SourcePath */
    put_pathname (comm, afp_handle->filename);
    /* DestPath */
    put_pathname (comm, afp_handle->tmp_filename);

    g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                       close_replace_exchange_files_cb,
                                       G_VFS_JOB (job)->cancellable, job);
    g_object_unref (comm);
  }
  else if (afp_handle->type == AFP_HANDLE_TYPE_REPLACE_FILE_DIRECT)
  {
    GVfsAfpCommand *comm;

    comm = g_vfs_afp_command_new (AFP_COMMAND_SET_FORK_PARMS);
    /* pad byte */
    g_vfs_afp_command_put_byte (comm, 0);

    /* OForkRefNum */
    g_vfs_afp_command_put_int16 (comm, afp_handle->fork_refnum);
    /* Bitmap */
    g_vfs_afp_command_put_uint16 (comm, AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT);
    /* ForkLen */
    g_vfs_afp_command_put_int64 (comm, afp_handle->size);

    g_vfs_afp_connection_send_command (afp_backend->server->conn, comm, NULL,
                                       close_replace_set_fork_parms_cb,
                                       G_VFS_JOB (job)->cancellable, job);
    g_object_unref (comm);
  }
  else
  {
    /* Get ETAG */
    get_fork_parms (afp_backend, afp_handle->fork_refnum, AFP_FILE_BITMAP_MOD_DATE_BIT,
                    G_VFS_JOB (job)->cancellable, close_write_get_fork_parms_cb,
                    job);
  }
  
  return TRUE;
}

static void
close_read_close_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobCloseRead *job = G_VFS_JOB_CLOSE_READ (user_data);

  GError *err = NULL;
  
  if (!close_fork_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle handle)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  close_fork (afp_backend, afp_handle->fork_refnum, G_VFS_JOB (job)->cancellable,
              close_read_close_fork_cb, job);
  afp_handle_free ((AfpHandle *)job->handle);
  
  return TRUE;
}

static void
create_open_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);

  gint16 fork_refnum;
  GError *err = NULL;
  AfpHandle *afp_handle;
  
  if (!open_fork_finish (afp_backend, res, &fork_refnum, NULL, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  afp_handle = afp_handle_new (fork_refnum);
  afp_handle->type = AFP_HANDLE_TYPE_CREATE_FILE;
  
  g_vfs_job_open_for_write_set_handle (job, (GVfsBackendHandle) afp_handle);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_initial_offset (job, 0);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
create_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);

  GError *err = NULL;

  if (!create_file_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  open_fork (afp_backend, job->filename, AFP_ACCESS_MODE_WRITE_BIT, 0,
             G_VFS_JOB (job)->cancellable, create_open_fork_cb, job);
}

static gboolean
try_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  create_file (afp_backend, filename, FALSE, G_VFS_JOB (job)->cancellable,
               create_cb, job);

  return TRUE;
}

static void
replace_open_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);

  gint16 fork_refnum;
  GError *err = NULL;
  AfpHandle *afp_handle;
  char *tmp_filename;
  
  if (!open_fork_finish (afp_backend, res, &fork_refnum, NULL, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  afp_handle = afp_handle_new (fork_refnum);
  tmp_filename = g_object_get_data (G_OBJECT (job), "TempFilename");
  /* Replace using temporary file */
  if (tmp_filename)
  {
    afp_handle->type = AFP_HANDLE_TYPE_REPLACE_FILE_TEMP;
    afp_handle->filename = g_strdup (job->filename);
    afp_handle->tmp_filename = g_strdup (tmp_filename);
  }
  else
    afp_handle->type = AFP_HANDLE_TYPE_REPLACE_FILE_DIRECT;
    
  g_vfs_job_open_for_write_set_handle (job, (GVfsBackendHandle) afp_handle);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_initial_offset (job, 0);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void replace_create_tmp_file (GVfsBackendAfp *afp_backend, GVfsJobOpenForWrite *job);

static void
replace_create_tmp_file_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);

  GError *err = NULL;
  char *tmp_filename;

  if (!create_file_finish (afp_backend, res, &err))
  {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_EXISTS))
      replace_create_tmp_file (afp_backend, job);

    /* We don't have the necessary permissions to create a temporary file
     * so we try to write directly to the file */
    else if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
    {
      g_object_set_data (G_OBJECT (job), "TempFilename", NULL);
      open_fork (afp_backend, job->filename, AFP_ACCESS_MODE_WRITE_BIT, 0,
                 G_VFS_JOB (job)->cancellable, replace_open_fork_cb, job);
    }
                              
    else
    {
      g_vfs_job_failed (G_VFS_JOB (job), err->domain, err->code,
                        _("Unable to create temporary file (%s)"), err->message);
    }
    g_error_free (err);
    return;
  }

  tmp_filename = g_object_get_data (G_OBJECT (job), "TempFilename");
  open_fork (afp_backend, tmp_filename, AFP_ACCESS_MODE_WRITE_BIT, 0,
             G_VFS_JOB (job)->cancellable, replace_open_fork_cb, job);
}

static void
random_chars (char *str, int len)
{
  int i;
  const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  for (i = 0; i < len; i++)
    str[i] = chars[g_random_int_range (0, strlen(chars))];
}

static void
replace_create_tmp_file (GVfsBackendAfp *afp_backend, GVfsJobOpenForWrite *job)
{
  char basename[] = "~gvfXXXX.tmp";
  char *dir, *tmp_filename;

  random_chars (basename + 4, 4);
  dir = g_path_get_dirname (job->filename);

  tmp_filename = g_build_filename (dir, basename, NULL);
  g_free (dir);

  g_object_set_data_full (G_OBJECT (job), "TempFilename", tmp_filename, g_free);
  create_file (afp_backend, tmp_filename, FALSE, G_VFS_JOB (job)->cancellable,
               replace_create_tmp_file_cb, job);
}

static void
replace_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);

  GError *err = NULL;
  GFileInfo *info;

  info = get_filedir_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    /* Create file if it doesn't exist */
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      try_create (G_VFS_BACKEND (afp_backend), job, job->filename, job->flags);

    else
      g_vfs_job_failed_from_error (G_VFS_JOB (job), err);

    g_error_free (err);
    return;
  }

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                              _("File is directory"));
  }
  
  else if (job->etag && g_strcmp0 (g_file_info_get_etag (info), job->etag) != 0)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), 
                              G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                              _("The file was externally modified"));
  }
  else
  {
    if (afp_backend->vol_attrs_bitmap & AFP_VOLUME_ATTRIBUTES_BITMAP_NO_EXCHANGE_FILES)
    {
      open_fork (afp_backend, job->filename, AFP_ACCESS_MODE_WRITE_BIT, 0,
                 G_VFS_JOB (job)->cancellable, replace_open_fork_cb, job);
    }
    else
      replace_create_tmp_file (afp_backend, job);
  }

  g_object_unref (info);
}
  
static gboolean
try_replace (GVfsBackend *backend,
             GVfsJobOpenForWrite *job,
             const char *filename,
             const char *etag,
             gboolean make_backup,
             GFileCreateFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  if (make_backup)
  { 
    /* FIXME: implement! */
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_CANT_CREATE_BACKUP,
                              _("backups not supported yet"));
    return TRUE;
  }

  get_filedir_parms (afp_backend, filename, AFP_FILE_BITMAP_MOD_DATE_BIT, 0,
                     G_VFS_JOB (job)->cancellable, replace_get_filedir_parms_cb,
                     job);
  return TRUE;
}

static void
append_to_get_fork_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->backend_handle;

  GFileInfo *info;
  GError *err = NULL;
  goffset size;

  info = get_fork_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err); 
    g_error_free (err);
    afp_handle_free (afp_handle);
    return;
  }

  size = g_file_info_get_size (info);
  g_object_unref (info);

  afp_handle->offset = size;
  g_vfs_job_open_for_write_set_initial_offset (job, size);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
append_to_open_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);

  gint16 fork_refnum;
  GError *err = NULL;
  AfpHandle *afp_handle;

  if (!open_fork_finish (afp_backend, res, &fork_refnum, NULL, &err))
  {
    /* Create file if it doesn't exist */
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      try_create (G_VFS_BACKEND (afp_backend), job, job->filename, job->flags);
    
    else
      g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    
    g_error_free (err);
    return;
  }

  afp_handle = afp_handle_new (fork_refnum);
  afp_handle->type = AFP_HANDLE_TYPE_APPEND_TO_FILE;
  g_vfs_job_open_for_write_set_handle (job, (GVfsBackendHandle) afp_handle);
  
  get_fork_parms (afp_backend, afp_handle->fork_refnum,
                  AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT,
                  G_VFS_JOB (job)->cancellable, append_to_get_fork_parms_cb,
                  job);
}

static gboolean
try_append_to (GVfsBackend *backend,
               GVfsJobOpenForWrite *job,
               const char *filename,
               GFileCreateFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  open_fork (afp_backend, job->filename, AFP_ACCESS_MODE_WRITE_BIT, 0,
             G_VFS_JOB (job)->cancellable, append_to_open_fork_cb, job);
  return TRUE;
}

static void
read_open_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobOpenForRead *job = G_VFS_JOB_OPEN_FOR_READ (user_data);

  GError *err = NULL;
  gint16 fork_refnum;
  AfpHandle *afp_handle;
  
  if (!open_fork_finish (afp_backend, res, &fork_refnum, NULL, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  afp_handle = afp_handle_new (fork_refnum);
  afp_handle->type = AFP_HANDLE_TYPE_READ_FILE;
  
  g_vfs_job_open_for_read_set_handle (job, (GVfsBackendHandle) afp_handle);
  g_vfs_job_open_for_read_set_can_seek (job, TRUE);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  open_fork (afp_backend, filename, AFP_ACCESS_MODE_READ_BIT, 0,
             G_VFS_JOB (job)->cancellable, read_open_fork_cb, job);
  return TRUE;
}

static guint16
create_filedir_bitmap (GVfsBackendAfp *afp_backend, GFileAttributeMatcher *matcher)
{
  guint16 bitmap;

  bitmap = AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT | AFP_FILEDIR_BITMAP_UTF8_NAME_BIT;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_CREATED))
    bitmap |= AFP_FILEDIR_BITMAP_CREATE_DATE_BIT;
  
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_MODIFIED) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ETAG_VALUE))
    bitmap |= AFP_FILEDIR_BITMAP_MOD_DATE_BIT;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_UNIX_MODE) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_UNIX_UID) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_UNIX_GID) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_USER) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_USER_REAL) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_GROUP))
      
  {
    if (afp_backend->vol_attrs_bitmap & AFP_VOLUME_ATTRIBUTES_BITMAP_SUPPORTS_UNIX_PRIVS)
      bitmap |= AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT;
  }
      
  return bitmap;
}

static guint16
create_file_bitmap (GVfsBackendAfp *afp_backend, GFileAttributeMatcher *matcher)
{
  guint16 file_bitmap;
  
  file_bitmap = create_filedir_bitmap (afp_backend, matcher);

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    file_bitmap |= AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT;
  
  return file_bitmap;
}

static guint16
create_dir_bitmap (GVfsBackendAfp *afp_backend, GFileAttributeMatcher *matcher)
{
  guint16 dir_bitmap;
  
  dir_bitmap = create_filedir_bitmap (afp_backend, matcher);

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_AFP_CHILDREN_COUNT))
    dir_bitmap |= AFP_DIR_BITMAP_OFFSPRING_COUNT_BIT;
  
  return dir_bitmap;
}

static void
enumerate (GVfsBackendAfp *afp_backend,
           GVfsJobEnumerate *job,
           gint32 start_index);

static void
enumerate_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  
  guint16 file_bitmap;
  guint16  dir_bitmap;
  gint16 count, i;

  gint64 start_index, max;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                          _("Permission denied"));
        break;
      case AFP_RESULT_DIR_NOT_FOUND:
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                          _("Directory doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_TYPE_ERR:
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                          _("Target object is not a directory"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_vfs_job_succeeded (G_VFS_JOB (job));
        g_vfs_job_enumerate_done (job);
        break;
      default:
        job_failed_from_afp_result_code (G_VFS_JOB (job), res_code);
        break;
    }
    return;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_uint16 (reply, &dir_bitmap);

  g_vfs_afp_reply_read_int16 (reply, &count);
  for (i = 0; i < count; i++)
  {
    goffset start_pos;
    guint16 struct_length;
    guint8 FileDir;
    
    gboolean directory;
    guint16 bitmap;
    GFileInfo *info;

    start_pos = g_vfs_afp_reply_get_pos (reply);
    
    g_vfs_afp_reply_read_uint16 (reply, &struct_length);
    g_vfs_afp_reply_read_byte (reply, &FileDir);
    /* pad byte */
    g_vfs_afp_reply_read_byte (reply, NULL);

    directory = (FileDir & 0x80); 
    bitmap =  directory ? dir_bitmap : file_bitmap;
    
    info = g_file_info_new ();
    fill_info (afp_backend, info, reply, directory, bitmap);
    g_vfs_job_enumerate_add_info (job, info);
    g_object_unref (info);

    g_vfs_afp_reply_seek (reply, start_pos + struct_length, G_SEEK_SET);
  }
  g_object_unref (reply);
  
  start_index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (job),
                                                    "start-index"));
  start_index += count;

  max = (afp_backend->server->version >= AFP_VERSION_3_1) ? G_MAXINT32 : G_MAXINT16;
  /* Can't enumerate any more files */
  if (start_index > max)
  {
    g_vfs_job_succeeded (G_VFS_JOB (job));
    g_vfs_job_enumerate_done (job);
  }

  enumerate (afp_backend, job, start_index);
}

static void
enumerate (GVfsBackendAfp *afp_backend,
           GVfsJobEnumerate *job,
           gint32 start_index)
{
  GVfsAfpConnection *conn = afp_backend->server->conn;
  const char *filename = job->filename;
  GFileAttributeMatcher *matcher = job->attribute_matcher;
  
  GVfsAfpCommand *comm;
  guint16 file_bitmap, dir_bitmap;

  if (afp_backend->server->version >= AFP_VERSION_3_1)
    comm = g_vfs_afp_command_new (AFP_COMMAND_ENUMERATE_EXT2);
  else
    comm = g_vfs_afp_command_new (AFP_COMMAND_ENUMERATE_EXT);
  
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, afp_backend->volume_id);
  /* Directory ID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* File Bitmap */
  file_bitmap = create_file_bitmap (afp_backend, matcher);
  g_vfs_afp_command_put_uint16 (comm, file_bitmap);
  
  /* Dir Bitmap */
  dir_bitmap = create_dir_bitmap (afp_backend, matcher);
  g_vfs_afp_command_put_uint16 (comm, dir_bitmap);

  /* Req Count */
  g_vfs_afp_command_put_int16 (comm, ENUMERATE_REQ_COUNT);

  
  /* StartIndex and MaxReplySize */
  if (afp_backend->server->version >= AFP_VERSION_3_1)
  {
    g_vfs_afp_command_put_int32 (comm, start_index);
    g_vfs_afp_command_put_int32 (comm, ENUMERATE_EXT2_MAX_REPLY_SIZE);
  }
  else
  {
    g_vfs_afp_command_put_int16 (comm, start_index);
    g_vfs_afp_command_put_int16 (comm, ENUMERATE_EXT_MAX_REPLY_SIZE);
  }
  
  /* Pathname */
  put_pathname (comm, filename);

  g_object_set_data (G_OBJECT (job), "start-index",
                     GINT_TO_POINTER (start_index));
  
  g_vfs_afp_connection_send_command (conn, comm, NULL, enumerate_cb,
                                     G_VFS_JOB (job)->cancellable, job);
  g_object_unref (comm);
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  enumerate (afp_backend, job, 1);
  
  return TRUE;
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
                               GVfsJobQueryAttributes *job,
                               const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  GFileAttributeInfoList *list;

  list = g_file_attribute_info_list_new ();

  if (afp_backend->vol_attrs_bitmap & AFP_VOLUME_ATTRIBUTES_BITMAP_SUPPORTS_UNIX_PRIVS)
  {
    g_file_attribute_info_list_add (list,
                                    G_FILE_ATTRIBUTE_UNIX_MODE,
                                    G_FILE_ATTRIBUTE_TYPE_UINT32,
                                    G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                    G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
    g_file_attribute_info_list_add (list,
                                    G_FILE_ATTRIBUTE_UNIX_UID,
                                    G_FILE_ATTRIBUTE_TYPE_UINT32,
                                    G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                    G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
    g_file_attribute_info_list_add (list,
                                    G_FILE_ATTRIBUTE_UNIX_GID,
                                    G_FILE_ATTRIBUTE_TYPE_UINT32,
                                    G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                    G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  }
    
  g_vfs_job_query_attributes_set_list (job, list);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_file_attribute_info_list_unref (list);
  
  return TRUE;
}

static void
set_attribute_set_unix_privs_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobSetAttribute *job = G_VFS_JOB_SET_ATTRIBUTE (user_data);

  GError *err = NULL;

  if (!set_unix_privs_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
set_attribute_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobSetAttribute *job = G_VFS_JOB_SET_ATTRIBUTE (user_data);

  GFileInfo *info;
  GError *err = NULL;

  guint32 uid, gid, permissions, ua_permissions;

  info = get_filedir_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);
  gid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID);
  permissions = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);
  ua_permissions = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_UA_PERMISSIONS);

  g_object_unref (info);

  
  if (strcmp (job->attribute, G_FILE_ATTRIBUTE_UNIX_UID) == 0)
    uid = job->value.uint32;
  else if (strcmp (job->attribute, G_FILE_ATTRIBUTE_UNIX_GID) == 0)
    gid = job->value.uint32;
  else if (strcmp (job->attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0)
    permissions = job->value.uint32;

  set_unix_privs (afp_backend, job->filename,
                  uid, gid, permissions, ua_permissions,
                  G_VFS_JOB (job)->cancellable,
                  set_attribute_set_unix_privs_cb, job);
}

static gboolean
try_set_attribute (GVfsBackend *backend,
                   GVfsJobSetAttribute *job,
                   const char *filename,
                   const char *attribute,
                   GFileAttributeType type,
                   gpointer value_p,
                   GFileQueryInfoFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  if ((strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0 ||
       strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_UID) == 0 ||
       strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_GID) == 0)
      && afp_backend->vol_attrs_bitmap & AFP_VOLUME_ATTRIBUTES_BITMAP_SUPPORTS_UNIX_PRIVS)
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT32) 
      {
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,
                          G_IO_ERROR_INVALID_ARGUMENT,
                          "%s",
                          _("Invalid attribute type (uint32 expected)"));
        return TRUE;
      }

      get_filedir_parms (afp_backend, filename, AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT,
                         AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT,
                         G_VFS_JOB (job)->cancellable, set_attribute_get_filedir_parms_cb,
                         job);
      return TRUE;
    }

  else {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      _("Operation unsupported"));
    return TRUE;
  }
}


static void
query_fs_info_get_vol_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpServer *server = G_VFS_AFP_SERVER (source_object);
  GVfsJobQueryFsInfo *job = G_VFS_JOB_QUERY_FS_INFO (user_data);

  GError *err = NULL;
  GFileInfo *info;

  info = g_vfs_afp_server_get_vol_parms_finish (server, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  copy_file_info_into (info, job->file_info);
  g_object_unref (info);


  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  
  guint16 vol_bitmap = 0;

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "afp");

  
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
    vol_bitmap |= AFP_VOLUME_BITMAP_EXT_BYTES_TOTAL_BIT;
  
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    vol_bitmap |= AFP_VOLUME_BITMAP_EXT_BYTES_FREE_BIT;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY))
    vol_bitmap |= AFP_VOLUME_BITMAP_ATTRIBUTE_BIT;

  if (vol_bitmap != 0)
  {
    g_vfs_afp_server_get_vol_parms (afp_backend->server, afp_backend->volume_id,
                                    vol_bitmap, G_VFS_JOB (job)->cancellable,
                                    query_fs_info_get_vol_parms_cb, job);
  }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
get_name_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);

  char *name;
  AfpMapIDFunction map_function;
  guint outstanding_requests;

  name = map_id_finish (afp_backend, res, &map_function, NULL);
  if (name)
  {
    switch (map_function)
    {
      case AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME:
        g_file_info_set_attribute_string (job->file_info, G_FILE_ATTRIBUTE_OWNER_USER,
                                          name);
        break;
      case AFP_MAP_ID_FUNCTION_USER_ID_TO_UTF8_NAME:
        g_file_info_set_attribute_string (job->file_info, G_FILE_ATTRIBUTE_OWNER_USER_REAL,
                                          name);
        break;
      case AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME:
        g_file_info_set_attribute_string (job->file_info, G_FILE_ATTRIBUTE_OWNER_GROUP,
                                          name);
        break;

      default:
        g_assert_not_reached ();
    }

    g_free (name);
  }

  outstanding_requests = GPOINTER_TO_UINT (G_VFS_JOB (job)->backend_data);
  if (--outstanding_requests == 0)
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    G_VFS_JOB (job)->backend_data = GUINT_TO_POINTER (outstanding_requests);
}

static void
set_root_info (GVfsBackendAfp *afp_backend, GFileInfo *info)
{
  GIcon *icon;
  
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_name (info, "/");
  g_file_info_set_display_name (info,
                                g_vfs_backend_get_display_name (G_VFS_BACKEND (afp_backend)));
  g_file_info_set_content_type (info, "inode/directory");
  icon = g_vfs_backend_get_icon (G_VFS_BACKEND (afp_backend));
  if (icon != NULL)
    g_file_info_set_icon (info, icon);
}

static void
query_info_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (source_object);
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);
  

  GFileInfo *info;
  GError *err = NULL;

  GFileAttributeMatcher *matcher;
  guint outstanding_requests;
  
  info = get_filedir_parms_finish (afp_backend, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  outstanding_requests = 0;
  matcher = job->attribute_matcher;
  
  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_UID))  
  {
    guint32 uid;

    uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);

    if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_USER))
    {
      map_id (afp_backend, AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME, uid,
              G_VFS_JOB (job)->cancellable, get_name_cb, job);
      outstanding_requests++;
    }
    
    if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_USER_REAL))
    {
      map_id (afp_backend, AFP_MAP_ID_FUNCTION_USER_ID_TO_UTF8_NAME, uid,
              G_VFS_JOB (job)->cancellable, get_name_cb, job);
      outstanding_requests++;
    }
  }

  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_GID) &&
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_GROUP))  
  {
    guint32 gid;

    gid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID);

    map_id (afp_backend, AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME, gid,
            G_VFS_JOB (job)->cancellable, get_name_cb, job);
    outstanding_requests++;
  }
  
  G_VFS_JOB (job)->backend_data = GUINT_TO_POINTER (outstanding_requests);
  
  copy_file_info_into (info, job->file_info);
  g_object_unref (info);

  if (is_root (job->filename))
    set_root_info (afp_backend, job->file_info);
  
  if (outstanding_requests == 0)
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  g_debug ("Filename: %s\n", filename);
  
  if (is_root (filename))
  {
    guint16 dir_bitmap = 0;

    dir_bitmap = create_dir_bitmap (afp_backend, matcher);
    dir_bitmap &= ~AFP_DIR_BITMAP_UTF8_NAME_BIT;
    
    if (dir_bitmap != 0)
    {
      get_filedir_parms (afp_backend, filename, 0, dir_bitmap,
                         G_VFS_JOB (job)->cancellable,
                         query_info_get_filedir_parms_cb, job);
    }
    else
    {
      set_root_info (afp_backend, info);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  }
  
  else {
    guint16 file_bitmap, dir_bitmap;
    
    file_bitmap = create_file_bitmap (afp_backend, matcher);
    dir_bitmap = create_dir_bitmap (afp_backend, matcher);

    get_filedir_parms (afp_backend, filename, file_bitmap, dir_bitmap,
                       G_VFS_JOB (job)->cancellable, query_info_get_filedir_parms_cb,
                       job);
  }

  return TRUE;
}

static gboolean
get_userinfo (GVfsBackendAfp *afp_backend,
              GCancellable *cancellable,
              GError **error)
{
  GVfsAfpCommand *comm;
  guint16 bitmap;
  gboolean res;

  GVfsAfpReply *reply;
  AfpResultCode res_code;

  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_USER_INFO);
  /* Flags, ThisUser = 1 */
  g_vfs_afp_command_put_byte (comm, 0x01);
  /* UserId */
  g_vfs_afp_command_put_int32 (comm, 0);
  /* Bitmap */
  bitmap = AFP_GET_USER_INFO_BITMAP_GET_UID_BIT | AFP_GET_USER_INFO_BITMAP_GET_GID_BIT;
  g_vfs_afp_command_put_uint16 (comm, bitmap);

  res = g_vfs_afp_connection_send_command_sync (afp_backend->server->conn,
                                                comm, cancellable, error);
  g_object_unref (comm);
  if (!res)
    return FALSE;

  reply = g_vfs_afp_connection_read_reply_sync (afp_backend->server->conn,
                                                cancellable, error);
  if (!reply)
    return FALSE;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("Permission denied"));
        break;
        break;
      case AFP_RESULT_CALL_NOT_SUPPORTED:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     _("Command is not supported by server"));
        break;
      case AFP_RESULT_PWD_EXPIRED_ERR:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("User's password has expired"));
        break;
      case AFP_RESULT_PWD_NEEDS_CHANGE_ERR:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("User's password needs to be changed"));
        break;

      default:
        g_propagate_error (error, afp_result_code_to_gerror (res_code));
        break;
    }
    return FALSE;
  }

  /* Bitmap */
  g_vfs_afp_reply_read_uint16 (reply, NULL);
  /* UID */
  g_vfs_afp_reply_read_uint32 (reply, &afp_backend->user_id);
  /* GID */
  g_vfs_afp_reply_read_uint32 (reply, &afp_backend->group_id);

  g_object_unref (reply);
  
  return TRUE;
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  gboolean res;
  GError *err = NULL;

  GVfsAfpCommand *comm;
  GVfsAfpReply *reply;
  AfpResultCode res_code;
  
  GMountSpec *afp_mount_spec;
  char       *server_name;
  char       *display_name;

  afp_backend->server = g_vfs_afp_server_new (afp_backend->addr);

  res = g_vfs_afp_server_login (afp_backend->server, afp_backend->user, mount_source,
                                NULL, G_VFS_JOB (job)->cancellable, &err);
  if (!res)
    goto error;
  
  /* Get User Info */
  if (!get_userinfo (afp_backend, G_VFS_JOB (job)->cancellable, &err))
    goto error;
  
  /* Open Volume */
  comm = g_vfs_afp_command_new (AFP_COMMAND_OPEN_VOL);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume Bitmap */
  g_vfs_afp_command_put_uint16 (comm, AFP_VOLUME_BITMAP_VOL_ID_BIT | AFP_VOLUME_BITMAP_ATTRIBUTE_BIT);

  /* VolumeName */
  g_vfs_afp_command_put_pascal (comm, afp_backend->volume);

  /* TODO: password? */

  res = g_vfs_afp_connection_send_command_sync (afp_backend->server->conn,
                                                comm, G_VFS_JOB (job)->cancellable,
                                                &err);
  g_object_unref (comm);
  if (!res)
    goto error;

  reply = g_vfs_afp_connection_read_reply_sync (afp_backend->server->conn,
                                                G_VFS_JOB (job)->cancellable, &err);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    goto generic_error;
  }
  
  /* Volume Bitmap */
  g_vfs_afp_reply_read_uint16 (reply, NULL);
  /* Volume Attributes Bitmap */
  g_vfs_afp_reply_read_uint16 (reply, &afp_backend->vol_attrs_bitmap);
  /* Volume ID */
  g_vfs_afp_reply_read_uint16 (reply, &afp_backend->volume_id);
  
  g_object_unref (reply);
  
  /* set mount info */
  afp_mount_spec = g_mount_spec_new ("afp-volume");
  g_mount_spec_set (afp_mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  g_mount_spec_set (afp_mount_spec, "volume", afp_backend->volume);
  if (afp_backend->user)
    g_mount_spec_set (afp_mount_spec, "user", afp_backend->user);

  g_vfs_backend_set_mount_spec (backend, afp_mount_spec);
  g_mount_spec_unref (afp_mount_spec);

  if (afp_backend->server->utf8_server_name)
    server_name = afp_backend->server->utf8_server_name;
  else
    server_name = afp_backend->server->server_name;
  
  if (afp_backend->user)
    /* Translators: first %s is volumename, second username and third servername */ 
    display_name = g_strdup_printf (_("AFP volume %s for %s on %s"), 
                                    afp_backend->volume, afp_backend->user,
                                    server_name);
  else
    /* Translators: first %s is volumename and second servername */
    display_name = g_strdup_printf (_("AFP volume %s on %s"),
                                    afp_backend->volume, server_name);
  
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  g_vfs_backend_set_icon_name (backend, "folder-remote-afp");
  g_vfs_backend_set_user_visible (backend, TRUE);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
  return;

generic_error:
  /* Translators: first %s is volumename and second servername */ 
  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                    _("Couldn't mount AFP volume %s on %s"), afp_backend->volume,
                    afp_backend->server->server_name);
  return;
}
  
static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  const char *host, *volume, *portstr, *user;
  guint16 port = 548;

  host = g_mount_spec_get (mount_spec, "host");
  if (host == NULL)
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      _("No hostname specified"));
    return TRUE;
  }

  volume = g_mount_spec_get (mount_spec, "volume");
  if (volume == NULL)
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      _("No volume specified"));
    return TRUE;
  }
  afp_backend->volume = g_strdup (volume);

  portstr = g_mount_spec_get (mount_spec, "port");
  if (portstr != NULL)
  {
    port = atoi (portstr);
  }

  afp_backend->addr = G_NETWORK_ADDRESS (g_network_address_new (host, port));

  user = g_mount_spec_get (mount_spec, "user");
  afp_backend->user = g_strdup (user);

  return FALSE;
}

static void
g_vfs_backend_afp_init (GVfsBackendAfp *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);
  
  afp_backend->volume = NULL;
  afp_backend->user = NULL;

  afp_backend->addr = NULL;
}

static void
g_vfs_backend_afp_finalize (GObject *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);

  g_free (afp_backend->volume);
  g_free (afp_backend->user);

  if (afp_backend->addr)
    g_object_unref (afp_backend->addr);

  G_OBJECT_CLASS (g_vfs_backend_afp_parent_class)->finalize (object);
}

static void
g_vfs_backend_afp_class_init (GVfsBackendAfpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  object_class->finalize = g_vfs_backend_afp_finalize;

  backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_query_fs_info = try_query_fs_info;
  backend_class->try_set_attribute = try_set_attribute;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_close_read = try_close_read;
  backend_class->try_read = try_read;
  backend_class->try_seek_on_read = try_seek_on_read;
  backend_class->try_append_to = try_append_to;
  backend_class->try_create = try_create;
  backend_class->try_replace = try_replace;
  backend_class->try_write = try_write;
  backend_class->try_seek_on_write = try_seek_on_write;
  backend_class->try_close_write = try_close_write;
  backend_class->try_delete = try_delete;
  backend_class->try_make_directory = try_make_directory;
  backend_class->try_set_display_name = try_set_display_name;
  backend_class->try_move = try_move;
  backend_class->try_copy = try_copy;
}

void
g_vfs_afp_daemon_init (void)
{
  g_set_application_name (_("Apple Filing Protocol Service"));

#ifdef HAVE_GCRYPT
  gcry_check_version (NULL);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
#endif
}
