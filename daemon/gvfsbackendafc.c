/*
 * gvfs/daemon/gvfsbackendafc.c
 *
 * Copyright (c) 2008 Patrick Walton <pcwalton@cs.ucla.edu>
 */

#include <config.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <glib/gi18n.h>
#include <errno.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>

#include "gvfsbackendafc.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsdaemonutils.h"

#define G_VFS_BACKEND_AFC_MAX_FILE_SIZE G_MAXINT64
int g_blocksize = 4096; /* assume this is the default block size */

/* AFC_E_INVALID_ARGUMENT was renamed between 0.9.7 and 1.0.0 */
#ifndef AFC_E_INVALID_ARG
#define AFC_E_INVALID_ARG AFC_E_INVALID_ARGUMENT
#endif /* !AFC_E_INVALID_ARG */

struct _GVfsBackendAfc {
  GVfsBackend backend;

  char uuid[41];
  char *service;
  char *model;
  gboolean connected;

  idevice_t dev;
  afc_client_t afc_cli;
};

struct afc_error_mapping {
  afc_error_t from;
  GIOErrorEnum to;
};

static struct afc_error_mapping afc_error_to_g_io_error[] = {
      { AFC_E_UNKNOWN_ERROR            , G_IO_ERROR_FAILED },
      { AFC_E_OP_HEADER_INVALID        , G_IO_ERROR_FAILED },
      { AFC_E_NO_RESOURCES             , G_IO_ERROR_TOO_MANY_OPEN_FILES },
      { AFC_E_READ_ERROR               , G_IO_ERROR_NOT_DIRECTORY },
      { AFC_E_WRITE_ERROR              , G_IO_ERROR_FAILED },
      { AFC_E_UNKNOWN_PACKET_TYPE      , G_IO_ERROR_FAILED },
      { AFC_E_INVALID_ARG              , G_IO_ERROR_INVALID_ARGUMENT },
      { AFC_E_OBJECT_NOT_FOUND         , G_IO_ERROR_NOT_FOUND },
      { AFC_E_OBJECT_IS_DIR            , G_IO_ERROR_IS_DIRECTORY },
      { AFC_E_DIR_NOT_EMPTY            , G_IO_ERROR_NOT_EMPTY },
      { AFC_E_PERM_DENIED              , G_IO_ERROR_PERMISSION_DENIED },
      { AFC_E_SERVICE_NOT_CONNECTED    , G_IO_ERROR_HOST_NOT_FOUND },
      { AFC_E_OP_TIMEOUT               , G_IO_ERROR_TIMED_OUT },
      { AFC_E_TOO_MUCH_DATA            , G_IO_ERROR_FAILED },
      { AFC_E_END_OF_DATA              , G_IO_ERROR_FAILED },
      { AFC_E_OP_NOT_SUPPORTED         , G_IO_ERROR_NOT_SUPPORTED },
      { AFC_E_OBJECT_EXISTS            , G_IO_ERROR_EXISTS },
      { AFC_E_OBJECT_BUSY              , G_IO_ERROR_BUSY },
      { AFC_E_NO_SPACE_LEFT            , G_IO_ERROR_NO_SPACE },
      { AFC_E_OP_WOULD_BLOCK           , G_IO_ERROR_WOULD_BLOCK },
      { AFC_E_IO_ERROR                 , G_IO_ERROR_FAILED },
      { AFC_E_OP_INTERRUPTED           , G_IO_ERROR_CANCELLED },
      { AFC_E_OP_IN_PROGRESS           , G_IO_ERROR_PENDING },
      { AFC_E_INTERNAL_ERROR           , G_IO_ERROR_FAILED },
      { AFC_E_NOT_ENOUGH_DATA          , G_IO_ERROR_CLOSED },
      { AFC_E_MUX_ERROR                , G_IO_ERROR_FAILED },
      { -1 }
};

/**
 * Tries to convert the AFC error value into a GIOError.
 *
 * @param client AFC client to retrieve status value from.
 *
 * @return errno value.
 */
static GIOErrorEnum
g_io_error_from_afc_error (afc_error_t error)
{
  GIOErrorEnum res = G_IO_ERROR_FAILED;
  int i = 0; gboolean found = FALSE;

  while (afc_error_to_g_io_error[i++].from != -1)
    {
      if (afc_error_to_g_io_error[i].from == error)
        {
          res = afc_error_to_g_io_error[i++].to;
          found = TRUE;
          break;
        }
    }

  if (!found)
    g_warning ("Unknown AFC error (%d).\n", error);

  return res;
}

G_DEFINE_TYPE(GVfsBackendAfc, g_vfs_backend_afc, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_afc_close_connection (GVfsBackendAfc *self)
{
  if (self->connected)
    {
      afc_client_free (self->afc_cli);
      g_free (self->model);
      self->model = NULL;
      idevice_free (self->dev);
    }
  self->connected = FALSE;
}

static int
g_vfs_backend_afc_check (afc_error_t cond, GVfsJob *job)
{
  GIOErrorEnum error;

  if (G_LIKELY(cond == AFC_E_SUCCESS))
        return 0;

  error = g_io_error_from_afc_error (cond);
  switch (cond)
    {
    case AFC_E_INTERNAL_ERROR:
      g_vfs_job_failed (job, G_IO_ERROR, error,
                        _("Internal Apple File Control error"));
      break;
    case AFC_E_OBJECT_NOT_FOUND:
      g_vfs_job_failed (job, G_IO_ERROR, error,
                        _("File does not exist"));
    case AFC_E_DIR_NOT_EMPTY:
      g_vfs_job_failed (job, G_IO_ERROR, error,
                        _("The directory is not empty"));
      break;
    case AFC_E_OP_TIMEOUT:
      g_vfs_job_failed (job, G_IO_ERROR, error,
                        _("The device did not respond"));
      break;
    case AFC_E_NOT_ENOUGH_DATA:
      g_vfs_job_failed (job, G_IO_ERROR, error,
                        _("The connection was interrupted"));
      break;
    case AFC_E_MUX_ERROR:
      g_vfs_job_failed (job, G_IO_ERROR, error,
                        _("Invalid Apple File Control data received"));
      break;
    default:
      g_vfs_job_failed (job, G_IO_ERROR, error,
                        _("Unhandled Apple File Control error (%d)"), cond);
      break;
    }

  return 1;
}

static int
g_vfs_backend_lockdownd_check (lockdownd_error_t cond, GVfsJob *job)
{
  if (G_LIKELY(cond == LOCKDOWN_E_SUCCESS))
        return 0;

  switch (cond)
    {
    case LOCKDOWN_E_INVALID_ARG:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Lockdown Error: Invalid Argument"));
      break;
    case LOCKDOWN_E_PASSWORD_PROTECTED:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        _("Permission denied"));
    default:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Unhandled Lockdown error (%d)"), cond);
      break;
    }

  return 1;
}

static int
g_vfs_backend_idevice_check (idevice_error_t cond, GVfsJob *job)
{
  if (G_LIKELY(cond == IDEVICE_E_SUCCESS))
        return 0;

  switch (cond)
    {
    case IDEVICE_E_INVALID_ARG:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("libimobiledevice Error: Invalid Argument"));
      break;
    case IDEVICE_E_NO_DEVICE:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("libimobiledevice Error: No device found. Make sure usbmuxd is set up correctly."));
    default:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Unhandled libimobiledevice error (%d)"), cond);
      break;
    }

  return 1;
}

static void
_idevice_event_cb (const idevice_event_t *event, void *user_data)
{
  GVfsBackendAfc *afc_backend = G_VFS_BACKEND_AFC (user_data);

  g_return_if_fail (afc_backend->uuid != NULL);
  if (event->event != IDEVICE_DEVICE_REMOVE)
    return;
  if (g_str_equal (event->uuid, afc_backend->uuid) == FALSE)
    return;

  g_print ("Shutting down AFC backend for device uuid %s\n", afc_backend->uuid);

  g_vfs_backend_afc_close_connection (afc_backend);

  idevice_event_unsubscribe ();

  /* TODO: need a cleaner way to force unmount ourselves */
  exit (1);
}

/* Callback for mounting. */
static void
g_vfs_backend_afc_mount (GVfsBackend *backend,
                         GVfsJobMount *job,
                         GMountSpec *spec,
                         GMountSource *src,
                         gboolean automounting)
{
  const char *str;
  char *tmp;
  char *display_name = NULL;
  guint16 port;
  int virtual_port;
  GMountSpec *real_spec;
  GVfsBackendAfc *self;
  int retries;
  idevice_error_t err;
  lockdownd_client_t lockdown_cli = NULL;
  char *camera_x_content_types[] = { "x-content/audio-player", "x-content/image-dcf", NULL};
  char *media_player_x_content_types[] = {"x-content/audio-player", NULL};
  char **dcim_afcinfo;
  plist_t value;
  lockdownd_error_t lerr;
  const gchar *choices[] = {_("Cancel"), _("Try again")};
  gboolean aborted = FALSE;
  gchar *message = NULL;
  gint choice;
  gboolean ret;

  self = G_VFS_BACKEND_AFC(backend);
  self->connected = FALSE;

  idevice_event_subscribe (_idevice_event_cb, self);

  /* setup afc */

  str = g_mount_spec_get(spec, "host");
  if (G_UNLIKELY(str == NULL))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Invalid mount spec"));
      return;
    }
  if (G_UNLIKELY(sscanf(str, "%40s", (char *) &self->uuid) < 1))
    {
      g_vfs_job_failed (G_VFS_JOB(job), G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid AFC location: must be in the form of "
                          "afc://uuid:port-number"));
      return;
    }

  str = g_mount_spec_get (spec, "port");
  if (str == NULL)
    virtual_port = 1;
  else
    virtual_port = atoi (str);

  /* set a generic display name */
  if (virtual_port >= 2)
    {
      self->service = g_strdup_printf ("com.apple.afc%d", virtual_port);
      display_name = g_strdup_printf (_("Service %d on Apple Mobile Device"),
                                      virtual_port);
    }
  else
    {
      self->service = g_strdup ("com.apple.afc");
      display_name = g_strdup_printf (_("Apple Mobile Device"));
    }

  g_vfs_backend_set_display_name (G_VFS_BACKEND(self), display_name);
  g_free (display_name);
  display_name = NULL;

  real_spec = g_mount_spec_new ("afc");
  tmp = g_strdup_printf ("%40s", (char *) &self->uuid);
  g_mount_spec_set (real_spec, "host", tmp);
  g_free (tmp);

  /* INFO: Don't ever set the DefaultPort again or everything goes crazy */
  if (virtual_port != 1)
    {
      tmp = g_strdup_printf ("%d", virtual_port);
      g_mount_spec_set (real_spec, "port", tmp);
      g_free (tmp);
    }

  g_vfs_backend_set_mount_spec (G_VFS_BACKEND(self), real_spec);
  g_mount_spec_unref (real_spec);

  retries = 0;
  do {
      err = idevice_new(&self->dev, self->uuid);
      if (err == IDEVICE_E_SUCCESS)
          break;
      g_usleep (G_USEC_PER_SEC);
  } while (retries++ < 10);

  if (G_UNLIKELY(g_vfs_backend_idevice_check(err, G_VFS_JOB(job))))
    goto out_destroy_service;

  /* first, connect without handshake to get preliminary information */
  if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lockdownd_client_new (self->dev, &lockdown_cli, "gvfsd-afc"), G_VFS_JOB(job))))
    goto out_destroy_dev;

  /* try to use pretty device name */
  if (LOCKDOWN_E_SUCCESS == lockdownd_get_device_name (lockdown_cli, &display_name))
    {
      if (display_name)
        {
          if (virtual_port >= 2)
            {
              /* translators:
               * This is the device name, with the service being browsed in brackets, eg.:
               * Alan Smithee's iPhone (Service 2 on Apple Mobile Device */
              g_vfs_backend_set_display_name (G_VFS_BACKEND(self),
                                              g_strdup_printf (_("%s (%s)"), display_name, self->service));
            }
          else
            {
              g_vfs_backend_set_display_name (G_VFS_BACKEND(self), display_name);
            }
        }
    }

  /* set correct fd icon spec name depending on device model */
  value = NULL;
  if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lockdownd_get_value (lockdown_cli, NULL, "DeviceClass", &value), G_VFS_JOB(job))))
    goto out_destroy_lockdown;

  plist_get_string_val (value, &self->model);
  if ((self->model != NULL) && (g_str_equal (self->model, "iPod") != FALSE))
    {
      g_vfs_backend_set_icon_name (G_VFS_BACKEND(self), "multimedia-player-apple-ipod-touch");
    }
  else if ((self->model != NULL) && (g_str_equal (self->model, "iPad") != FALSE))
    {
      g_vfs_backend_set_icon_name (G_VFS_BACKEND(self), "computer-apple-ipad");
    }
  else
    {
      g_vfs_backend_set_icon_name (G_VFS_BACKEND(self), "phone-apple-iphone");
    }

  lockdownd_client_free (lockdown_cli);
  lockdown_cli = NULL;

  /* now, try to connect with handshake */
  do {
    lerr = lockdownd_client_new_with_handshake (self->dev,
                                                &lockdown_cli,
                                                "gvfsd-afc");
    if (lerr != LOCKDOWN_E_PASSWORD_PROTECTED)
      break;

    aborted = FALSE;
    if (!message)
      /* translators:
       * %s is the device name. 'Try again' is the caption of the button
       * shown in the dialog which is defined above. */
      message = g_strdup_printf (_("Device '%s' is password protected. Enter the password on the device and click 'Try again'."), display_name);

    ret = g_mount_source_ask_question (src,
                                       message,
                                       choices,
                                       2,
                                       &aborted,
                                       &choice);
    if (!ret || aborted || (choice == 0))
      break;
  } while (1);

  g_free (display_name);
  display_name = NULL;
  g_free (message);

  if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lerr, G_VFS_JOB(job))))
    goto out_destroy_dev;

  if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lockdownd_start_service (lockdown_cli,
                                                                         self->service, &port),
                                                G_VFS_JOB(job))))
    {
      goto out_destroy_lockdown;
    }
  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_client_new (self->dev,
                                                          port, &self->afc_cli),
                                          G_VFS_JOB(job))))
    {
      goto out_destroy_lockdown;
    }

  /* lockdown connection is not needed anymore */
  lockdownd_client_free (lockdown_cli);

  /* Add camera item if necessary */
  if (virtual_port < 2)
    {
      dcim_afcinfo = NULL;
      if (afc_get_file_info (self->afc_cli, "/DCIM", &dcim_afcinfo) == AFC_E_SUCCESS)
        g_vfs_backend_set_x_content_types (backend, camera_x_content_types);
      else
        g_vfs_backend_set_x_content_types (backend, media_player_x_content_types);
      g_strfreev (dcim_afcinfo);
    }

  self->connected = TRUE;
  g_vfs_job_succeeded (G_VFS_JOB(job));
  return;

out_destroy_lockdown:
  lockdownd_client_free (lockdown_cli);

out_destroy_dev:
  idevice_free (self->dev);

out_destroy_service:
  g_free (self->service);
  g_free (self->model);
  g_free (display_name);
}

static void
g_vfs_backend_afc_unmount (GVfsBackend *backend,
                           GVfsJobUnmount *job,
                           GMountUnmountFlags flags,
                           GMountSource *mount_source)
{
  GVfsBackendAfc *self;

  /* FIXME: check on G_MOUNT_UNMOUNT_FORCE flag */
  self = G_VFS_BACKEND_AFC (backend);
  g_vfs_backend_afc_close_connection (self);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static gboolean file_get_info (GVfsBackendAfc *backend, const char *path, GFileInfo *info);

static gboolean
is_directory (GVfsBackendAfc *backend,
              const char *path)
{
  gboolean result = FALSE;
  GFileInfo *info;

  info = g_file_info_new();
  if (file_get_info (backend, path, info))
    {
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        result = TRUE;
    }

  g_object_unref (info);
  return result;
}

static gboolean
is_regular (GVfsBackendAfc *backend,
            const char *path)
{
  gboolean result = FALSE;
  GFileInfo *info;

  info = g_file_info_new();
  if (file_get_info (backend, path, info))
    {
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR)
	result = TRUE;
    }

  g_object_unref (info);
  return result;
}

/* Callback to open an existing file for reading. */
static void
g_vfs_backend_afc_open_for_read (GVfsBackend *backend,
                                 GVfsJobOpenForRead *job,
                                 const char *path)
{
  uint64_t fd = 0;
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (is_directory (self, path))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_IS_DIRECTORY,
                        _("Can't open directory"));
      return;
    }

  if (!is_regular (self, path))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("File doesn't exist"));
      return;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_open (self->afc_cli,
                                                         path, AFC_FOPEN_RDONLY, &fd),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_open_for_read_set_handle (job, GUINT_TO_POINTER((gulong) fd));
  g_vfs_job_open_for_read_set_can_seek (job, TRUE);
  g_vfs_job_succeeded (G_VFS_JOB(job));

  return;
}

/* Callback to open a nonexistent file for writing. */
static void
g_vfs_backend_afc_create (GVfsBackend *backend,
                          GVfsJobOpenForWrite *job,
                          const char *path,
                          GFileCreateFlags flags)
{
  uint64_t fd = 0;
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_open (self->afc_cli,
                                                         path, AFC_FOPEN_RW, &fd),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_open_for_write_set_handle (job, GUINT_TO_POINTER((gulong)fd));
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_succeeded (G_VFS_JOB(job));

  return;
}

/* Callback to open a possibly-existing file for writing. */
static void
g_vfs_backend_afc_append_to (GVfsBackend *backend,
                             GVfsJobOpenForWrite *job,
                             const char *path,
                             GFileCreateFlags flags)
{
  uint64_t fd = 0;
  uint64_t off = 0;
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_open (self->afc_cli,
                                                         path, AFC_FOPEN_RW, &fd),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_seek (self->afc_cli,
                                                         fd, 0, SEEK_END),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_tell (self->afc_cli,
                                                         fd, &off),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_open_for_write_set_handle (job, GUINT_TO_POINTER((gulong)fd));
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_initial_offset (job, off);
  g_vfs_job_succeeded (G_VFS_JOB(job));

  return;
}

static void
g_vfs_backend_afc_replace (GVfsBackend *backend,
                           GVfsJobOpenForWrite *job,
                           const char *filename,
                           const char *etag,
                           gboolean make_backup,
                           GFileCreateFlags flags)
{
  uint64_t fd = 0;
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail(self->connected);

  if (make_backup)
    {
      /* FIXME: implement! */
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Backups are not yet supported."));
      return;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_open (self->afc_cli,
                                                         filename, AFC_FOPEN_WR, &fd),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_open_for_write_set_handle (job, GUINT_TO_POINTER((gulong)fd));
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_succeeded (G_VFS_JOB(job));

  return;
}

/* Callback to close a file that was previously opened for reading. */
static void
g_vfs_backend_afc_close_read (GVfsBackend *backend,
                              GVfsJobCloseRead *job,
                              GVfsBackendHandle handle)
{
  GVfsBackendAfc *self;
  uint64_t fd = 0;

  fd = GPOINTER_TO_UINT(handle);
  g_return_if_fail (fd != 0);

  self = G_VFS_BACKEND_AFC(backend);

  if (self->connected)
    afc_file_close (self->afc_cli, fd);

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_close_write (GVfsBackend *backend,
                               GVfsJobCloseWrite *job,
                               GVfsBackendHandle handle)
{
  GVfsBackendAfc *self;
  uint64_t fd = 0;

  fd = GPOINTER_TO_UINT(handle);
  g_return_if_fail (fd != 0);

  self = G_VFS_BACKEND_AFC(backend);

  if (self->connected)
    afc_file_close(self->afc_cli, fd);

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_read (GVfsBackend *backend,
                        GVfsJobRead *job,
                        GVfsBackendHandle handle,
                        char *buffer,
                        gsize req)
{
  guint32 nread = 0;
  GVfsBackendAfc *self;
  uint64_t fd = 0;

  fd = GPOINTER_TO_UINT(handle);
  g_return_if_fail (fd != 0);

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (req > 0 &&
      G_UNLIKELY(g_vfs_backend_afc_check (afc_file_read (self->afc_cli,
                                                         fd, buffer, req, &nread),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_read_set_size (job, nread);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_write (GVfsBackend *backend,
                         GVfsJobWrite *job,
                         GVfsBackendHandle handle,
                         char *buffer,
                         gsize sz)
{
  guint32 nwritten = 0;
  GVfsBackendAfc *self;
  uint64_t fd = 0;

  fd = GPOINTER_TO_UINT(handle);
  g_return_if_fail (fd != 0);

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (sz > 0 &&
      G_UNLIKELY(g_vfs_backend_afc_check(afc_file_write (self->afc_cli,
                                                         fd, buffer, sz, &nwritten),
                                         G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_write_set_written_size (job, nwritten);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static int
g_vfs_backend_afc_seek (GVfsBackendAfc *self,
                        GVfsJob *job,
                        GVfsBackendHandle handle,
                        goffset offset,
                        GSeekType type)
{
  int afc_seek_type;
  uint64_t fd = 0;

  switch (type)
    {
    case G_SEEK_SET:
      afc_seek_type = SEEK_SET;
      break;
    case G_SEEK_CUR:
      afc_seek_type = SEEK_CUR;
      break;
    case G_SEEK_END:
      afc_seek_type = SEEK_END;
      break;
    default:
      g_vfs_job_failed(job, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("Invalid seek type"));
      return 1;
    }

  fd = GPOINTER_TO_UINT(handle);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_seek (self->afc_cli,
                                                         fd, offset, afc_seek_type),
                                          job)))
    {
      return 1;
    }

  return 0;
}

static void
g_vfs_backend_afc_seek_on_read (GVfsBackend *backend,
                                GVfsJobSeekRead *job,
                                GVfsBackendHandle handle,
                                goffset offset,
                                GSeekType type)
{
  GVfsBackendAfc *self;

  g_return_if_fail (handle != NULL);

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (!g_vfs_backend_afc_seek (self, G_VFS_JOB(job), handle, offset, type))
    {
      g_vfs_job_seek_read_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB(job));
    }
}

static void
g_vfs_backend_afc_seek_on_write (GVfsBackend *backend,
                                 GVfsJobSeekWrite *job,
                                 GVfsBackendHandle handle,
                                 goffset offset,
                                 GSeekType type)
{
  GVfsBackendAfc *self;

  g_return_if_fail (handle != NULL);

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (!g_vfs_backend_afc_seek (self, G_VFS_JOB(job), handle, offset, type))
    {
      g_vfs_job_seek_write_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB(job));
    }
}

static void
g_vfs_backend_afc_set_info_from_afcinfo (GVfsBackendAfc *self,
                                         GFileInfo *info,
                                         char **afcinfo,
                                         const char *basename,
                                         const char *path,
                                         GFileAttributeMatcher *matcher,
                                         GFileQueryInfoFlags flags)
{
  GFileType type = G_FILE_TYPE_REGULAR;
  GIcon *icon = NULL;
  gchar *content_type = NULL;
  char *display_name;
  char *linktarget = NULL;
  char **afctargetinfo = NULL;
  gboolean hidden = FALSE;
  int i;

  /* get file attributes from info list */
  for (i = 0; afcinfo[i]; i += 2)
    {
      if (afcinfo[i] == NULL)
        continue;
      if (g_str_equal (afcinfo[i], "st_size"))
        {
          g_file_info_set_size (info, atoll(afcinfo[i+1]));
        }
      else if (g_str_equal (afcinfo[i], "st_blocks"))
        {
            g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_BLOCKS, atoi(afcinfo[i+1]));
        }
      else if (g_str_equal (afcinfo[i], "st_ifmt"))
        {
          if (g_str_equal (afcinfo[i+1], "S_IFREG"))
            {
              type = G_FILE_TYPE_REGULAR;
            }
          else if (g_str_equal (afcinfo[i+1], "S_IFDIR"))
            {
              type = G_FILE_TYPE_DIRECTORY;
              content_type = g_strdup ("inode/directory");
            }
          else if (g_str_equal (afcinfo[i+1], "S_IFLNK"))
            {
              type = G_FILE_TYPE_SYMBOLIC_LINK;
              content_type = g_strdup ("inode/symlink");
            }
          else if (g_str_equal (afcinfo[i+1], "S_IFBLK"))
            {
              type = G_FILE_TYPE_SPECIAL;
              content_type = g_strdup ("inode/blockdevice");
            }
          else if (g_str_equal (afcinfo[i+1], "S_IFCHR"))
            {
              type = G_FILE_TYPE_SPECIAL;
              content_type = g_strdup ("inode/chardevice");
            }
          else if (g_str_equal (afcinfo[i+1], "S_IFIFO"))
            {
              type = G_FILE_TYPE_SPECIAL;
              content_type = g_strdup ("inode/fifo");
            }
          else if (g_str_equal (afcinfo[i+1], "S_IFSOCK"))
            {
              type = G_FILE_TYPE_SPECIAL;
              content_type = g_strdup ("inode/socket");
            }
          g_file_info_set_file_type (info, type);
        }
      else if (g_str_equal (afcinfo[i], "st_nlink"))
        {
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_NLINK, atoi(afcinfo[i+1]));
        }
      else if (g_str_equal (afcinfo[i], "st_mtime"))
        {
	  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, atoll(afcinfo[i+1]) / 1000000000);
	}
      else if (g_str_equal (afcinfo[i], "LinkTarget"))
        {
          linktarget = g_strdup (afcinfo[i+1]);
          g_file_info_set_symlink_target (info, linktarget);
          g_file_info_set_is_symlink (info, TRUE);
        }
    }

  if (content_type == NULL)
    content_type = g_content_type_guess (basename, NULL, 0, NULL);
  
  if (content_type)
    {
      g_file_info_set_content_type (info, content_type);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, content_type);
    }

  /* and set some additional info */
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, getuid ());
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, getgid ());

  /*
   * Maybe this icon stuff should be moved out into a generic function? It
   * seems a little funny to put this in the backends.
   */
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE)
      || g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_ICON))
    {
      if (type == G_FILE_TYPE_DIRECTORY)
        {
          icon = g_themed_icon_new ("folder");
        }
      else
        {
          if (content_type)
            {
              icon = g_content_type_get_icon (content_type);
              if (G_IS_THEMED_ICON(icon))
                g_themed_icon_append_name (G_THEMED_ICON(icon), "text-x-generic");
            }
        }

      if (icon == NULL)
        icon = g_themed_icon_new ("text-x-generic");

      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
    }

  g_free (content_type);

  /* for symlinks to work we need to return GFileInfo for the linktarget */
  if ((flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS) == 0)
    {
      if (type == G_FILE_TYPE_SYMBOLIC_LINK)
        {
          /* query the linktarget instead and merge the file info of it */
          if (AFC_E_SUCCESS == afc_get_file_info (self->afc_cli, linktarget, &afctargetinfo))
            g_vfs_backend_afc_set_info_from_afcinfo (self, info, afctargetinfo, linktarget, NULL, matcher, flags);
          if (afctargetinfo)
            g_strfreev (afctargetinfo);
        }
    }

  g_free (linktarget);

  /* regardless of symlink recursion; still set the basename of the source */
  g_file_info_set_name(info, basename);

  /* handle root directory */
  if (g_str_equal (basename, "/"))
    display_name = g_strdup (g_vfs_backend_get_display_name (G_VFS_BACKEND(self)));
  else
    display_name = g_filename_display_name (basename);

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME))
    g_file_info_set_display_name (info, display_name);

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME))
    g_file_info_set_edit_name (info, display_name);

  g_free (display_name);

  /* mark dot files as hidden */
  if (basename != NULL && basename[0] == '.')
     hidden = TRUE;

  g_file_info_set_is_hidden (info, hidden);

  /* Check for matching thumbnail in .MISC directory */
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_PREVIEW_ICON) &&
      path != NULL &&
      g_str_has_prefix (path, "/DCIM/") &&
      hidden == FALSE &&
      basename != NULL &&
      type == G_FILE_TYPE_REGULAR &&
      strlen (path) > 1 &&
      strlen (basename) > 4 &&
      basename[strlen(basename) - 4] == '.')
    {
      char *thumb_uri, *thumb_base, *thumb_path;
      char *parent, *ptr, *no_suffix;
      char **thumb_afcinfo;
      GFile *thumb_file;

      GMountSpec *mount_spec;
      const char *port;

      /* Parent directory */
      ptr = strrchr (path, '/');
      if (ptr == NULL)
        return;
      parent = g_strndup (path, ptr - path);

      /* Basename with suffix replaced */
      no_suffix = g_strndup (basename, strlen (basename) - 3);
      thumb_base = g_strdup_printf ("%s%s", no_suffix, "THM");
      g_free (no_suffix);

      /* Full thumbnail path */
      thumb_path = g_build_filename (parent, ".MISC", thumb_base, NULL);

      g_free (parent);
      g_free (thumb_base);

      thumb_afcinfo = NULL;
      if (afc_get_file_info (self->afc_cli, thumb_path, &thumb_afcinfo) != 0)
        {
          g_strfreev (thumb_afcinfo);
          g_free (thumb_path);
          return;
        }
      g_strfreev (thumb_afcinfo);

      /* Get the URI for the thumbnail file */
      mount_spec = g_vfs_backend_get_mount_spec (G_VFS_BACKEND (self));
      port = g_mount_spec_get (mount_spec, "port");
      thumb_uri = g_strdup_printf ("afc://%s%s%s", self->uuid, port ? port : "", thumb_path);
      thumb_file = g_file_new_for_uri (thumb_uri);
      g_free (thumb_uri);

      /* Set preview icon */
      icon = g_file_icon_new (thumb_file);
      g_object_unref (thumb_file);
      g_file_info_set_attribute_object (info,
                                        G_FILE_ATTRIBUTE_PREVIEW_ICON,
                                        G_OBJECT (icon));
      g_object_unref (icon);
    }
}

static gboolean
file_get_info (GVfsBackendAfc *backend,
               const char *path,
               GFileInfo *info)
{
  char **afcinfo = NULL;
  const char *basename, *ptr;
  gboolean result = FALSE;

  g_return_val_if_fail (backend->connected, result);
  g_return_val_if_fail (info, result);

  if (G_LIKELY(afc_get_file_info (backend->afc_cli, path, &afcinfo) == AFC_E_SUCCESS))
    {
      ptr = strrchr (path, '/');
      if (ptr && ptr[1] != '\0')
        basename = ptr + 1;
      else
        basename = path;

      g_vfs_backend_afc_set_info_from_afcinfo (backend, info, afcinfo, basename, path, NULL, 0);
      result = TRUE;
    }

  if (afcinfo)
    g_strfreev(afcinfo);

  return result;
}

/* Callback for iterating over a directory. */
static void
g_vfs_backend_afc_enumerate (GVfsBackend *backend,
                             GVfsJobEnumerate *job,
                             const char *path,
                             GFileAttributeMatcher *matcher,
                             GFileQueryInfoFlags flags)
{
  GFileInfo *info;
  GVfsBackendAfc *self;
  gboolean trailing_slash;
  gchar *file_path;
  char **ptr, **list = NULL;
  char **afcinfo = NULL;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_read_directory (self->afc_cli, path, &list),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  trailing_slash = g_str_has_suffix (path, "/");

  for (ptr = list; *ptr; ptr++)
    {
      if (g_str_equal(*ptr, ".") || g_str_equal(*ptr, ".."))
        continue;

      if (!trailing_slash)
        file_path = g_strdup_printf ("%s/%s", path, *ptr);
      else
        file_path = g_strdup_printf ("%s%s", path, *ptr);

      /*
       * This call might fail if the file in question is removed while we're
       * iterating over the directory list. In that case, just don't include
       * it in the list.
       */
      if (G_LIKELY(afc_get_file_info(self->afc_cli, file_path, &afcinfo) == AFC_E_SUCCESS))
        {
          info = g_file_info_new ();
          g_vfs_backend_afc_set_info_from_afcinfo (self, info, afcinfo, *ptr, file_path, matcher, flags);
          g_vfs_job_enumerate_add_info (job, info);
          g_object_unref (G_OBJECT(info));
          g_strfreev (afcinfo);
        }

      g_free (file_path);
    }

  g_strfreev (list);

  g_vfs_job_enumerate_done (job);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_query_info (GVfsBackend *backend,
                              GVfsJobQueryInfo *job,
                              const char *path,
                              GFileQueryInfoFlags flags,
                              GFileInfo *info,
                              GFileAttributeMatcher *matcher)
{
  GVfsBackendAfc *self;
  const char *basename, *ptr;
  char **afcinfo = NULL;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_get_file_info (self->afc_cli, path, &afcinfo),
                                          G_VFS_JOB(job))))
    {
      if (afcinfo)
        g_strfreev(afcinfo);
      return;
    }

  ptr = strrchr (path, '/');
  if (ptr && ptr[1] != '\0')
    basename = ptr + 1;
  else
    basename = path;

  g_vfs_backend_afc_set_info_from_afcinfo (self, info, afcinfo, basename, path, matcher, flags);
  if (afcinfo)
    g_strfreev (afcinfo);

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

/*
 * The following keys are currently known:
 *   Model: 'iPhone1,1'
 *   FSTotalBytes: storage capacity of drive
 *   FSFreeBytes: free space on drive
 *   FSBlockSize: block granularity
 */
static void
g_vfs_backend_afc_query_fs_info (GVfsBackend *backend,
                                 GVfsJobQueryFsInfo *job,
                                 const char *path,
                                 GFileInfo *info,
                                 GFileAttributeMatcher *matcher)
{
  GVfsBackendAfc *self;
  char **kvps, **ptr;
  uint64_t totalspace = 0, freespace = 0;
  int blocksize = 0;

  self = G_VFS_BACKEND_AFC(backend);

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "afc");

  if (self->connected)
    {
      if (G_UNLIKELY(g_vfs_backend_afc_check (afc_get_device_info (self->afc_cli, &kvps), G_VFS_JOB(job))))
        return;

      for (ptr = kvps; *ptr; ptr++)
        {
          if (g_str_equal (*ptr, "FSTotalBytes"))
            {
              totalspace = g_ascii_strtoull (*(ptr+1), (char **) NULL, 10);
            }
          else if (g_str_equal (*ptr, "FSFreeBytes"))
            {
              freespace = g_ascii_strtoull (*(ptr+1), (char **) NULL, 10);
            }
          else if (g_str_equal (*ptr, "FSBlockSize"))
            {
              blocksize = atoi (*(ptr+1));
            }
        }

      g_strfreev (kvps);

      g_file_info_set_attribute_uint32 (info,
                                        G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE,
                                        (guint32) blocksize);
      g_file_info_set_attribute_uint64 (info,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
                                        (guint64) totalspace);
      g_file_info_set_attribute_uint64 (info,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                        (guint64) freespace);
      g_file_info_set_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_READONLY,
                                         FALSE);
      g_file_info_set_attribute_uint32 (info,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW,
                                        G_FILESYSTEM_PREVIEW_TYPE_IF_LOCAL);
    }

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_set_display_name (GVfsBackend *backend,
                                    GVfsJobSetDisplayName *job,
                                    const char *filename,
                                    const char *display_name)
{
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_rename_path (self->afc_cli,
                                                           filename, display_name),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_set_display_name_set_new_path (job, display_name);

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_set_attribute (GVfsBackend *backend,
				 GVfsJobSetAttribute *job,
				 const char *filename,
				 const char *attribute,
				 GFileAttributeType type,
				 gpointer value_p,
				 GFileQueryInfoFlags flags)
{
  GVfsBackendAfc *self;
  uint64_t mtime = 0;
  afc_error_t err;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail(self->connected);

  if (g_str_equal (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED) == FALSE)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation unsupported"));
      return;
    }

  mtime = *(guint64*)(value_p) * (guint64)1000000000;

  err = afc_set_file_time (self->afc_cli, filename, mtime);
  if (err == AFC_E_UNKNOWN_PACKET_TYPE)
    {
      /* ignore error for pre-3.1 devices as the do not support setting file modification times */
      return g_vfs_job_succeeded (G_VFS_JOB(job));
    }
  if (G_UNLIKELY(g_vfs_backend_afc_check (err, G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_make_directory (GVfsBackend *backend,
                                  GVfsJobMakeDirectory *job,
                                  const char *path)
{
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail(self->connected);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_make_directory (self->afc_cli,
                                                              path),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_make_symlink (GVfsBackend *backend,
                                GVfsJobMakeSymlink *job,
                                const char *filename,
                                const char *symlink_value)
{
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_make_link (self->afc_cli,
                                                         AFC_SYMLINK, symlink_value, filename),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_move (GVfsBackend *backend,
                        GVfsJobMove *job,
                        const char *source,
                        const char *destination,
                        GFileCopyFlags flags,
                        GFileProgressCallback progress_callback,
                        gpointer progress_callback_data)
{
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail(self->connected);

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* FIXME: implement! */
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Backups are not yet supported."));
      return;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_rename_path (self->afc_cli,
                                                           source, destination),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_delete (GVfsBackend *backend,
                          GVfsJobDelete *job,
                          const char *filename)
{
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_remove_path (self->afc_cli,
                                                           filename),
                                          G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_succeeded (G_VFS_JOB(job));
}


static void
g_vfs_backend_afc_finalize (GObject *obj)
{
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(obj);
  g_vfs_backend_afc_close_connection (self);

  if (G_OBJECT_CLASS(g_vfs_backend_afc_parent_class)->finalize)
    (*G_OBJECT_CLASS(g_vfs_backend_afc_parent_class)->finalize) (obj);
}

static void
g_vfs_backend_afc_init (GVfsBackendAfc *self)
{
  if (g_getenv ("GVFS_DEBUG") != NULL)
    {
      /* enable full debugging */
      idevice_set_debug_level (1);
    }
}

static void
g_vfs_backend_afc_class_init (GVfsBackendAfcClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS(klass);

  gobject_class->finalize = g_vfs_backend_afc_finalize;

  backend_class->mount            = g_vfs_backend_afc_mount;
  backend_class->unmount          = g_vfs_backend_afc_unmount;
  backend_class->open_for_read    = g_vfs_backend_afc_open_for_read;
  backend_class->close_read       = g_vfs_backend_afc_close_read;
  backend_class->read             = g_vfs_backend_afc_read;
  backend_class->seek_on_read     = g_vfs_backend_afc_seek_on_read;
  backend_class->create           = g_vfs_backend_afc_create;
  backend_class->append_to        = g_vfs_backend_afc_append_to;
  backend_class->replace          = g_vfs_backend_afc_replace;
  backend_class->close_write      = g_vfs_backend_afc_close_write;
  backend_class->write            = g_vfs_backend_afc_write;
  backend_class->seek_on_write    = g_vfs_backend_afc_seek_on_write;
  backend_class->enumerate        = g_vfs_backend_afc_enumerate;
  backend_class->query_info       = g_vfs_backend_afc_query_info;
  backend_class->query_fs_info    = g_vfs_backend_afc_query_fs_info;
  backend_class->make_directory   = g_vfs_backend_afc_make_directory;
  backend_class->delete           = g_vfs_backend_afc_delete;
  backend_class->make_symlink     = g_vfs_backend_afc_make_symlink;
  backend_class->move             = g_vfs_backend_afc_move;
  backend_class->set_display_name = g_vfs_backend_afc_set_display_name;
  backend_class->set_attribute    = g_vfs_backend_afc_set_attribute;
}

/*
 * vim: sw=2 ts=8 cindent expandtab cinoptions=f0,>4,n2,{2,(0,^-2,t0 ai
 */
