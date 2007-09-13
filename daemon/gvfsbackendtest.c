#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gvfserror.h>
#include <gio/gfile.h>
#include <gio/gfilelocal.h>

#include "gvfsbackendtest.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobgetinfo.h"
#include "gvfsjobenumerate.h"

G_DEFINE_TYPE (GVfsBackendTest, g_vfs_backend_test, G_VFS_TYPE_BACKEND);

static void
g_vfs_backend_test_finalize (GObject *object)
{
  GVfsBackendTest *backend;

  backend = G_VFS_BACKEND_TEST (object);
  
  if (G_OBJECT_CLASS (g_vfs_backend_test_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_test_parent_class)->finalize) (object);
}

static void
g_vfs_backend_test_init (GVfsBackendTest *test_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (test_backend);
  GMountSpec *mount_spec;

  g_vfs_backend_set_display_name (backend, "test");

  mount_spec = g_mount_spec_new ("test");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);
}


static gboolean
try_mount (GVfsBackend *backend,
	   GVfsJobMount *job,
	   GMountSpec *mount_spec,
	   GMountSource *mount_source)
{
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static gboolean 
open_idle_cb (gpointer data)
{
  GVfsJobOpenForRead *job = data;
  int fd;

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_VFS_ERROR,
			G_VFS_ERROR_CANCELLED,
			_("Operation was cancelled"));
      return FALSE;
    }
  
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

static void
open_read_cancelled_cb (GVfsJob *job, gpointer data)
{
  guint tag = GPOINTER_TO_INT (data);

  g_print ("open_read_cancelled_cb\n");
  
  if (g_source_remove (tag))
    g_vfs_job_failed (job, G_VFS_ERROR,
		      G_VFS_ERROR_CANCELLED,
		      _("Operation was cancelled"));
}

static gboolean 
try_open_for_read (GVfsBackend *backend,
		   GVfsJobOpenForRead *job,
		   const char *filename)
{
  GError *error;

  g_print ("try_open_for_read (%s)\n", filename);
  
  if (strcmp (filename, "/fail") == 0)
    {
      error = g_error_new (G_FILE_ERROR, G_FILE_ERROR_IO, "Test error");
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    }
  else
    {
      guint tag = g_timeout_add (0, open_idle_cb, job);
      g_signal_connect (job, "cancelled", (GCallback)open_read_cancelled_cb, GINT_TO_POINTER (tag));
    }
  
  return TRUE;
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
try_read (GVfsBackend *backend,
	  GVfsJobRead *job,
	  GVfsBackendHandle handle,
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

static void
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
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
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  int fd;

  g_print ("close ()\n");

  fd = GPOINTER_TO_INT (handle);
  close(fd);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_get_info (GVfsBackend *backend,
	     GVfsJobGetInfo *job,
	     const char *filename,
	     const char *attributes,
	     GFileGetInfoFlags flags)
{
  GFile *file;
  GFileInfo *info;
  GError *error;

  g_print ("do_get_file_info (%s)\n", filename);
  
  file = g_file_local_new (filename);

  error = NULL;
  info = g_file_get_info (file, attributes, flags,
			  NULL, &error);

  if (info)
    {
      g_vfs_job_get_info_set_info (job, info);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);

  g_object_unref (info);
  g_object_unref (file);
}

static gboolean
try_enumerate (GVfsBackend *backend,
	       GVfsJobEnumerate *job,
	       const char *filename,
	       const char *attributes,
	       GFileGetInfoFlags flags)
{
  GFileInfo *info1, *info2;;
  GList *l;

  g_print ("try_enumerate (%s)\n", filename);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  info1 = g_file_info_new ();
  info2 = g_file_info_new ();
  g_file_info_set_name (info1, "file1");
  g_file_info_set_file_type (info1, G_FILE_TYPE_REGULAR);
  g_file_info_set_name (info2, "file2");
  g_file_info_set_file_type (info2, G_FILE_TYPE_REGULAR);
  
  l = NULL;
  l = g_list_append (l, info1);
  l = g_list_append (l, info2);

  g_vfs_job_enumerate_add_info (job, l);

  g_list_free (l);
  g_object_unref (info1);
  g_object_unref (info2);

  g_vfs_job_enumerate_done (job);
  
  return TRUE;
}

static void
g_vfs_backend_test_class_init (GVfsBackendTestClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_test_finalize;

  backend_class->try_mount = try_mount;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_read = try_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->get_info = do_get_info;
  backend_class->try_enumerate = try_enumerate;
}
