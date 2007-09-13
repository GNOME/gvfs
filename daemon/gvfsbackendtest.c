#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <gvfs/gvfserror.h>
#include <glib/gi18n.h>

#include "gvfsbackendtest.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"

G_DEFINE_TYPE (GVfsBackendTest, g_vfs_backend_test, G_TYPE_VFS_BACKEND);

static void
g_vfs_backend_test_finalize (GObject *object)
{
  GVfsBackendTest *backend;

  backend = G_VFS_BACKEND_TEST (object);
  
  if (G_OBJECT_CLASS (g_vfs_backend_test_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_test_parent_class)->finalize) (object);
}

static void
g_vfs_backend_test_init (GVfsBackendTest *backend)
{
}

GVfsBackendTest *
g_vfs_backend_test_new (void)
{
  GVfsBackendTest *backend;
  backend = g_object_new (G_TYPE_VFS_BACKEND_TEST,
			 NULL);
  return backend;
}

static gboolean 
open_idle_cb (gpointer data)
{
  GVfsJobOpenForRead *job = data;
  int fd;

  fd = g_open (job->filename, O_RDONLY);
  if (fd == -1)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_FILE_ERROR,
			g_file_error_from_errno (errno),
			"Error opening file %s: %s",
			job->filename, g_strerror (errno));
    }
  else
    {
      g_vfs_job_open_for_read_set_can_seek (job, TRUE);
      g_vfs_job_open_for_read_set_handle (job, GINT_TO_POINTER (fd));
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  return FALSE;
}

static gboolean 
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  char *filename)
{
  GError *error;

  g_print ("open_for_read (%s)\n", filename);
  
  if (strcmp (filename, "/fail") == 0)
    {
      error = g_error_new (G_FILE_ERROR, G_FILE_ERROR_IO, "Test error");
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      return TRUE;
    }
  else
    {
      g_idle_add (open_idle_cb, job);
      return TRUE;
    }
}

static gboolean 
read_idle_cb (gpointer data)
{
  GVfsJobRead *job = data;
  int fd;
  ssize_t res;

  fd = GPOINTER_TO_INT (job->handle);

  res = read (fd, job->buffer, job->bytes_requested);

  if (res == -1)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_FILE_ERROR,
			g_file_error_from_errno (errno),
			"Error reading from file: %s",
			g_strerror (errno));
    }
  else
    {
      g_vfs_job_read_set_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  
  return FALSE;
}

static void
read_cancelled_cb (GVfsJob *job, gpointer data)
{
  guint tag = GPOINTER_TO_INT (job->backend_data);

  g_source_remove (tag);
  g_vfs_job_failed (job, G_VFS_ERROR,
		    G_VFS_ERROR_CANCELLED,
		    _("Operation was cancelled"));
}

static gboolean
do_read (GVfsBackend *backend,
	 GVfsJobRead *job,
	 GVfsHandle *handle,
	 char *buffer,
	 gsize bytes_requested)
{
  guint tag;

  g_print ("read (%d)\n", bytes_requested);

  tag = g_timeout_add (0, read_idle_cb, job);
  G_VFS_JOB (job)->backend_data = GINT_TO_POINTER (tag);
  g_signal_connect (job, "cancelled", (GCallback)read_cancelled_cb, NULL);
  
  return TRUE;
}

static gboolean
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsHandle *handle,
		 goffset    offset,
		 GSeekType  type)
{
  int whence;
  int fd;
  off_t final_offset;

  g_print ("seek_on_read (%d, %d)\n", (int)offset, type);

  switch (type)
    {
    default:
    case G_SEEK_SET:
      whence = SEEK_SET;
      break;
    case G_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case G_SEEK_END:
      whence = SEEK_END;
      break;
    }
      
  
  fd = GPOINTER_TO_INT (handle);

  final_offset = lseek (fd, offset, whence);
  
  if (final_offset == (off_t)-1)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_FILE_ERROR,
			g_file_error_from_errno (errno),
			"Error seeking in file: %s",
			g_strerror (errno));
    }
  else
    {
      g_vfs_job_seek_read_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

  return TRUE;
}

static gboolean
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsHandle *handle)
{
  int fd;

  g_print ("close ()\n");

  fd = GPOINTER_TO_INT (handle);
  close(fd);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  return TRUE;
}

static void
g_vfs_backend_test_class_init (GVfsBackendTestClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_test_finalize;

  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
}
