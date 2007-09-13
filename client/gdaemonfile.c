#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gdaemonfile.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gdaemonfileinputstream.h>
#include <gdaemonfileoutputstream.h>
#include <gdaemonfileenumerator.h>
#include <glib/gi18n-lib.h>
#include "gdbusutils.h"
#include "gmountoperationdbus.h"
#include <gio/gsimpleasyncresult.h>

static void g_daemon_file_file_iface_init (GFileIface       *iface);

struct _GDaemonFile
{
  GObject parent_instance;

  GMountSpec *mount_spec;
  char *path;
};

static void g_daemon_file_read_async (GFile *file,
				      int io_priority,
				      GCancellable *cancellable,
				      GAsyncReadyCallback callback,
				      gpointer callback_data);

G_DEFINE_TYPE_WITH_CODE (GDaemonFile, g_daemon_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_daemon_file_file_iface_init))

static void
g_daemon_file_finalize (GObject *object)
{
  GDaemonFile *daemon_file;

  daemon_file = G_DAEMON_FILE (object);

  g_mount_spec_unref (daemon_file->mount_spec);
  g_free (daemon_file->path);
  
  if (G_OBJECT_CLASS (g_daemon_file_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_parent_class)->finalize) (object);
}

static void
g_daemon_file_class_init (GDaemonFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_finalize;
}

static void
g_daemon_file_init (GDaemonFile *daemon_file)
{
}

static char *
canonicalize_path (const char *path)
{
  char *canon, *start, *p, *q;

  if (*path != '/')
    canon = g_strconcat ("/", path, NULL);
  else
    canon = g_strdup (path);

  /* Skip initial slash */
  start = canon + 1;

  p = start;
  while (*p != 0)
    {
      if (p[0] == '.' && (p[1] == 0 || p[1] == '/'))
	{
	  memmove (p, p+1, strlen (p+1)+1);
	}
      else if (p[0] == '.' && p[1] == '.' && (p[2] == 0 || p[2] == '/'))
	{
	  q = p + 2;
	  /* Skip previous separator */
	  p = p - 2;
	  if (p < start)
	    p = start;
	  while (p > start && *p != '/')
	    p--;
	  if (*p == '/')
	    p++;
	  memmove (p, q, strlen (q)+1);
	}
      else
	{
	  /* Skip until next separator */
	  while (*p != 0 && *p != '/')
	    p++;

	  /* Keep one separator */
	  if (*p != 0)
	    p++;
	}

      /* Remove additional separators */
      q = p;
      while (*q && *q == '/')
	q++;

      if (p != q)
	memmove (p, q, strlen (q)+1);
    }

  /* Remove trailing slashes */
  if (p > start && *(p-1) == '/')
    *(p-1) = 0;
  
  return canon;
}

GFile *
g_daemon_file_new (GMountSpec *mount_spec,
		   const char *path)
{
  GDaemonFile *daemon_file;

  daemon_file = g_object_new (G_TYPE_DAEMON_FILE, NULL);
  /* TODO: These should be construct only properties */
  daemon_file->mount_spec = g_mount_spec_ref (mount_spec);
  daemon_file->path = canonicalize_path (path);
 
  return G_FILE (daemon_file);
}

static gboolean
g_daemon_file_is_native (GFile *file)
{
  return FALSE;
}

static char *
g_daemon_file_get_basename (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  char *last_slash;    

  /* This code relies on the path being canonicalized */
  
  last_slash = strrchr (daemon_file->path, '/');
  /* If no slash, or only "/" fallback to full path */
  if (last_slash == NULL ||
      last_slash[1] == '\0')
    return g_strdup (daemon_file->path);

  return g_strdup (last_slash + 1);
}

static char *
g_daemon_file_get_path (GFile *file)
{
  return NULL;
}

static char *
g_daemon_file_get_uri (GFile *file)
{
  /* TODO: implement to-uri */
  return NULL;
}

static char *
g_daemon_file_get_parse_name (GFile *file)
{
  /* TODO: implement to-iri */
  return NULL;
}

static GFile *
g_daemon_file_get_parent (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  const char *path;
  GDaemonFile *parent;
  const char *base;
  char *parent_path;
  gsize len;    

  path = daemon_file->path;
  base = strrchr (path, '/');
  if (base == NULL || base == path)
    return NULL;

  while (base > path && *base == '/')
    base--;

  len = (guint) 1 + base - path;
  
  parent_path = g_new (gchar, len + 1);
  g_memmove (parent_path, path, len);
  parent_path[len] = 0;

  parent = g_object_new (G_TYPE_DAEMON_FILE, NULL);
  parent->mount_spec = g_mount_spec_ref (daemon_file->mount_spec);
  parent->path = parent_path;
  
  return G_FILE (parent);
}

static GFile *
g_daemon_file_dup (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);

  return g_daemon_file_new (daemon_file->mount_spec,
			    daemon_file->path);
}

static guint
g_daemon_file_hash (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);

  return
    g_str_hash (daemon_file->path) ^
    g_mount_spec_hash (daemon_file->mount_spec);
}

static gboolean
g_daemon_file_equal (GFile *file1,
		     GFile *file2)
{
  GDaemonFile *daemon_file1 = G_DAEMON_FILE (file1);
  GDaemonFile *daemon_file2 = G_DAEMON_FILE (file2);

  return g_str_equal (daemon_file1->path, daemon_file2->path);
}

static GFile *
g_daemon_file_resolve_relative (GFile *file,
				const char *relative_path)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  char *path;
  GFile *child;

  if (*relative_path == '/')
    return g_daemon_file_new (daemon_file->mount_spec, relative_path);
  
  path = g_build_path ("/", daemon_file->path, relative_path, NULL);
  child = g_daemon_file_new (daemon_file->mount_spec, path);
  g_free (path);
  
  return child;
}

static DBusMessage *
do_sync_path_call (GFile *file,
		   const char *op,
		   DBusConnection **connection_out,
		   GCancellable *cancellable,
		   GError **error,
		   int first_arg_type,
		   ...)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  DBusMessage *message, *reply;
  GMountInfo *mount_info;
  const char *path;
  va_list var_args;

  mount_info = _g_daemon_vfs_get_mount_info_sync (daemon_file->mount_spec,
						  daemon_file->path,
						  error);
  if (mount_info == NULL)
    return NULL;
  
  message =
    dbus_message_new_method_call (mount_info->dbus_id,
				  mount_info->object_path,
				  G_VFS_DBUS_MOUNTPOINT_INTERFACE,
				  op);

  path = _g_mount_info_resolve_path (mount_info,
				     daemon_file->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path, 0);

  va_start (var_args, first_arg_type);
  _g_dbus_message_append_args_valist (message,
				      first_arg_type,
				      var_args);
  va_end (var_args);

  reply = _g_vfs_daemon_call_sync (message,
				   connection_out,
				   cancellable, error);
  dbus_message_unref (message);

  _g_mount_info_unref (mount_info);
  
  return reply;
}

typedef void (*AsyncPathCallCallback) (DBusMessage *reply,
				       DBusConnection *connection,
				       GError *io_error,
				       GCancellable *cancellable,
				       gpointer op_callback,
				       gpointer op_callback_data,
				       gpointer callback_data);


typedef struct {
  GFile *file;
  char *op;
  GCancellable *cancellable;
  DBusMessage *args;
  GError *io_error;
  gpointer op_callback;
  gpointer op_callback_data;
  AsyncPathCallCallback callback;
  gpointer callback_data;
} AsyncPathCall;

static void
async_path_call_free (AsyncPathCall *data)
{
  g_object_unref (data->file);
  g_free (data->op);
  if (data->cancellable)
    g_object_unref (data->cancellable);
  if (data->io_error)
    g_error_free (data->io_error);
  if (data->args)
    dbus_message_unref (data->args);
  g_free (data);
}

static gboolean
do_async_path_call_error_idle (gpointer _data)
{
  AsyncPathCall *data = _data;

  data->callback (NULL, NULL, data->io_error, data->cancellable,
		  data->op_callback, data->op_callback_data,
		  data->callback_data);

  async_path_call_free (data);
  
  return FALSE;
}

static void
async_path_call_done (DBusMessage *reply,
		      DBusConnection *connection,
		      GError *io_error,
		      gpointer _data)
{
  AsyncPathCall *data = _data;

  data->callback (reply, connection, io_error, data->cancellable,
		  data->op_callback, data->op_callback_data,
		  data->callback_data);
  
  async_path_call_free (data);
}

static void
do_async_path_call_callback (GMountInfo *mount_info,
			     gpointer _data,
			     GError *error)
{
  AsyncPathCall *data = _data;
  GDaemonFile *daemon_file = G_DAEMON_FILE (data->file);
  const char *path;
  DBusMessage *message;
  DBusMessageIter arg_source, arg_dest;
  
  if (error != NULL)
    {
      data->io_error = g_error_copy (error);
      g_idle_add (do_async_path_call_error_idle, data);
      return;
    }

  message =
    dbus_message_new_method_call (mount_info->dbus_id,
				  mount_info->object_path,
				  G_VFS_DBUS_MOUNTPOINT_INTERFACE,
				  data->op);
  
  path = _g_mount_info_resolve_path (mount_info,
				     daemon_file->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path, 0);

  /* Append more args from data->args */

  if (data->args)
    {
      dbus_message_iter_init (data->args, &arg_source);
      dbus_message_iter_init_append (message, &arg_dest);

      _g_dbus_message_iter_copy (&arg_dest, &arg_source);
    }

  _g_vfs_daemon_call_async (message,
			    async_path_call_done, data,
			    data->cancellable);
  
  dbus_message_unref (message);
}

static void
do_async_path_call (GFile *file,
		    const char *op,
		    GCancellable *cancellable,
		    gpointer op_callback,
		    gpointer op_callback_data,
		    AsyncPathCallCallback callback,
		    gpointer callback_data,
		    int first_arg_type,
		    ...)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  va_list var_args;
  GError *error;
  AsyncPathCall *data;

  data = g_new0 (AsyncPathCall, 1);

  data->file = g_object_ref (file);
  data->op = g_strdup (op);
  if (data->cancellable)
    data->cancellable = g_object_ref (cancellable);
  data->op_callback = op_callback;
  data->op_callback_data = op_callback_data;
  data->callback = callback;
  data->callback_data = callback_data;
  
  error = NULL;

  if (first_arg_type != 0)
    {
      data->args = dbus_message_new (DBUS_MESSAGE_TYPE_METHOD_CALL);
      if (data->args == NULL)
	_g_dbus_oom ();
      
      va_start (var_args, first_arg_type);
      _g_dbus_message_append_args_valist (data->args,
					  first_arg_type,
					  var_args);
      va_end (var_args);
    }
  
  
  _g_daemon_vfs_get_mount_info_async (daemon_file->mount_spec,
				      daemon_file->path,
				      do_async_path_call_callback,
				      data);
}


static GFileEnumerator *
g_daemon_file_enumerate_children (GFile      *file,
				  const char *attributes,
				  GFileGetInfoFlags flags,
				  GCancellable *cancellable,
				  GError **error)
{
  DBusMessage *reply;
  dbus_uint32_t flags_dbus;
  char *obj_path;
  GDaemonFileEnumerator *enumerator;
  DBusConnection *connection;

  enumerator = g_daemon_file_enumerator_new ();
  obj_path = g_daemon_file_enumerator_get_object_path (enumerator);
						       
  if (attributes == NULL)
    attributes = "";
  flags_dbus = flags;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_ENUMERATE,
			     &connection, cancellable, error,
			     DBUS_TYPE_STRING, &obj_path,
			     DBUS_TYPE_STRING, &attributes,
			     DBUS_TYPE_UINT32, &flags_dbus,
			     0);
  
  g_free (obj_path);

  if (reply == NULL)
    goto error;

  dbus_message_unref (reply);

  g_daemon_file_enumerator_set_sync_connection (enumerator, connection);
  
  return G_FILE_ENUMERATOR (enumerator);

 error:
  if (reply)
    dbus_message_unref (reply);
  g_object_unref (enumerator);
  return NULL;
}

static GFileInfo *
g_daemon_file_get_info (GFile                *file,
			const char           *attributes,
			GFileGetInfoFlags     flags,
			GCancellable         *cancellable,
			GError              **error)
{
  DBusMessage *reply;
  dbus_uint32_t flags_dbus;
  DBusMessageIter iter;
  GFileInfo *info;

  if (attributes == NULL)
    attributes = "";
  flags_dbus = flags;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_GET_INFO,
			     NULL, cancellable, error,
			     DBUS_TYPE_STRING, &attributes,
			     DBUS_TYPE_UINT32, &flags,
			     0);
  if (reply == NULL)
    return NULL;

  info = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRUCT))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from get_info"));
      goto out;
    }

  info = _g_dbus_get_file_info (&iter, error);

 out:
  dbus_message_unref (reply);
  return info;
}

typedef struct {
  GFile *file;
  GAsyncReadyCallback callback;
  gpointer user_data;
  gboolean can_seek;
} GetFDData;

static void
read_async_get_fd_cb (int fd,
		      gpointer callback_data)
{
  GetFDData *data = callback_data;
  GFileInputStream *stream;
  GSimpleAsyncResult *res;
  
  if (fd == -1)
    {
      res = g_simple_async_result_new_error (G_OBJECT (data->file),
					     data->callback,
					     data->user_data,
					     G_IO_ERROR, G_IO_ERROR_FAILED,
					     _("Didn't get stream file descriptor"));
    }
  else
    {
      res = g_simple_async_result_new (G_OBJECT (data->file),
				       data->callback,
				       data->user_data,
				       g_daemon_file_read_async);
      stream = g_daemon_file_input_stream_new (fd, data->can_seek);
      g_simple_async_result_set_op_res_gpointer (res, stream, g_object_unref);
    }

  g_simple_async_result_complete (res);
  
  g_free (data);
}

static void
read_async_cb (DBusMessage *reply,
	       DBusConnection *connection,
	       GError *io_error,
	       GCancellable *cancellable,
	       gpointer op_callback,
	       gpointer op_callback_data,
	       gpointer callback_data)
{
  GFile *file = callback_data;
  guint32 fd_id;
  dbus_bool_t can_seek;
  GetFDData *get_fd_data;
  GSimpleAsyncResult *res;
  
  if (io_error != NULL)
    {
      res = g_simple_async_result_new_from_error (G_OBJECT (file),
						  op_callback,
						  op_callback_data,
						  io_error);
      g_simple_async_result_complete (res);
    }
  else
    {
      if (!dbus_message_get_args (reply, NULL,
				  DBUS_TYPE_UINT32, &fd_id,
				  DBUS_TYPE_BOOLEAN, &can_seek,
				  DBUS_TYPE_INVALID))
	{
	  res = g_simple_async_result_new_error (G_OBJECT (file),
						 op_callback,
						 op_callback_data,
						 G_IO_ERROR, G_IO_ERROR_FAILED,
						 _("Invalid return value from open"));
	  g_simple_async_result_complete (res);
	  return;
	}

      get_fd_data = g_new0 (GetFDData, 1);
      get_fd_data->file = file;
      get_fd_data->callback = op_callback;
      get_fd_data->user_data = op_callback_data;
      get_fd_data->can_seek = can_seek;
      
      _g_dbus_connection_get_fd_async (connection, fd_id,
				       read_async_get_fd_cb, get_fd_data);
    }
}

static void
g_daemon_file_read_async (GFile *file,
			  int io_priority,
			  GCancellable *cancellable,
			  GAsyncReadyCallback callback,
			  gpointer callback_data)
{
  do_async_path_call (file,
		      G_VFS_DBUS_OP_OPEN_FOR_READ,
		      cancellable,
		      callback, callback_data,
		      read_async_cb, file,
		      0);
}

static GFileInputStream *
g_daemon_file_read_finish (GFile                  *file,
			   GAsyncResult           *res,
			   GError                **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  gpointer op;

  g_assert (g_simple_async_result_get_source_tag (simple) == g_daemon_file_read_async);

  op = g_simple_async_result_get_op_res_gpointer (simple);
  if (op)
    return g_object_ref (op);
  
  return NULL;
}


static GFileInputStream *
g_daemon_file_read (GFile *file,
		    GCancellable *cancellable,
		    GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_OPEN_FOR_READ,
			     &connection, cancellable, error,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_input_stream_new (fd, can_seek);
}

static GFileOutputStream *
g_daemon_file_append_to (GFile *file,
			 GCancellable *cancellable,
			 GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 mtime, initial_offset;
  dbus_bool_t make_backup;

  mode = 1;
  mtime = 0;
  make_backup = FALSE;
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_OPEN_FOR_WRITE,
			     &connection, cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_UINT64, &mtime,
			     DBUS_TYPE_BOOLEAN, &make_backup,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
}

static GFileOutputStream *
g_daemon_file_create (GFile *file,
		      GCancellable *cancellable,
		      GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 mtime, initial_offset;
  dbus_bool_t make_backup;

  mode = 0;
  mtime = 0;
  make_backup = FALSE;
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_OPEN_FOR_WRITE,
			     &connection, cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_UINT64, &mtime,
			     DBUS_TYPE_BOOLEAN, &make_backup,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
}

static GFileOutputStream *
g_daemon_file_replace (GFile *file,
		       time_t mtime,
		       gboolean make_backup,
		       GCancellable *cancellable,
		       GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 dbus_mtime, initial_offset;
  dbus_bool_t dbus_make_backup;

  mode = 2;
  dbus_mtime = mtime;
  dbus_make_backup = make_backup;
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_OPEN_FOR_WRITE,
			     &connection, cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_UINT64, &dbus_mtime,
			     DBUS_TYPE_BOOLEAN, &dbus_make_backup,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
}

typedef struct {
  GFile *file;
  GMountOperation *mount_operation;
  GAsyncReadyCallback callback;
  gpointer user_data;
} MountData;

static void g_daemon_file_mount_for_location (GFile *location,
					      GMountOperation *mount_operation,
					      GAsyncReadyCallback callback,
					      gpointer user_data);

static void
mount_reply (DBusMessage *reply,
	     GError *error,
	     gpointer user_data)
{
  MountData *data = user_data;
  GSimpleAsyncResult *res;

  if (reply == NULL)
    {
      res = g_simple_async_result_new_from_error (G_OBJECT (data->file),
						  data->callback,
						  data->user_data,
						  error);
    }
  else
    {
      res = g_simple_async_result_new (G_OBJECT (data->file),
				       data->callback,
				       data->user_data,
				       g_daemon_file_mount_for_location);
    }

  g_simple_async_result_complete (res);
  
  g_object_unref (data->file);
  if (data->mount_operation)
    g_object_unref (data->mount_operation);
  g_free (data);
}

static void
g_daemon_file_mount_for_location (GFile *location,
				  GMountOperation *mount_operation,
				  GAsyncReadyCallback callback,
				  gpointer user_data)
{
  GDaemonFile *daemon_file;
  DBusMessage *message;
  GMountSpec *spec;
  GMountSource *mount_source;
  DBusMessageIter iter;
  MountData *data;
  
  daemon_file = G_DAEMON_FILE (location);
  
  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
					  G_VFS_DBUS_MOUNTTRACKER_PATH,
					  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					  G_VFS_DBUS_MOUNTTRACKER_OP_MOUNT_LOCATION);

  spec = g_mount_spec_copy (daemon_file->mount_spec);
  g_mount_spec_set_mount_prefix (spec, daemon_file->path);
  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, spec);
  g_mount_spec_unref (spec);

  if (mount_operation)
    mount_source = g_mount_operation_dbus_wrap (mount_operation);
  else
    mount_source = g_mount_source_new_dummy ();
  g_mount_source_to_dbus (mount_source, message);
  g_object_unref (mount_source);

  data = g_new0 (MountData, 1);
  data->callback = callback;
  data->user_data = user_data;
  data->file = g_object_ref (location);
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);
  
  _g_dbus_connection_call_async (NULL, message,
				 G_VFS_DBUS_MOUNT_TIMEOUT_MSECS,
				 mount_reply, data);
 
  dbus_message_unref (message);
}

static gboolean
g_daemon_file_mount_for_location_finish (GFile                  *location,
					 GAsyncResult           *result,
					 GError                **error)
{
  /* Errors handled in generic code */
  return TRUE;
}


static void
g_daemon_file_file_iface_init (GFileIface *iface)
{
  iface->dup = g_daemon_file_dup;
  iface->hash = g_daemon_file_hash;
  iface->equal = g_daemon_file_equal;
  iface->is_native = g_daemon_file_is_native;
  iface->get_basename = g_daemon_file_get_basename;
  iface->get_path = g_daemon_file_get_path;
  iface->get_uri = g_daemon_file_get_uri;
  iface->get_parse_name = g_daemon_file_get_parse_name;
  iface->get_parent = g_daemon_file_get_parent;
  iface->resolve_relative = g_daemon_file_resolve_relative;
  iface->enumerate_children = g_daemon_file_enumerate_children;
  iface->get_info = g_daemon_file_get_info;
  iface->read = g_daemon_file_read;
  iface->append_to = g_daemon_file_append_to;
  iface->create = g_daemon_file_create;
  iface->replace = g_daemon_file_replace;
  iface->read_async = g_daemon_file_read_async;
  iface->read_finish = g_daemon_file_read_finish;
  iface->mount_for_location = g_daemon_file_mount_for_location;
  iface->mount_for_location_finish = g_daemon_file_mount_for_location_finish;
}
