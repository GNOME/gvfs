/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gioerror.h>
#include <gio/gfile.h>

#include "gvfsbackendftp.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobgetinfo.h"
#include "gvfsjobgetfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"

struct _GVfsBackendFtp
{
  GVfsBackend parent_instance;

  GMountSource *mount_source; /* Only used/set during mount */
  int mount_try;
  gboolean mount_try_again;
};

G_DEFINE_TYPE (GVfsBackendFtp, g_vfs_backend_ftp, G_VFS_TYPE_BACKEND);

static void
g_vfs_backend_ftp_finalize (GObject *object)
{
  GVfsBackendFtp *backend;

  backend = G_VFS_BACKEND_FTP (object);

  if (G_OBJECT_CLASS (g_vfs_backend_ftp_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_ftp_parent_class)->finalize) (object);
}

static void
g_vfs_backend_ftp_init (GVfsBackendFtp *backend)
{
}

static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount (GVfsBackend *backend,
	   GVfsJobMount *job,
	   GMountSpec *mount_spec,
	   GMountSource *mount_source,
	   gboolean is_automount)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);
  const char *server, *share, *user, *domain;

  server = g_mount_spec_get (mount_spec, "server");
  share = g_mount_spec_get (mount_spec, "share");

  if (server == NULL || share == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("Invalid mount spec"));
      return TRUE;
    }

  user = g_mount_spec_get (mount_spec, "user");
  domain = g_mount_spec_get (mount_spec, "domain");

  /* TODO */

#if 0  
  op_backend->server = g_strdup (server);
  op_backend->share = g_strdup (share);
  op_backend->user = g_strdup (user);
  op_backend->domain = g_strdup (domain);
#endif

  return FALSE;
}

static void 
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  const char *filename)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */

#if 0
  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_open_for_read_set_can_seek (job, TRUE);
      g_vfs_job_open_for_read_set_handle (job, file);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_read (GVfsBackend *backend,
	 GVfsJobRead *job,
	 GVfsBackendHandle handle,
	 char *buffer,
	 gsize bytes_requested)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */

#if 0
  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_read_set_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
	    
    }
#endif
}

static void
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

#if 0
  switch (type)
    {
    case G_SEEK_SET:
      whence = SEEK_SET;
      break;
    case G_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case G_SEEK_END:
      whence = SEEK_END;
      break;
    default:
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Unsupported seek type"));
      return;
    }
#endif

  /* TODO */

#if 0
  if (res == (off_t)-1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_seek_read_set_offset (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */

#if 0
  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
#endif

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_create (GVfsBackend *backend,
	   GVfsJobOpenForWrite *job,
	   const char *filename)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */

#if 0
  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      handle = g_new0 (FtpWriteHandle, 1);
      handle->file = file;

      g_vfs_job_open_for_write_set_can_seek (job, TRUE);
      g_vfs_job_open_for_write_set_handle (job, handle);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_append_to (GVfsBackend *backend,
	      GVfsJobOpenForWrite *job,
	      const char *filename)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */

#if 0
  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      handle = g_new0 (FtpWriteHandle, 1);
      handle->file = file;

      initial_offset = op_backend->ftp_context->lseek (op_backend->ftp_context, file,
						       0, SEEK_CUR);
      if (initial_offset == (off_t) -1)
	g_vfs_job_open_for_write_set_can_seek (job, FALSE);
      else
	{
	  g_vfs_job_open_for_write_set_initial_offset (job, initial_offset);
	  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
	}
      g_vfs_job_open_for_write_set_handle (job, handle);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_replace (GVfsBackend *backend,
	    GVfsJobOpenForWrite *job,
	    const char *filename,
	    time_t mtime,
	    gboolean make_backup)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
do_write (GVfsBackend *backend,
	  GVfsJobWrite *job,
	  GVfsBackendHandle _handle,
	  char *buffer,
	  gsize buffer_size)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */

#if 0
  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_write_set_written_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_seek_on_write (GVfsBackend *backend,
		  GVfsJobSeekWrite *job,
		  GVfsBackendHandle _handle,
		  goffset    offset,
		  GSeekType  type)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
do_close_write (GVfsBackend *backend,
		GVfsJobCloseWrite *job,
		GVfsBackendHandle _handle)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
do_get_info (GVfsBackend *backend,
	     GVfsJobGetInfo *job,
	     const char *filename,
	     const char *attributes,
	     GFileGetInfoFlags flags)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
do_get_fs_info (GVfsBackend *backend,
		GVfsJobGetFsInfo *job,
		const char *filename,
		const char *attributes)
{
  /* TODO */
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
			       GVfsJobQueryAttributes *job,
			       const char *filename)
{
  /* TODO */

  return TRUE;
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      const char *attributes,
	      GFileGetInfoFlags flags)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
do_set_display_name (GVfsBackend *backend,
		     GVfsJobSetDisplayName *job,
		     const char *filename,
		     const char *display_name)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
do_delete (GVfsBackend *backend,
	   GVfsJobDelete *job,
	   const char *filename)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
do_make_directory (GVfsBackend *backend,
		   GVfsJobMakeDirectory *job,
		   const char *filename)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
do_move (GVfsBackend *backend,
	 GVfsJobMove *job,
	 const char *source,
	 const char *destination,
	 GFileCopyFlags flags,
	 GFileProgressCallback progress_callback,
	 gpointer progress_callback_data)
{
  GVfsBackendFtp *op_backend = G_VFS_BACKEND_FTP (backend);

  /* TODO */
}

static void
g_vfs_backend_ftp_class_init (GVfsBackendFtpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_ftp_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->create = do_create;
  backend_class->append_to = do_append_to;
  backend_class->replace = do_replace;
  backend_class->write = do_write;
  backend_class->seek_on_write = do_seek_on_write;
  backend_class->close_write = do_close_write;
  backend_class->get_info = do_get_info;
  backend_class->get_fs_info = do_get_fs_info;
  backend_class->enumerate = do_enumerate;
  backend_class->set_display_name = do_set_display_name;
  backend_class->delete = do_delete;
  backend_class->make_directory = do_make_directory;
  backend_class->move = do_move;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
}
