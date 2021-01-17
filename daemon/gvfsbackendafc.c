/*
 * gvfs/daemon/gvfsbackendafc.c
 *
 * Copyright (c) 2008 Patrick Walton <pcwalton@cs.ucla.edu>
 * Copyright (c) 2010 Bastien Nocera <hadess@hadess.net>
 */

#include <config.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <glib/gi18n.h>
#include <errno.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>
#include <libimobiledevice/house_arrest.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/sbservices.h>

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

/* This needs to match with the code in the afc monitor */
typedef enum {
  VIRTUAL_PORT_AFC             = 1,
  VIRTUAL_PORT_AFC_JAILBROKEN  = 2,
  VIRTUAL_PORT_APPS            = 3
} VirtualPort;

typedef enum {
  ACCESS_MODE_UNDEFINED = 0,
  ACCESS_MODE_AFC,
  ACCESS_MODE_HOUSE_ARREST
} AccessMode;

typedef struct {
  guint64 fd;
  afc_client_t afc_cli;
  char *app;
} FileHandle;

typedef struct {
  char *display_name;
  char *id;
  char *icon_path;
  house_arrest_client_t house_arrest;
  guint num_users;
  afc_client_t afc_cli;
} AppInfo;

struct _GVfsBackendAfc {
  GVfsBackend backend;

  char *uuid;
  char *service;
  char *model;
  gboolean connected;
  AccessMode mode;

  idevice_t dev;
  afc_client_t afc_cli; /* for ACCESS_MODE_AFC */

  guint force_umount_id;

  /* for ACCESS_MODE_HOUSE_ARREST */
  GHashTable *apps; /* hash table of AppInfo */
  instproxy_client_t inst;
  sbservices_client_t sbs;
  GMutex apps_lock;
};

G_DEFINE_TYPE(GVfsBackendAfc, g_vfs_backend_afc, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_afc_close_connection (GVfsBackendAfc *self)
{
  if (self->connected)
    {
      if (self->mode == ACCESS_MODE_AFC)
        {
          afc_client_free (self->afc_cli);
        }
      else if (self->mode == ACCESS_MODE_HOUSE_ARREST)
        {
          if (self->apps != NULL)
            {
              g_hash_table_destroy (self->apps);
              self->apps = NULL;
            }
          if (self->inst)
            {
              instproxy_client_free (self->inst);
              self->inst = NULL;
            }
          if (self->sbs)
            {
              sbservices_client_free (self->sbs);
              self->sbs = NULL;
            }
          if (self->force_umount_id == 0)
            g_mutex_clear (&self->apps_lock);
        }
      else
        {
          g_assert_not_reached ();
        }
      g_free (self->model);
      self->model = NULL;
      idevice_free (self->dev);
      self->dev = NULL;
    }
  self->connected = FALSE;
}

static int
g_vfs_backend_afc_check (afc_error_t cond, GVfsJob *job)
{
  if (G_LIKELY(cond == AFC_E_SUCCESS))
        return 0;

  switch (cond)
    {
    case AFC_E_INTERNAL_ERROR:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Internal Apple File Control error"));
      break;
    case AFC_E_OBJECT_NOT_FOUND:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        _("File doesn’t exist"));
      break;
    case AFC_E_DIR_NOT_EMPTY:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY,
                        _("Directory not empty"));
      break;
    case AFC_E_OP_TIMEOUT:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        _("The device did not respond"));
      break;
    case AFC_E_NOT_ENOUGH_DATA:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_CLOSED,
                        _("The connection was interrupted"));
      break;
    case AFC_E_PERM_DENIED:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        _("Permission denied"));
      break;
    case AFC_E_MUX_ERROR:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid Apple File Control data received"));
      break;
    default:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Unhandled Apple File Control error (%d)"), cond);
      break;
    }

  return 1;
}

static int
g_vfs_backend_inst_check (instproxy_error_t cond, GVfsJob *job)
{
  if (G_LIKELY(cond == INSTPROXY_E_SUCCESS))
    {
      return 0;
    }

  g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                    _("Listing applications installed on device failed"));

  g_message ("Instproxy not available (err = %d)", cond);

  return 1;
}

static int
g_vfs_backend_sbs_check (sbservices_error_t cond, GVfsJob *job)
{
  if (G_LIKELY(cond == SBSERVICES_E_SUCCESS))
    {
      return 0;
    }

  g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                    _("Accessing application icons on device failed"));

  g_message ("SBServices not available (err = %d)", cond);

  return 1;
}

static int
g_vfs_backend_lockdownd_check (lockdownd_error_t  cond,
                               GVfsJob           *job,
                               const char        *internal_job)
{
  if (G_LIKELY(cond == LOCKDOWN_E_SUCCESS))
        return 0;

  g_debug ("Got lockdown error '%d' while doing '%s'\n",
           cond, internal_job);

  switch (cond)
    {
    case LOCKDOWN_E_INVALID_ARG:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Lockdown Error: Invalid Argument"));
      break;
    case LOCKDOWN_E_PASSWORD_PROTECTED:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        _("The device is password protected"));
      break;
    case LOCKDOWN_E_SSL_ERROR:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                        _("Unable to connect"));
      break;
    case LOCKDOWN_E_USER_DENIED_PAIRING:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                        _("User refused to trust this computer"));
      break;
    case LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                        _("The user has not trusted this computer"));
      break;
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
      break;
    default:
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Unhandled libimobiledevice error (%d)"), cond);
      break;
    }

  g_debug ("idevice_new() failed with error '%d'\n", cond);

  return 1;
}

static void
app_info_free (AppInfo *info)
{
  /* Note, those are allocated by the plist parser, so we use free(), not g_free() */
  free (info->display_name);
  free (info->id);

  g_free (info->icon_path);
  if (info->house_arrest)
    house_arrest_client_free (info->house_arrest);
  if (info->afc_cli)
    afc_client_free (info->afc_cli);

  g_free (info);
}

static gboolean
force_umount_idle (gpointer user_data)
{
  GVfsBackendAfc *afc_backend = G_VFS_BACKEND_AFC (user_data);

  g_vfs_backend_afc_close_connection (afc_backend);

  idevice_event_unsubscribe ();

  g_vfs_backend_force_unmount (G_VFS_BACKEND(afc_backend));

  afc_backend->force_umount_id = 0;
  g_object_unref (afc_backend);

  return G_SOURCE_REMOVE;
}

static void
_idevice_event_cb (const idevice_event_t *event, void *user_data)
{
  GVfsBackendAfc *afc_backend = G_VFS_BACKEND_AFC (user_data);
  const gchar *event_udid;

  g_return_if_fail (afc_backend->uuid != NULL);
  if (event->event != IDEVICE_DEVICE_REMOVE)
    return;

  event_udid = event->udid;
  if (g_str_equal (event_udid, afc_backend->uuid) == FALSE)
    return;

  g_print ("Shutting down AFC backend for device uuid %s\n", afc_backend->uuid);

  /* This might happen if the user manages to unplug/replug/unplug the same device
   * before the idle runs
   */
  if (afc_backend->force_umount_id != 0)
    {
      g_print ("AFC device with uuid %s is already being removed",
               afc_backend->uuid);
      return;
    }

  /* idevice_event_unsubscribe () will terminate the thread _idevice_event_cb
   * is running in, so we need to call back into our main loop */
  afc_backend->force_umount_id = g_idle_add (force_umount_idle,
                                             g_object_ref (afc_backend));
}

static gboolean
unpair_client (lockdownd_client_t client,
               const char        *udid)
{
  lockdownd_error_t lerr;
  gboolean ret = FALSE;

  lerr = lockdownd_unpair (client, NULL);
  if (lerr == LOCKDOWN_E_SUCCESS)
    {
      ret = TRUE;
    }

  return ret;
}

/* keep in sync with the choices array in g_vfs_backend_afc_mount() */
enum {
  CHOICE_TRY_AGAIN = 0,
  CHOICE_CANCEL
};

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
  lockdownd_service_descriptor_t lockdown_service = NULL;
  VirtualPort virtual_port;
  GMountSpec *real_spec;
  GVfsBackendAfc *self;
  int retries;
  idevice_error_t err;
  lockdownd_client_t lockdown_cli = NULL;
  lockdownd_client_t lockdown_cli_old = NULL;
  char *camera_x_content_types[] = { "x-content/audio-player", "x-content/image-dcf", NULL};
  char *media_player_x_content_types[] = {"x-content/audio-player", NULL};
  char **dcim_afcinfo;
  plist_t value;
  lockdownd_error_t lerr;
  afc_error_t aerr;
  const gchar *choices[] = {_("Try again"), _("Cancel"), NULL}; /* keep in sync with the enum above */
  gboolean aborted = FALSE;
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
                        _("Invalid AFC location: must be in the form of "
                          "afc://uuid:port-number"));
      return;
    }
  self->uuid = g_strdup (str);

  str = g_mount_spec_get (spec, "port");
  if (str == NULL)
    virtual_port = 1;
  else
    virtual_port = atoi (str);

  /* set a generic display name */
  switch (virtual_port) {
    case VIRTUAL_PORT_AFC:
      self->mode = ACCESS_MODE_AFC;
      self->service = g_strdup ("com.apple.afc");
      display_name = g_strdup_printf (_("Apple Mobile Device"));
      break;
    case VIRTUAL_PORT_AFC_JAILBROKEN:
      self->mode = ACCESS_MODE_AFC;
      self->service = g_strdup_printf ("com.apple.afc%d", virtual_port);
      display_name = g_strdup_printf (_("Apple Mobile Device, Jailbroken"));
      break;
    case VIRTUAL_PORT_APPS:
      self->mode = ACCESS_MODE_HOUSE_ARREST;
      self->service = g_strdup ("com.apple.mobile.house_arrest");
      display_name = g_strdup_printf (_("Documents on Apple Mobile Device"));
      break;
    default:
      g_vfs_job_failed (G_VFS_JOB(job), G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Invalid AFC location: must be in the form of "
                          "afc://uuid:port-number"));
      g_debug ("Failed to mount, the AFC location was in the wrong format");
      return;
  }

  g_vfs_backend_set_display_name (G_VFS_BACKEND(self), display_name);
  g_free (display_name);
  display_name = NULL;

  real_spec = g_mount_spec_new ("afc");
  g_mount_spec_set (real_spec, "host", self->uuid);

  /* INFO: Don't ever set the DefaultPort again or everything goes crazy */
  if (virtual_port != VIRTUAL_PORT_AFC)
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
  lerr = lockdownd_client_new (self->dev, &lockdown_cli, "gvfsd-afc");
  if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lerr, G_VFS_JOB(job), "new client, no handshake")))
    goto out_destroy_dev;

  /* try to use pretty device name */
  if (LOCKDOWN_E_SUCCESS == lockdownd_get_device_name (lockdown_cli, &display_name))
    {
      if (display_name)
        {
          switch (virtual_port) {
            case VIRTUAL_PORT_AFC:
              g_vfs_backend_set_display_name (G_VFS_BACKEND(self), display_name);
              break;
            case VIRTUAL_PORT_AFC_JAILBROKEN:
              g_vfs_backend_set_display_name (G_VFS_BACKEND(self),
              /* translators:
               * This is the device name, with the service being browsed in brackets, eg.:
               * Alan Smithee's iPhone (jailbreak) */
                                              g_strdup_printf (_("%s (jailbreak)"), display_name));
              break;
            case VIRTUAL_PORT_APPS:
              g_vfs_backend_set_display_name (G_VFS_BACKEND(self),
              /* translators:
               * This is "Documents on foo" where foo is the device name, eg.:
               * Documents on Alan Smithee's iPhone */
                                              g_strdup_printf (_("Documents on %s"), display_name));
              break;
            default:
              g_assert_not_reached ();
          }
        }
    }

  /* set correct freedesktop icon spec name depending on device model */
  value = NULL;
  lerr = lockdownd_get_value (lockdown_cli, NULL, "DeviceClass", &value);
  if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lerr, G_VFS_JOB(job), "getting device class")))
    goto out_destroy_lockdown;

  plist_get_string_val (value, &self->model);
  if ((self->model != NULL) && (g_str_equal (self->model, "iPod") != FALSE))
    {
      g_vfs_backend_set_icon_name (G_VFS_BACKEND(self), "multimedia-player-apple-ipod-touch");
      g_vfs_backend_set_symbolic_icon_name (G_VFS_BACKEND(self), "multimedia-player-apple-ipod-touch-symbolic");
    }
  else if ((self->model != NULL) && (g_str_equal (self->model, "iPad") != FALSE))
    {
      g_vfs_backend_set_icon_name (G_VFS_BACKEND(self), "computer-apple-ipad");
      g_vfs_backend_set_symbolic_icon_name (G_VFS_BACKEND(self), "computer-apple-ipad-symbolic");
    }
  else
    {
      g_vfs_backend_set_icon_name (G_VFS_BACKEND(self), "phone-apple-iphone");
      g_vfs_backend_set_symbolic_icon_name (G_VFS_BACKEND(self), "phone-apple-iphone-symbolic");
    }

  /* save the old client until we connect with the handshake */
  lockdown_cli_old = lockdown_cli;
  lockdown_cli = NULL;

  /* now, try to connect with handshake */
  retries = 0;
  do {
    char *message;

    g_debug ("Lockdown client try #%d\n", retries);
    lerr = lockdownd_client_new_with_handshake (self->dev,
                                                &lockdown_cli,
                                                "gvfsd-afc");
    if (lerr == LOCKDOWN_E_SSL_ERROR)
      {
        unpair_client (lockdown_cli_old, self->uuid);
        continue;
      }

    if (lerr == LOCKDOWN_E_USER_DENIED_PAIRING)
      {
        aborted = TRUE;
        break;
      }

    /* An unknown error? Let's try again without prompting */
    if (lerr == LOCKDOWN_E_UNKNOWN_ERROR)
      {
        g_debug ("Got an unknown lockdown error, retrying after a short sleep\n");
        g_usleep (G_USEC_PER_SEC);
        continue;
      }

    if (lerr != LOCKDOWN_E_PASSWORD_PROTECTED &&
        lerr != LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING)
      break;

    aborted = FALSE;
    if (lerr == LOCKDOWN_E_PASSWORD_PROTECTED)
      {
        /* translators:
         * %s is the device name. 'Try again' is the caption of the button
         * shown in the dialog which is defined above. */
        message = g_strdup_printf (_("Device Locked\nThe device “%s” is locked.\n\nEnter the passcode on the device and click “Try again”."), display_name);
      }
    else if (lerr == LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING)
      {
        /* translators:
         * %s is the device name. 'Try again' is the caption of the button
         * shown in the dialog which is defined above. 'Trust' is the caption
         * of the button shown in the device. */
        message = g_strdup_printf (_("Untrusted Device\nThe device “%s” is not trusted yet.\n\nSelect “Trust” on the device and click “Try again”."), display_name);
      }
    else
      g_assert_not_reached ();

    ret = g_mount_source_ask_question (src,
                                       message,
                                       choices,
                                       &aborted,
                                       &choice);
    g_free (message);

    if (!ret || aborted || (choice == CHOICE_CANCEL))
      {
        if (!ret)
          g_debug ("g_mount_source_ask_question() failed\n");
        else if (aborted)
          g_debug ("g_mount_source_ask_question() aborted\n");
        else
          g_debug ("g_mount_source_ask_question() choice was 'cancel'\n");
        break;
      }
  } while (retries++ < 10);

  /* Now we're done with the old client */
  lockdownd_client_free (lockdown_cli_old);
  lockdown_cli_old = NULL;

  g_free (display_name);
  display_name = NULL;

  if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lerr, G_VFS_JOB(job), "initial paired client")))
    goto out_destroy_dev;

  switch (self->mode) {
    case ACCESS_MODE_AFC:
      lerr = lockdownd_start_service (lockdown_cli, self->service, &lockdown_service);
      if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lerr, G_VFS_JOB(job), "starting lockdownd")))
        {
          goto out_destroy_lockdown;
        }
      aerr = afc_client_new (self->dev, lockdown_service, &self->afc_cli);
      if (G_UNLIKELY(g_vfs_backend_afc_check (aerr, G_VFS_JOB(job))))
        {
          goto out_destroy_lockdown;
        }
      break;
    case ACCESS_MODE_HOUSE_ARREST:
      lerr = lockdownd_start_service (lockdown_cli, "com.apple.mobile.installation_proxy", &lockdown_service);
      if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lerr, G_VFS_JOB(job), "starting install proxy")))
        {
          g_warning ("couldn't start inst proxy");
          goto out_destroy_lockdown;
        }
      aerr = instproxy_client_new (self->dev, lockdown_service, &self->inst);
      if (G_UNLIKELY(g_vfs_backend_inst_check (aerr, G_VFS_JOB(job))))
        {
          g_warning ("couldn't create inst proxy instance");
          goto out_destroy_lockdown;
        }
      lerr = lockdownd_start_service (lockdown_cli, "com.apple.springboardservices", &lockdown_service);
      if (G_UNLIKELY(g_vfs_backend_lockdownd_check (lerr, G_VFS_JOB(job), "starting install services")))
        {
          g_warning ("couldn't start SBServices proxy");
          goto out_destroy_lockdown;
        }
      aerr = sbservices_client_new (self->dev, lockdown_service, &self->sbs);
      if (G_UNLIKELY(g_vfs_backend_sbs_check (aerr, G_VFS_JOB(job))))
        {
          g_warning ("couldn't create SBServices proxy instance");
          goto out_destroy_lockdown;
        }
      /* Create directory for the icon cache */
        {
          char *path;

          path = g_build_filename (g_get_user_cache_dir (),
                                   "libimobiledevice",
                                   "icons", NULL);
          g_mkdir_with_parents (path, 0755);
          g_free (path);
        }
      break;
    default:
      g_assert_not_reached ();
  }

  /* lockdown connection is not needed anymore */
  lockdownd_client_free (lockdown_cli);
  lockdownd_service_descriptor_free (lockdown_service);

  /* Add camera item if necessary */
  if (self->mode == ACCESS_MODE_AFC)
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
  lockdownd_service_descriptor_free (lockdown_service);

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

  idevice_event_unsubscribe ();

  /* FIXME: check on G_MOUNT_UNMOUNT_FORCE flag */
  self = G_VFS_BACKEND_AFC (backend);
  g_vfs_backend_afc_close_connection (self);

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static gboolean file_get_info (GVfsBackendAfc *backend, afc_client_t afc_cli, const char *path, GFileInfo *info);

static gboolean
is_directory (GVfsBackendAfc *backend,
              afc_client_t afc_cli,
              const char *path)
{
  gboolean result = FALSE;
  GFileInfo *info;

  info = g_file_info_new();
  if (file_get_info (backend, afc_cli, path, info))
    {
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        result = TRUE;
    }

  g_object_unref (info);
  return result;
}

static gboolean
is_regular (GVfsBackendAfc *backend,
            afc_client_t afc_cli,
            const char *path)
{
  gboolean result = FALSE;
  GFileInfo *info;

  info = g_file_info_new();
  if (file_get_info (backend, afc_cli, path, info))
    {
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR)
	result = TRUE;
    }

  g_object_unref (info);
  return result;
}

static gboolean
g_vfs_backend_setup_afc_for_app (GVfsBackendAfc *self,
                                 gboolean        last_try,
                                 const char     *id)
{
  AppInfo *info;
  lockdownd_client_t lockdown_cli;
  lockdownd_service_descriptor_t lockdown_service = NULL;
  house_arrest_client_t house_arrest;
  afc_client_t afc;
  plist_t dict, error;
  lockdownd_error_t lerr;
  gboolean retry = FALSE;

  g_mutex_lock (&self->apps_lock);

  info = g_hash_table_lookup (self->apps, id);

  if (info == NULL ||
      info->afc_cli != NULL)
    goto out;

  /* Load house arrest and afc now! */
  lockdown_cli = NULL;
  if (lockdownd_client_new_with_handshake (self->dev, &lockdown_cli, "gvfsd-afc") != LOCKDOWN_E_SUCCESS)
    {
      g_warning ("Failed to get a lockdown to start house arrest for app %s", info->id);
      goto out;
    }

  lerr = lockdownd_start_service (lockdown_cli, "com.apple.mobile.house_arrest", &lockdown_service);
  if (lerr != LOCKDOWN_E_SUCCESS)
    {
      if (lerr == LOCKDOWN_E_SERVICE_LIMIT && !last_try)
        {
          retry = TRUE;
          g_debug ("Failed to start house arrest for app %s (%d)\n", info->id, lerr);
        }
      else
        g_warning ("Failed to start house arrest for app %s (%d)", info->id, lerr);
      lockdownd_client_free (lockdown_cli);
      goto out;
    }

  house_arrest = NULL;
  house_arrest_client_new (self->dev, lockdown_service, &house_arrest);
  if (house_arrest == NULL)
    {
      g_warning ("Failed to start house arrest client for app %s", info->id);
      lockdownd_client_free (lockdown_cli);
      lockdownd_service_descriptor_free (lockdown_service);
      goto out;
    }

  lockdownd_service_descriptor_free (lockdown_service);

  dict = NULL;
  if (house_arrest_send_command (house_arrest, "VendDocuments", info->id) != HOUSE_ARREST_E_SUCCESS ||
      house_arrest_get_result (house_arrest, &dict) != HOUSE_ARREST_E_SUCCESS)
    {
      g_warning ("Failed to set up house arrest for app %s", info->id);
      house_arrest_client_free (house_arrest);
      lockdownd_client_free (lockdown_cli);
      goto out;
    }
  error = plist_dict_get_item (dict, "Error");
  if (error != NULL)
    {
      char *str;

      plist_get_string_val (error, &str);
      g_warning ("Failed to set up house arrest for app %s: %s", info->id, str);
      free (str);
      plist_free (dict);

      house_arrest_client_free (house_arrest);
      lockdownd_client_free (lockdown_cli);
      goto out;
    }
  plist_free (dict);

  lockdownd_client_free (lockdown_cli);

  afc = NULL;
  afc_client_new_from_house_arrest_client(house_arrest, &afc);
  if (afc == NULL)
    {
      g_warning ("Failed to set up afc client for app %s", info->id);
      house_arrest_client_free (house_arrest);
      goto out;
    }

  info->house_arrest = house_arrest;
  info->afc_cli = afc;

out:
  g_mutex_unlock (&self->apps_lock);
  return !retry;
}

static FileHandle *
g_vfs_backend_file_handle_new (GVfsBackendAfc *self,
                               const char     *app)
{
  AppInfo *info;
  FileHandle *handle;

  handle = g_new0 (FileHandle, 1);

  if (app == NULL)
    return handle;

  g_mutex_lock (&self->apps_lock);
  info = g_hash_table_lookup (self->apps, app);
  info->num_users++;
  g_mutex_unlock (&self->apps_lock);

  handle->app = g_strdup (app);

  return handle;
}

static void
g_vfs_backend_file_handle_free (GVfsBackendAfc *self,
                                FileHandle     *fh)
{
  AppInfo *info;

  if (fh == NULL)
    return;

  if (fh->app == NULL)
    goto out;

  g_mutex_lock (&self->apps_lock);
  info = g_hash_table_lookup (self->apps, fh->app);
  g_assert (info->num_users != 0);
  info->num_users--;
  g_mutex_unlock (&self->apps_lock);

out:
  if (self->connected)
    afc_file_close (fh->afc_cli, fh->fd);
  g_free (fh->app);
  g_free (fh);
}

/* If we succeeded in removing access to at least one
 * HouseArrest service, return TRUE */
static gboolean
g_vfs_backend_gc_house_arrest (GVfsBackendAfc *self,
                               const char     *app)
{
  GList *apps, *l;
  gboolean ret = FALSE;

  g_mutex_lock (&self->apps_lock);

  apps = g_hash_table_get_values (self->apps);
  /* XXX: We might want to sort the apps so the
   * oldest used gets cleaned up first */

  for (l = apps; l != NULL; l = l->next)
    {
      AppInfo *info = l->data;

      /* Don't close the same app we're trying to
       * connect to the service, but return as it's
       * already setup */
      if (g_strcmp0 (info->id, app) == 0)
        {
          g_debug ("A HouseArrest service for '%s' is already setup\n", app);
          ret = TRUE;
          break;
        }

      if (info->afc_cli == NULL ||
          info->num_users > 0)
        continue;

      g_clear_pointer (&info->afc_cli, afc_client_free);
      g_clear_pointer (&info->house_arrest, house_arrest_client_free);

      g_debug ("Managed to free HouseArrest service from '%s', for '%s'\n",
               info->id, app);
      ret = TRUE;
      break;
    }

  g_mutex_unlock (&self->apps_lock);

  return ret;
}

/* If force_afc_mount is TRUE, then we'll try to mount
 * the app if there's one in the path, otherwise, we'll hold on */
static char *
g_vfs_backend_parse_house_arrest_path (GVfsBackendAfc *self,
                                       gboolean        force_afc_mount,
                                       const char     *path,
                                       gboolean       *is_doc_root_ret,
                                       char          **new_path)
{
  char **comps;
  char *app;
  gboolean setup_afc, is_doc_root;

  if (path == NULL || *path == '\0' || g_str_equal (path, "/"))
    {
      *new_path = NULL;
      return NULL;
    }

  if (*path != '/')
    comps = g_strsplit (path, "/", -1);
  else
    comps = g_strsplit (path + 1, "/", -1);

  setup_afc = force_afc_mount;
  app = comps[0];
  is_doc_root = (comps[0] != NULL && comps[1] == NULL);
  if (is_doc_root_ret)
    *is_doc_root_ret = is_doc_root;

  /* Replace the app path with "Documents" so the gvfs
   * path of afc://<uuid>/org.gnome.test/foo.txt should
   * correspond to Documents/foo.txt in the app's container */
  comps[0] = g_strdup ("Documents");
  *new_path = g_strjoinv ("/", comps);
  if (is_doc_root)
    setup_afc = TRUE;
  g_strfreev (comps);

  if (app != NULL &&
      setup_afc)
    {
      if (!g_vfs_backend_setup_afc_for_app (self, FALSE, app))
        {
          g_debug ("Ran out of HouseArrest clients for app '%s', trying again\n", app);
          g_vfs_backend_gc_house_arrest (self, app);
          g_vfs_backend_setup_afc_for_app (self, TRUE, app);
        }
    }

  return app;
}

/* Callback to open an existing file for reading. */
static void
g_vfs_backend_afc_open_for_read (GVfsBackend *backend,
                                 GVfsJobOpenForRead *job,
                                 const char *path)
{
  uint64_t fd = 0;
  GVfsBackendAfc *self;
  char *new_path;
  afc_client_t afc_cli;
  FileHandle *handle;
  char *app = NULL;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      AppInfo *info;
      gboolean is_doc_root;

      new_path = NULL;
      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, path, &is_doc_root, &new_path);
      if (app == NULL || is_doc_root)
        goto is_dir_bail;

      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        goto not_found_bail;

      afc_cli = info->afc_cli;
    }
  else
    {
      afc_cli = self->afc_cli;
      new_path = NULL;
    }

  if (is_directory (self, afc_cli, new_path ? new_path : path))
    {
is_dir_bail:
      g_free (new_path);
      g_free (app);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_IS_DIRECTORY,
                        _("Can’t open directory"));
      return;
    }

  if (!is_regular (self, afc_cli, new_path ? new_path : path))
    {
not_found_bail:
      g_free (new_path);
      g_free (app);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("File doesn’t exist"));
      return;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_open (afc_cli,
                                                         new_path ? new_path : path, AFC_FOPEN_RDONLY, &fd),
                                          G_VFS_JOB(job))))
    {
      g_free (new_path);
      g_free (app);
      return;
    }

  g_free (new_path);

  handle = g_vfs_backend_file_handle_new (self, app);
  handle->fd = fd;
  handle->afc_cli = afc_cli;

  g_free (app);

  g_vfs_job_open_for_read_set_handle (job, handle);
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
  char *new_path, *app = NULL;
  afc_client_t afc_cli;
  FileHandle *fh;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  new_path = NULL;

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      AppInfo *info;

      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, path, NULL, &new_path);
      if (app == NULL)
        {
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB(job));
          return;
        }
      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        {
          g_free (new_path);
          g_free (app);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB(job));
          return;
        }
      afc_cli = info->afc_cli;
    }
  else
    {
      afc_cli = self->afc_cli;
      new_path = NULL;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_open (afc_cli,
                                                         new_path ? new_path : path, AFC_FOPEN_RW, &fd),
                                          G_VFS_JOB(job))))
    {
      g_free (new_path);
      g_free (app);
      return;
    }

  g_free (new_path);

  fh = g_vfs_backend_file_handle_new (self, app);
  fh->fd = fd;
  fh->afc_cli = afc_cli;

  g_free (app);

  g_vfs_job_open_for_write_set_handle (job, fh);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (job, TRUE);
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
  char *new_path, *app = NULL;
  afc_client_t afc_cli;
  FileHandle *fh;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  new_path = NULL;

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      AppInfo *info;

      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, path, NULL, &new_path);
      if (app == NULL)
        {
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB(job));
          return;
        }
      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        {
          g_free (new_path);
          g_free (app);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB(job));
          return;
        }
      afc_cli = info->afc_cli;
    }
  else
    {
      afc_cli = self->afc_cli;
      new_path = NULL;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_open (afc_cli,
                                                         new_path ? new_path : path, AFC_FOPEN_RW, &fd),
                                          G_VFS_JOB(job))))
    {
      g_free (new_path);
      g_free (app);
      return;
    }

  g_free (new_path);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_seek (afc_cli,
                                                         fd, 0, SEEK_END),
                                          G_VFS_JOB(job))))
    {
      afc_file_close (afc_cli, fd);
      g_free (app);
      return;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_tell (afc_cli,
                                                         fd, &off),
                                          G_VFS_JOB(job))))
    {
      afc_file_close (afc_cli, fd);
      g_free (app);
      return;
    }

  fh = g_vfs_backend_file_handle_new (self, app);
  fh->fd = fd;
  fh->afc_cli = afc_cli;

  g_free (app);

  g_vfs_job_open_for_write_set_handle (job, fh);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (job, TRUE);
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
  char *new_path, *app = NULL;
  afc_client_t afc_cli;
  FileHandle *fh;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail(self->connected);

  if (make_backup)
    {
      /* FIXME: implement! */
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Backups not supported"));
      return;
    }

  new_path = NULL;

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      AppInfo *info;

      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, filename, NULL, &new_path);
      if (app == NULL)
        {
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB(job));
          return;
        }
      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        {
          g_free (new_path);
          g_free (app);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB(job));
          return;
        }
      afc_cli = info->afc_cli;
    }
  else
    {
      afc_cli = self->afc_cli;
      new_path = NULL;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_open (afc_cli,
                                                         new_path ? new_path : filename, AFC_FOPEN_WR, &fd),
                                          G_VFS_JOB(job))))
    {
      g_free (new_path);
      g_free (app);
      return;
    }

  g_free (new_path);

  fh = g_vfs_backend_file_handle_new (self, app);
  fh->fd = fd;
  fh->afc_cli = afc_cli;

  g_free (app);

  g_vfs_job_open_for_write_set_handle (job, fh);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (job, TRUE);
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
  FileHandle *fh;

  fh = (FileHandle *) handle;

  self = G_VFS_BACKEND_AFC(backend);

  g_vfs_backend_file_handle_free (self, fh);

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_close_write (GVfsBackend *backend,
                               GVfsJobCloseWrite *job,
                               GVfsBackendHandle handle)
{
  GVfsBackendAfc *self;
  FileHandle *fh;

  fh = (FileHandle *) handle;

  self = G_VFS_BACKEND_AFC(backend);

  g_vfs_backend_file_handle_free (self, fh);

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
  FileHandle *fh;

  fh = (FileHandle *) handle;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (req > 0 &&
      G_UNLIKELY(g_vfs_backend_afc_check (afc_file_read (fh->afc_cli,
                                                         fh->fd, buffer, req, &nread),
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
  FileHandle *fh;

  fh = (FileHandle *) handle;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (sz > 0 &&
      G_UNLIKELY(g_vfs_backend_afc_check(afc_file_write (fh->afc_cli,
                                                         fh->fd, buffer, sz, &nwritten),
                                         G_VFS_JOB(job))))
    {
      return;
    }

  g_vfs_job_write_set_written_size (job, nwritten);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static goffset
g_vfs_backend_afc_seek (GVfsBackendAfc *self,
                        GVfsJob *job,
                        GVfsBackendHandle handle,
                        goffset offset,
                        GSeekType type)
{
  int afc_seek_type;
  FileHandle *fh;
  guint64 new_offset;

  if ((afc_seek_type = gvfs_seek_type_to_lseek (type)) == -1)
    {
      g_vfs_job_failed(job, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("Unsupported seek type"));
      return -1;
    }

  fh = (FileHandle *) handle;

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_seek (fh->afc_cli,
                                                         fh->fd, offset, afc_seek_type),
                                          job)))
      return -1;

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_file_tell (fh->afc_cli, fh->fd, &new_offset),
                                          job)))
      return -1;

  return new_offset;
}

static void
g_vfs_backend_afc_seek_on_read (GVfsBackend *backend,
                                GVfsJobSeekRead *job,
                                GVfsBackendHandle handle,
                                goffset offset,
                                GSeekType type)
{
  GVfsBackendAfc *self;
  goffset new_offset;

  g_return_if_fail (handle != NULL);

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  new_offset = g_vfs_backend_afc_seek (self, G_VFS_JOB(job), handle, offset, type);
  if (new_offset >= 0)
    {
      g_vfs_job_seek_read_set_offset (job, new_offset);
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
  goffset new_offset;

  g_return_if_fail (handle != NULL);

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  new_offset = g_vfs_backend_afc_seek (self, G_VFS_JOB(job), handle, offset, type);
  if (new_offset >= 0)
    {
      g_vfs_job_seek_write_set_offset (job, new_offset);
      g_vfs_job_succeeded (G_VFS_JOB(job));
    }
}

static void
g_vfs_backend_afc_truncate (GVfsBackend *backend,
                            GVfsJobTruncate *job,
                            GVfsBackendHandle handle,
                            goffset size)
{
  GVfsBackendAfc *self;
  FileHandle *fh;

  g_return_if_fail (handle != NULL);

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  fh = (FileHandle *) handle;

  if (!g_vfs_backend_afc_check (afc_file_truncate (fh->afc_cli, fh->fd, size),
                                G_VFS_JOB(job)))
    g_vfs_job_succeeded (G_VFS_JOB(job));
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
  GIcon *symbolic_icon = NULL;
  gchar *content_type = NULL;
  gboolean uncertain_content_type = FALSE;
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
          int blocks = atoi(afcinfo[i+1]);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_BLOCKS, blocks);
          g_file_info_set_attribute_uint64 (info,
                                            G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE,
                                            blocks * G_GUINT64_CONSTANT (512));
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
      else if (g_str_equal (afcinfo[i], "st_birthtime"))
        {
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED, atoll(afcinfo[i+1]) / 1000000000);
        }
      else if (g_str_equal (afcinfo[i], "LinkTarget"))
        {
          linktarget = g_strdup (afcinfo[i+1]);
          g_file_info_set_symlink_target (info, linktarget);
          g_file_info_set_is_symlink (info, TRUE);
        }
    }

  if (content_type == NULL)
    content_type = g_content_type_guess (basename, NULL, 0, &uncertain_content_type);
  
  if (content_type)
    {
      if (!uncertain_content_type)
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
      || g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_ICON)
      || g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON))
    {
      if (type == G_FILE_TYPE_DIRECTORY)
        {
          icon = g_content_type_get_icon (content_type);
          symbolic_icon = g_content_type_get_symbolic_icon (content_type);
        }
      else
        {
          if (content_type)
            {
              icon = g_content_type_get_icon (content_type);
              if (G_IS_THEMED_ICON(icon))
                {
                  g_themed_icon_append_name (G_THEMED_ICON(icon), "text-x-generic");
                }

              symbolic_icon = g_content_type_get_symbolic_icon (content_type);
              if (G_IS_THEMED_ICON(symbolic_icon))
                {
                  g_themed_icon_append_name (G_THEMED_ICON(symbolic_icon), "text-x-generic-symbolic");
                }
            }
        }

      if (icon == NULL)
        icon = g_themed_icon_new ("text-x-generic");
      if (symbolic_icon == NULL)
        symbolic_icon = g_themed_icon_new ("text-x-generic-symbolic");

      g_file_info_set_icon (info, icon);
      g_file_info_set_symbolic_icon (info, symbolic_icon);
      g_object_unref (icon);
      g_object_unref (symbolic_icon);
    }

  g_free (content_type);

  /* for symlinks to work we need to return GFileInfo for the linktarget */
  if ((flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS) == 0 &&
      self->mode == ACCESS_MODE_AFC)
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

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
}

static void
g_vfs_backend_afc_set_info_from_app (GVfsBackendAfc *self,
                                     GFileInfo *info,
                                     AppInfo *app_info)
{
  GIcon *icon;
  GIcon *symbolic_icon;
  const gchar *content_type = "inode/directory";

  /* content-type */
  g_file_info_set_content_type (info, content_type);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, content_type);
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);

  /* icon */
  if (app_info == NULL || app_info->icon_path == NULL)
    {
      icon = g_content_type_get_icon (content_type);
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
    }
  else
    {
      GFile *file;

      file = g_file_new_for_path (app_info->icon_path);
      icon = g_file_icon_new (file);
      g_object_unref (file);
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
    }

  /* symbolic icon */
  symbolic_icon = g_content_type_get_symbolic_icon (content_type);
  g_file_info_set_symbolic_icon (info, symbolic_icon);
  g_object_unref (symbolic_icon);

  /* name */
  if (app_info != NULL)
    {
      g_file_info_set_name(info, app_info->id);
      g_file_info_set_display_name (info, app_info->display_name);
    }
  else
    {
      g_file_info_set_name(info, "/");
      g_file_info_set_display_name (info, g_vfs_backend_get_display_name (G_VFS_BACKEND(self)));
    }

  /* owner */
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, getuid ());
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, getgid ());
}

static gboolean
file_get_info (GVfsBackendAfc *backend,
               afc_client_t afc_cli,
               const char *path,
               GFileInfo *info)
{
  char **afcinfo = NULL;
  const char *basename, *ptr;
  gboolean result = FALSE;

  g_return_val_if_fail (backend->connected, result);
  g_return_val_if_fail (info, result);

  if (G_LIKELY(afc_get_file_info (afc_cli, path, &afcinfo) == AFC_E_SUCCESS))
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

static char *
g_vfs_backend_load_icon (sbservices_client_t sbs,
                         const char         *id)
{
  char *path;
  char *filename;
  char *data;
  guint64 len;

  filename = g_strdup_printf ("%s.png", id);
  path = g_build_filename (g_get_user_cache_dir (),
                           "libimobiledevice",
                           "icons",
                           filename, NULL);
  g_free (filename);

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return path;

  data = NULL;
  len = 0;
  if (sbservices_get_icon_pngdata (sbs, id, &data, &len) != SBSERVICES_E_SUCCESS ||
      data == NULL || len == 0)
    {
      if (data != NULL)
        free (data);
      g_free (path);
      return NULL;
    }

  if (g_file_set_contents (path, data, len, NULL) == FALSE)
    {
      free (data);
      g_free (path);
      return NULL;
    }
  free (data);

  return path;
}


/* apps_lock needs to be locked before calling this */
static gboolean
g_vfs_backend_load_apps (GVfsBackendAfc *self)
{
  plist_t client_opts;
  guint num_apps, i;
  plist_t apps = NULL;
  instproxy_error_t err;

  g_assert (self->mode == ACCESS_MODE_HOUSE_ARREST);

  if (self->apps != NULL)
    {
      return TRUE;
    }

  client_opts = instproxy_client_options_new ();
  instproxy_client_options_add (client_opts, "ApplicationType", "User", NULL);

  err = instproxy_browse (self->inst, client_opts, &apps);
  instproxy_client_options_free (client_opts);

  if (err != INSTPROXY_E_SUCCESS)
    {
      return FALSE;
    }

  self->apps = g_hash_table_new_full (g_str_hash, g_str_equal,
                                      (GDestroyNotify) g_free,
                                      (GDestroyNotify) app_info_free);

  num_apps = plist_array_get_size(apps);
  for (i = 0; i < num_apps; i++)
    {
      plist_t app;
      plist_t p_appid;
      plist_t p_name;
      plist_t p_sharing;
      char *s_appid;
      char *s_name;
      guint8 b_sharing;
      AppInfo *info;

      app = plist_array_get_item(apps, i);
      p_appid = plist_dict_get_item (app, "CFBundleIdentifier");
      p_name = plist_dict_get_item (app, "CFBundleDisplayName");
      p_sharing = plist_dict_get_item (app, "UIFileSharingEnabled");
      b_sharing = FALSE;
      if (p_sharing)
        {
          if (plist_get_node_type (p_sharing) == PLIST_BOOLEAN)
            {
              plist_get_bool_val (p_sharing, &b_sharing);
            }
          else if (plist_get_node_type (p_sharing) == PLIST_STRING)
            {
              char *p_sharing_val = NULL;

              plist_get_string_val (p_sharing, &p_sharing_val);
              if (p_sharing_val)
                {
                  if ((g_ascii_strcasecmp (p_sharing_val, "YES") == 0) || (g_ascii_strcasecmp (p_sharing_val, "true") == 0))
                    b_sharing = TRUE;
                  free (p_sharing_val);
                }
            }
        }

      /* Doesn't support documents, or missing metadata? */
      if (!b_sharing || p_appid == NULL || p_name == NULL)
        {
          continue;
        }

      s_appid = s_name = NULL;
      plist_get_string_val (p_appid, &s_appid);
      if (s_appid == NULL)
        {
          continue;
        }
      plist_get_string_val (p_name, &s_name);
      if (s_name == NULL)
        {
          free (s_appid);
          continue;
        }

      info = g_new0 (AppInfo, 1);
      info->display_name = s_name;
      info->id = s_appid;

      info->icon_path = g_vfs_backend_load_icon (self->sbs, info->id);

      g_hash_table_insert (self->apps, g_strdup (s_appid), info);
    }

  plist_free (apps);

  return TRUE;
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
  char *new_path = NULL;
  afc_client_t afc_cli;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (self->mode == ACCESS_MODE_AFC)
    {
      afc_cli = self->afc_cli;
      if (G_UNLIKELY(g_vfs_backend_afc_check (afc_read_directory (self->afc_cli, path, &list),
                                              G_VFS_JOB(job))))
        {
          return;
        }
    }
  else if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      char *app;

      g_mutex_lock (&self->apps_lock);
      if (g_vfs_backend_load_apps (self) == FALSE)
        {
          g_vfs_backend_afc_check (AFC_E_INTERNAL_ERROR, G_VFS_JOB (job));
          g_mutex_unlock (&self->apps_lock);
          return;
        }
      g_mutex_unlock (&self->apps_lock);

      app = g_vfs_backend_parse_house_arrest_path (self, TRUE, path, NULL, &new_path);

      if (app == NULL)
        {
          GList *apps, *l;

          apps = g_hash_table_get_values (self->apps);
          for (l = apps; l != NULL; l = l->next)
            {
              AppInfo *app_info = l->data;
              info = g_file_info_new ();
              g_vfs_backend_afc_set_info_from_app (self, info, app_info);
              g_vfs_job_enumerate_add_info (job, info);
              g_object_unref (G_OBJECT(info));
            }
          g_list_free (apps);
          g_vfs_job_enumerate_done (job);
          g_vfs_job_succeeded (G_VFS_JOB(job));
          return;
        }
      else
        {
          AppInfo *app_info;

          app_info = g_hash_table_lookup (self->apps, app);
          if (app_info == NULL)
            {
              g_free (app);
              g_free (new_path);
              g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB(job));
              return;
            }
          g_free (app);
          afc_cli = app_info->afc_cli;
          if (G_UNLIKELY(g_vfs_backend_afc_check (afc_read_directory (afc_cli, new_path, &list),
                                                  G_VFS_JOB(job))))
            {
              g_free (new_path);
              return;
            }
        }
    }
  else
    {
      g_assert_not_reached ();
    }

  trailing_slash = g_str_has_suffix (new_path ? new_path : path, "/");

  for (ptr = list; *ptr; ptr++)
    {
      if (g_str_equal(*ptr, ".") || g_str_equal(*ptr, ".."))
        continue;

      if (!trailing_slash)
        file_path = g_strdup_printf ("%s/%s", new_path ? new_path : path, *ptr);
      else
        file_path = g_strdup_printf ("%s%s", new_path ? new_path : path, *ptr);

      /*
       * This call might fail if the file in question is removed while we're
       * iterating over the directory list. In that case, just don't include
       * it in the list.
       */
      if (G_LIKELY(afc_get_file_info(afc_cli, file_path, &afcinfo) == AFC_E_SUCCESS))
        {
          info = g_file_info_new ();
          g_vfs_backend_afc_set_info_from_afcinfo (self, info, afcinfo, *ptr, file_path, matcher, flags);

          g_vfs_job_enumerate_add_info (job, info);
          g_object_unref (G_OBJECT(info));
          g_strfreev (afcinfo);
        }

      g_free (file_path);
    }

  g_free (new_path);
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
  char *new_path;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);
  new_path = NULL;

  if (self->mode == ACCESS_MODE_AFC)
    {
      if (G_UNLIKELY(g_vfs_backend_afc_check (afc_get_file_info (self->afc_cli, path, &afcinfo),
                                              G_VFS_JOB(job))))
        {
          if (afcinfo)
                g_strfreev(afcinfo);
          return;
        }
    }
  else if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      char *app;
      gboolean is_doc_root;

      g_mutex_lock (&self->apps_lock);
      if (g_vfs_backend_load_apps (self) == FALSE)
        {
          g_vfs_backend_afc_check (AFC_E_INTERNAL_ERROR, G_VFS_JOB (job));
          g_mutex_unlock (&self->apps_lock);
          return;
        }
      g_mutex_unlock (&self->apps_lock);

      app = g_vfs_backend_parse_house_arrest_path (self, TRUE, path, &is_doc_root, &new_path);

      if (app == NULL)
        {
          g_vfs_backend_afc_set_info_from_app (self, info, NULL);
          g_vfs_job_succeeded (G_VFS_JOB(job));
          return;
        }
      else
        {
          AppInfo *app_info;

          app_info = g_hash_table_lookup (self->apps, app);
          g_free (app);
          if (app_info == NULL)
            {
              g_free (new_path);
              g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB(job));
              return;
            }
          if (is_doc_root)
            {
              g_free (new_path);
              g_vfs_backend_afc_set_info_from_app (self, info, app_info);
              g_vfs_job_succeeded (G_VFS_JOB(job));
              return;
            }
          if (G_UNLIKELY(g_vfs_backend_afc_check (afc_get_file_info (app_info->afc_cli, new_path, &afcinfo),
                                                  G_VFS_JOB(job))))
            {
              g_free (new_path);
              return;
            }
        }
    }
  else
    {
      g_assert_not_reached ();
    }

  ptr = strrchr (new_path ? new_path : path, '/');
  if (ptr && ptr[1] != '\0')
    basename = ptr + 1;
  else
    basename = new_path ? new_path : path;

  g_vfs_backend_afc_set_info_from_afcinfo (self, info, afcinfo, basename, new_path ? new_path : path, matcher, flags);
  if (afcinfo)
    g_strfreev (afcinfo);

  g_free (new_path);

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
  afc_client_t afc_cli;

  self = G_VFS_BACKEND_AFC(backend);

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "afc");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, FALSE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS);

  if (!self->connected)
    {
      g_vfs_job_succeeded (G_VFS_JOB(job));
      return;
    }

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      char *app, *new_path;
      AppInfo *info;

      new_path = NULL;
      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, path, NULL, &new_path);
      if (app == NULL)
        {
          g_vfs_backend_afc_check (AFC_E_OP_NOT_SUPPORTED, G_VFS_JOB(job));
          return;
        }
      g_free (new_path);

      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        {
          g_free (app);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB(job));
          return;
        }

      afc_cli = info->afc_cli;
      g_free (app);
    }
  else
    {
      afc_cli = self->afc_cli;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_get_device_info (afc_cli, &kvps), G_VFS_JOB(job))))
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

  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_set_display_name (GVfsBackend *backend,
                                    GVfsJobSetDisplayName *job,
                                    const char *filename,
                                    const char *display_name)
{
  GVfsBackendAfc *self;
  char *afc_path;
  char *new_path;
  char *dirname;
  afc_client_t afc_cli;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      char *app;
      AppInfo *info;
      gboolean is_doc_root;

      new_path = NULL;
      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, filename, &is_doc_root, &afc_path);
      if (app == NULL || is_doc_root)
        {
          g_free (app);
          g_free (afc_path);
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB (job));
          return;
        }

      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        {
          g_free (app);
          g_free (afc_path);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB (job));
          return;
        }

      afc_cli = info->afc_cli;
      g_free (app);
    }
  else
    {
      afc_cli = self->afc_cli;
      afc_path = NULL;
    }

  dirname = g_path_get_dirname (afc_path ? afc_path : filename);
  new_path = g_build_filename (dirname, display_name, NULL);
  g_free (dirname);

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_rename_path (afc_cli,
                                                           afc_path ? afc_path : filename, new_path),
                                          G_VFS_JOB(job))))
    {
      g_free (afc_path);
      g_free (new_path);
      return;
    }

  g_free (new_path);
  g_free (afc_path);

  /* The new path, but in the original namespace */
  dirname = g_path_get_dirname (filename);
  new_path = g_build_filename (dirname, display_name, NULL);
  g_vfs_job_set_display_name_set_new_path (job, new_path);
  g_free (new_path);
  g_free (dirname);

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
  char *new_path;
  afc_client_t afc_cli;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail(self->connected);

  if (g_str_equal (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED) == FALSE)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported"));
      return;
    }

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      char *app;
      AppInfo *info;
      gboolean is_doc_root;

      new_path = NULL;
      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, filename, &is_doc_root, &new_path);
      if (app == NULL || is_doc_root)
        {
          g_free (app);
          g_free (new_path);
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB (job));
          return;
        }

      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        {
          g_free (app);
          g_free (new_path);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB (job));
          return;
        }

      afc_cli = info->afc_cli;
      g_free (app);
    }
  else
    {
      afc_cli = self->afc_cli;
      new_path = NULL;
    }

  mtime = *(guint64*)(value_p) * (guint64)1000000000;

  err = afc_set_file_time (afc_cli, new_path ? new_path : filename, mtime);
  g_free (new_path);

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
  char *new_path;
  afc_client_t afc_cli;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail(self->connected);

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      char *app;
      AppInfo *info;

      new_path = NULL;
      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, path, NULL, &new_path);
      if (app == NULL)
        {
          g_free (app);
          g_free (new_path);
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB (job));
          return;
        }

      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        {
          g_free (app);
          g_free (new_path);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB (job));
          return;
        }

      afc_cli = info->afc_cli;
      g_free (app);
    }
  else
    {
      afc_cli = self->afc_cli;
      new_path = NULL;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_make_directory (afc_cli,
                                                              new_path ? new_path : path),
                                          G_VFS_JOB(job))))
    {
      g_free (new_path);
      return;
    }

  g_free (new_path);
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

  /* Not bothering with symlink creation support in house arrest */
  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      g_vfs_backend_afc_check (AFC_E_OP_NOT_SUPPORTED, G_VFS_JOB (job));
      return;
    }

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
  char *new_src;
  char *new_dst;
  afc_client_t afc_cli;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail(self->connected);

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* FIXME: implement! */

      if (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE)
        {
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_CANT_CREATE_BACKUP,
                                    _("Backups not supported"));
        }
      else
        {
          /* Return G_IO_ERROR_NOT_SUPPORTED instead of G_IO_ERROR_CANT_CREATE_BACKUP
           * to be proceeded with copy and delete fallback (see g_file_move). */
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_NOT_SUPPORTED,
                                    "Operation not supported");
        }

      return;
    }

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      AppInfo *info;
      char *app_src, *app_dst;
      gboolean is_doc_root;

      app_src = g_vfs_backend_parse_house_arrest_path (self, FALSE, source, &is_doc_root, &new_src);
      if (app_src == NULL || is_doc_root)
        {
          g_free (app_src);
          g_free (new_src);
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB(job));
          return;
        }
      app_dst = g_vfs_backend_parse_house_arrest_path (self, FALSE, destination, &is_doc_root, &new_dst);
      if (app_dst == NULL || is_doc_root)
        {
          g_free (app_src);
          g_free (new_src);
          g_free (app_dst);
          g_free (new_dst);
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB(job));
          return;
        }
      if (!g_str_equal (app_dst, app_src))
        {
          g_free (app_src);
          g_free (new_src);
          g_free (app_dst);
          g_free (new_dst);
          g_vfs_backend_afc_check (AFC_E_OP_NOT_SUPPORTED, G_VFS_JOB(job));
          return;
        }
      g_free (app_dst);
      info = g_hash_table_lookup (self->apps, app_src);
      if (info == NULL)
        {
          g_free (app_src);
          g_free (new_src);
          g_free (new_dst);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB(job));
          return;
        }
      afc_cli = info->afc_cli;
      g_free (app_src);
    }
  else
    {
      afc_cli = self->afc_cli;
      new_src = new_dst = NULL;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_rename_path (afc_cli,
                                                           new_src ? new_src : source,
                                                           new_dst ? new_dst : destination),
                                          G_VFS_JOB(job))))
    {
      g_free (new_src);
      g_free (new_dst);
      return;
    }

  g_free (new_src);
  g_free (new_dst);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}

static void
g_vfs_backend_afc_delete (GVfsBackend *backend,
                          GVfsJobDelete *job,
                          const char *filename)
{
  GVfsBackendAfc *self;
  char *new_path;
  afc_client_t afc_cli;

  self = G_VFS_BACKEND_AFC(backend);
  g_return_if_fail (self->connected);

  if (self->mode == ACCESS_MODE_HOUSE_ARREST)
    {
      char *app;
      AppInfo *info;
      gboolean is_doc_root;

      new_path = NULL;
      app = g_vfs_backend_parse_house_arrest_path (self, FALSE, filename, &is_doc_root, &new_path);
      if (app == NULL || is_doc_root)
        {
          g_free (app);
          g_free (new_path);
          g_vfs_backend_afc_check (AFC_E_PERM_DENIED, G_VFS_JOB(job));
          return;
        }

      info = g_hash_table_lookup (self->apps, app);
      if (info == NULL)
        {
          g_free (app);
          g_free (new_path);
          g_vfs_backend_afc_check (AFC_E_OBJECT_NOT_FOUND, G_VFS_JOB(job));
          return;
        }

      afc_cli = info->afc_cli;
      g_free (app);
    }
  else
    {
      afc_cli = self->afc_cli;
      new_path = NULL;
    }

  if (G_UNLIKELY(g_vfs_backend_afc_check (afc_remove_path (afc_cli,
                                                           new_path ? new_path : filename),
                                          G_VFS_JOB(job))))
    {
      g_free (new_path);
      return;
    }

  g_free (new_path);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}


static void
g_vfs_backend_afc_finalize (GObject *obj)
{
  GVfsBackendAfc *self;

  self = G_VFS_BACKEND_AFC(obj);
  g_vfs_backend_afc_close_connection (self);

  idevice_event_unsubscribe ();
  /* After running idevice_event_unsubscribe() we won't get any new event
   * notifications, but we are also guaranteed that currently running event
   * callbacks will have completed, so no other thread is going to requeue
   * an idle after we removed it */
  if (self->force_umount_id != 0) {
      g_source_remove(self->force_umount_id);
      self->force_umount_id = 0;
  }

  g_free (self->uuid);

  if (G_OBJECT_CLASS(g_vfs_backend_afc_parent_class)->finalize)
    (*G_OBJECT_CLASS(g_vfs_backend_afc_parent_class)->finalize) (obj);
}

static void
g_vfs_backend_afc_init (GVfsBackendAfc *self)
{
  if (g_getenv ("GVFS_AFC_DEBUG") != NULL)
    {
      /* enable full debugging */
      idevice_set_debug_level (1);
    }

  g_mutex_init (&self->apps_lock);

  g_vfs_backend_handle_readonly_lockdown (G_VFS_BACKEND (self));
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
  backend_class->truncate         = g_vfs_backend_afc_truncate;
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
