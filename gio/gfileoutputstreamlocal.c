#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include "gvfserror.h"
#include "gfileoutputstreamlocal.h"
#include "gfileinfolocal.h"

G_DEFINE_TYPE (GFileOutputStreamLocal, g_file_output_stream_local, G_TYPE_FILE_OUTPUT_STREAM);

/* Some of the file replacement code was based on the code from gedit,
 * relicenced to LGPL with permissions from the authors.
 */
  

#define BACKUP_EXTENSION "~"

struct _GFileOutputStreamLocalPrivate {
  char *tmp_filename;
  char *original_filename;
  char *backup_filename;
  int fd;
};

static gssize     g_file_output_stream_local_write         (GOutputStream          *stream,
							    void                   *buffer,
							    gsize                   count,
							    GCancellable           *cancellable,
							    GError                **error);
static gboolean   g_file_output_stream_local_close         (GOutputStream          *stream,
							    GCancellable           *cancellable,
							    GError                **error);
static GFileInfo *g_file_output_stream_local_get_file_info (GFileOutputStream      *stream,
							    GFileInfoRequestFlags   requested,
							    char                   *attributes,
							    GCancellable           *cancellable,
							    GError                **error);


static void
g_file_output_stream_local_finalize (GObject *object)
{
  GFileOutputStreamLocal *file;
  
  file = G_FILE_OUTPUT_STREAM_LOCAL (object);
  
  g_free (file->priv->tmp_filename);
  g_free (file->priv->original_filename);
  g_free (file->priv->backup_filename);
  
  if (G_OBJECT_CLASS (g_file_output_stream_local_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_output_stream_local_parent_class)->finalize) (object);
}

static void
g_file_output_stream_local_class_init (GFileOutputStreamLocalClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);
  GFileOutputStreamClass *file_stream_class = G_FILE_OUTPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GFileOutputStreamLocalPrivate));
  
  gobject_class->finalize = g_file_output_stream_local_finalize;

  stream_class->write = g_file_output_stream_local_write;
  stream_class->close = g_file_output_stream_local_close;
  file_stream_class->get_file_info = g_file_output_stream_local_get_file_info;
}

static void
g_file_output_stream_local_init (GFileOutputStreamLocal *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_FILE_OUTPUT_STREAM_LOCAL,
					      GFileOutputStreamLocalPrivate);
}

static gssize
g_file_output_stream_local_write (GOutputStream *stream,
				  void         *buffer,
				  gsize         count,
				  GCancellable *cancellable,
				  GError      **error)
{
  GFileOutputStreamLocal *file;
  gssize res;

  file = G_FILE_OUTPUT_STREAM_LOCAL (stream);

  while (1)
    {
      if (g_cancellable_is_cancelled (cancellable))
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  return -1;
	}
      res = write (file->priv->fd, buffer, count);
      if (res == -1)
	{
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error writing to file': %s"),
		       g_strerror (errno));
	}
      
      break;
    }
  
  return res;
}

static gboolean
g_file_output_stream_local_close (GOutputStream *stream,
				  GCancellable *cancellable,
				  GError      **error)
{
  GFileOutputStreamLocal *file;
  struct stat final_stat;
  int res;

  file = G_FILE_OUTPUT_STREAM_LOCAL (stream);

  if (file->priv->tmp_filename)
    {
      /* We need to move the temp file to its final place,
       * and possibly create the backup file
       */

      if (file->priv->backup_filename)
	{
	  if (g_cancellable_is_cancelled (cancellable))
	    {
	      g_set_error (error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      goto err_out;
	    }
	  
	  /* create original -> backup link, the original is then renamed over */
	  if (link (file->priv->original_filename, file->priv->backup_filename) != 0)
	    {
	      g_set_error (error, G_FILE_ERROR,
			   g_file_error_from_errno (errno),
			   _("Error creating backup link: %s"),
			   g_strerror (errno));
	      goto err_out;
	    }
	}
      

      if (g_cancellable_is_cancelled (cancellable))
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  goto err_out;
	}

      /* tmp -> original */
      if (rename (file->priv->tmp_filename, file->priv->original_filename) != 0)
	{
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error renamining temporary file: %s"),
		       g_strerror (errno));
	  goto err_out;
	}
    }
  
  if (g_file_output_stream_get_should_get_final_mtime (G_FILE_OUTPUT_STREAM (stream)))
    {
      if (g_cancellable_is_cancelled (cancellable))
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  goto err_out;
	}
      
      if (fstat (file->priv->fd, &final_stat) == 0)
	g_file_output_stream_set_final_mtime (G_FILE_OUTPUT_STREAM (stream),
					      final_stat.st_mtime);
    }

  while (1)
    {
      res = close (file->priv->fd);
      if (res == -1)
	{
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error closing file: %s"),
		       g_strerror (errno));
	}
      break;
    }
  
  return res != -1;

 err_out:
  /* A simple try to close the fd in case we fail before the actual close */
  close (file->priv->fd);
  return FALSE;
}

static GFileInfo *
g_file_output_stream_local_get_file_info (GFileOutputStream     *stream,
					  GFileInfoRequestFlags requested,
					  char                 *attributes,
					  GCancellable         *cancellable,
					  GError              **error)
{
  GFileOutputStreamLocal *file;

  file = G_FILE_OUTPUT_STREAM_LOCAL (stream);

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  return g_file_info_local_get_from_fd (file->priv->fd,
					requested,
					attributes,
					error);
}

GFileOutputStream *
g_file_output_stream_local_create  (const char   *filename,
				    GCancellable *cancellable,
				    GError     **error)
{
  GFileOutputStreamLocal *stream;
  int fd;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  fd = g_open (filename,
	       O_CREAT | O_EXCL | O_WRONLY,
	       0666);
  if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error opening file '%s': %s"),
		   filename, g_strerror (errno));
      return NULL;
    }
  
  stream = g_object_new (G_TYPE_FILE_OUTPUT_STREAM_LOCAL, NULL);
  stream->priv->fd = fd;
  return G_FILE_OUTPUT_STREAM (stream);
}

GFileOutputStream *
g_file_output_stream_local_append  (const char   *filename,
				    GCancellable *cancellable,
				    GError      **error)
{
  GFileOutputStreamLocal *stream;
  int fd;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  fd = g_open (filename,
	       O_CREAT | O_APPEND | O_WRONLY,
	       0666);
  if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error opening file '%s': %s"),
		   filename, g_strerror (errno));
      return NULL;
    }
  
  stream = g_object_new (G_TYPE_FILE_OUTPUT_STREAM_LOCAL, NULL);
  stream->priv->fd = fd;
  
  return G_FILE_OUTPUT_STREAM (stream);
}

static char *
create_backup_filename (const char *filename)
{
  return g_strconcat (filename, BACKUP_EXTENSION, NULL);
}

#define BUFSIZE	8192 /* size of normal write buffer */

static gboolean
copy_file_data (gint     sfd,
		gint     dfd,
		GError **error)
{
  gboolean ret = TRUE;
  gpointer buffer;
  const gchar *write_buffer;
  ssize_t bytes_read;
  ssize_t bytes_to_write;
  ssize_t bytes_written;
  
  buffer = g_malloc (BUFSIZE);
  
  do
    {
      bytes_read = read (sfd, buffer, BUFSIZE);
      if (bytes_read == -1)
	{
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error reading from file': %s"),
		       g_strerror (errno));
	  ret = FALSE;
	  break;
	}
      
      bytes_to_write = bytes_read;
      write_buffer = buffer;
      
      do
	{
	  bytes_written = write (dfd, write_buffer, bytes_to_write);
	  if (bytes_written == -1)
	    {
	      if (errno == EINTR)
		continue;
	      
	      g_set_error (error, G_FILE_ERROR,
			   g_file_error_from_errno (errno),
			   _("Error writing to file: %s"),
			   g_strerror (errno));
	      ret = FALSE;
	      break;
	    }
	  
	  bytes_to_write -= bytes_written;
	  write_buffer += bytes_written;
	}
      while (bytes_to_write > 0);
      
    } while ((bytes_read != 0) && (ret == TRUE));

  g_free (buffer);
  
  return ret;
}

static int
handle_overwrite_open (const char *filename,
		       time_t original_mtime,
		       gboolean create_backup,
		       char **temp_filename,
		       GCancellable *cancellable,
		       GError      **error)
{
  int fd = -1;
  struct stat original_stat;
  gboolean is_symlink;
  int open_flags;

  /* We only need read access to the original file if we are creating a backup.
   * We also add O_CREATE to avoid a race if the file was just removed */
  if (create_backup)
    open_flags = O_RDWR | O_CREAT;
  else
    open_flags = O_WRONLY | O_CREAT;
  
  /* Some systems have O_NOFOLLOW, which lets us avoid some races
   * when finding out if the file we opened was a symlink */
#ifdef O_NOFOLLOW
  is_symlink = FALSE;
  fd = g_open (filename, open_flags | O_NOFOLLOW, 0666);
  if (fd == -1 && errno == ELOOP)
    {
      /* Could be a symlink, or it could be a regular ELOOP error,
       * but then the next open will fail too. */
      is_symlink = TRUE;
      fd = g_open (filename, open_flags, 0666);
    }
#else
  fd = g_open (filename, open_flags, 0666);
  /* This is racy, but we do it as soon as possible to minimize the race */
  is_symlink = g_file_test (filename, G_FILE_TEST_IS_SYMLINK);
#endif
    
  if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error opening file '%s': %s"),
		   filename, g_strerror (errno));
      return -1;
    }
  
  if (fstat (fd, &original_stat) != 0) 
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error stating file '%s': %s"),
		   filename, g_strerror (errno));
      goto err_out;
    }
  
  /* not a regular file */
  if (!S_ISREG (original_stat.st_mode))
    {
      if (S_ISDIR (original_stat.st_mode))
	g_set_error (error,
		     G_FILE_ERROR,
		     G_FILE_ERROR_ISDIR,
		     _("Target file is a directory"));
      else
	g_set_error (error,
		     G_VFS_ERROR,
		     G_VFS_ERROR_NOT_REGULAR_FILE,
		     _("Target file is not a regular file"));
      goto err_out;
    }
  
  if (original_mtime != 0 &&
      original_stat.st_mtime != original_mtime)
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_WRONG_MTIME,
		   _("The file was externally modified"));
      goto err_out;
    }

  /* We use two backup strategies.
   * The first one (which is faster) consist in saving to a
   * tmp file then rename the original file to the backup and the
   * tmp file to the original name. This is fast but doesn't work
   * when the file is a link (hard or symbolic) or when we can't
   * write to the current dir or can't set the permissions on the
   * new file. 
   * The second strategy consist simply in copying the old file
   * to a backup file and rewrite the contents of the file.
   */
  
  if (!(original_stat.st_nlink > 1) && !is_symlink)
    {
      char *dirname, *tmp_filename;
      int tmpfd;
      
      dirname = g_path_get_dirname (filename);
      tmp_filename = g_build_filename (dirname, ".goutputstream-XXXXXX", NULL);
      g_free (dirname);

      tmpfd = g_mkstemp (tmp_filename);
      if (tmpfd == -1)
	{
	  g_free (tmp_filename);
	  goto fallback_strategy;
	}
      
      /* try to keep permissions */
      if (fchown (tmpfd, original_stat.st_uid, original_stat.st_gid) == -1 ||
	  fchmod (tmpfd, original_stat.st_mode) == -1)
	{
	  close (tmpfd);
	  unlink (tmp_filename);
	  g_free (tmp_filename);
	  goto fallback_strategy;
	}

      close (fd);
      *temp_filename = tmp_filename;
      return tmpfd;
    }

 fallback_strategy:

  if (create_backup)
    {
      char *backup_filename;
      int bfd;
      
      backup_filename = create_backup_filename (filename);

      if (unlink (backup_filename) == -1 && errno != ENOENT)
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANT_CREATE_BACKUP,
		       _("Backup file creation failed"));
	  g_free (backup_filename);
	  goto err_out;
	}

      bfd = g_open (backup_filename,
		    O_WRONLY | O_CREAT | O_EXCL,
		    original_stat.st_mode & 0777);

      if (bfd == -1)
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANT_CREATE_BACKUP,
		       _("Backup file creation failed"));
	  g_free (backup_filename);
	  goto err_out;
	}

      /* Try to set the group of the backup same as the
       * original file. If this fails, set the protection
       * bits for the group same as the protection bits for
       * others. */
      if (fchown (bfd, (uid_t) -1, original_stat.st_gid) != 0)
	{
	  if (fchmod (bfd,
		      (original_stat.st_mode & 0707) |
		      ((original_stat.st_mode & 07) << 3)) != 0)
	    {
	      g_set_error (error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANT_CREATE_BACKUP,
			   _("Backup file creation failed"));
	      unlink (backup_filename);
	      close (bfd);
	      g_free (backup_filename);
	      goto err_out;
	    }
	}

      if (!copy_file_data (fd, bfd, NULL))
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANT_CREATE_BACKUP,
		       _("Backup file creation failed"));
	  unlink (backup_filename);
	  close (bfd);
	  g_free (backup_filename);
	  
	  goto err_out;
	}
      
      close (bfd);
      g_free (backup_filename);

      /* Seek back to the start of the file after the backup copy */
      if (lseek (fd, 0, SEEK_SET) == -1)
	{
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error seeking in file: %s"),
		       g_strerror (errno));
	  goto err_out;
	}
    }

  /* Truncate the file at the start */
  if (ftruncate (fd, 0) == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error truncating file: %s"),
		   g_strerror (errno));
      goto err_out;
    }
    
  return fd;

 err_out:
  close (fd);
  return -1;
}

GFileOutputStream *
g_file_output_stream_local_replace (const char   *filename,
				    time_t        original_mtime,
				    gboolean      create_backup,
				    GCancellable *cancellable,
				    GError      **error)
{
  GFileOutputStreamLocal *stream;
  int fd;
  char *temp_file;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }


  temp_file = NULL;
  /* If the file doesn't exist, create it */
  fd = g_open (filename,
	       O_CREAT | O_EXCL | O_WRONLY,
	       0666);

  if (fd == -1 && errno == EEXIST)
    {
      /* The file already exists */
      fd = handle_overwrite_open (filename, original_mtime, create_backup, &temp_file,
				  cancellable, error);
      if (fd == -1)
	return NULL;
    }
  else if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error opening file '%s': %s"),
		   filename, g_strerror (errno));
      return NULL;
    }
  
 
  stream = g_object_new (G_TYPE_FILE_OUTPUT_STREAM_LOCAL, NULL);
  stream->priv->fd = fd;
  stream->priv->tmp_filename = temp_file;
  stream->priv->backup_filename = create_backup_filename (filename);
  stream->priv->original_filename =  g_strdup (filename);
  
  return G_FILE_OUTPUT_STREAM (stream);
}
