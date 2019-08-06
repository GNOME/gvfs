/*
 * gvfs/monitor/afc/afc-volume.c
 *
 * Copyright (c) 2008 Patrick Walton <pcwalton@cs.ucla.edu>
 */

#include <config.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

#include "afcvolume.h"

struct _GVfsAfcVolume {
  GObject parent;

  GVolumeMonitor *monitor;

  char *uuid;
  char *service;

  char *name;
  char *icon;
  char *symbolic_icon;
  char *icon_fallback;
};

static void g_vfs_afc_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_EXTENDED(GVfsAfcVolume, g_vfs_afc_volume, G_TYPE_OBJECT, 0,
                       G_IMPLEMENT_INTERFACE(G_TYPE_VOLUME, g_vfs_afc_volume_iface_init))

static void
g_vfs_afc_volume_finalize (GObject *object)
{
  GVfsAfcVolume *self = NULL;

  self = G_VFS_AFC_VOLUME(object);

  g_free (self->uuid);
  g_free (self->service);

  g_free (self->name);
  g_free (self->icon);
  g_free (self->symbolic_icon);
  g_free (self->icon_fallback);

  if (G_OBJECT_CLASS(g_vfs_afc_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS(g_vfs_afc_volume_parent_class)->finalize) (G_OBJECT(self));
}

static void
g_vfs_afc_volume_init (GVfsAfcVolume *self)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (self);

  afc_volume->name = g_strdup ("iPhone");
  afc_volume->icon = g_strdup ("phone-apple-iphone");
  afc_volume->symbolic_icon = g_strdup ("phone-apple-iphone-symbolic");
}

static void
g_vfs_afc_volume_class_init (GVfsAfcVolumeClass *klass)
{
  GObjectClass *gobject_class;
  gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->finalize = g_vfs_afc_volume_finalize;
}

static gboolean
_g_vfs_afc_volume_check_house_arrest_version (lockdownd_client_t lockdown_cli)
{
  plist_t value;
  lockdownd_error_t lerr;
  gboolean retval;

  value = NULL;
  retval = FALSE;

  lerr = lockdownd_get_value (lockdown_cli, NULL, "ProductVersion", &value);
  if (G_LIKELY(lerr == LOCKDOWN_E_SUCCESS))
    {
      if (plist_get_node_type(value) == PLIST_STRING)
        {
          char *version_string = NULL;

          plist_get_string_val(value, &version_string);
          if (version_string)
            {
              /* parse version */
              int maj = 0;
              int min = 0;
              int rev = 0;

              sscanf(version_string, "%d.%d.%d", &maj, &min, &rev);
              free(version_string);

              if (maj > 3)
                {
                    retval = TRUE;
                }
              else if (maj == 3)
                {
                  if (min > 1)
                        retval = TRUE;
                  if (min == 1 && rev >= 2)
                        retval = TRUE;
              }
            }
        }
    }

  return retval;
}

static int
_g_vfs_afc_volume_update_metadata (GVfsAfcVolume *self)
{
  idevice_t dev;
  lockdownd_client_t lockdown_cli = NULL;
  idevice_error_t err;
  guint retries;
  plist_t value;
  char *model, *display_name;
  lockdownd_service_descriptor_t lockdown_service = NULL;
  lockdownd_error_t lerr;

  retries = 0;
  do {
      err = idevice_new (&dev, self->uuid);
      if (err == IDEVICE_E_SUCCESS)
        break;
      g_usleep (G_USEC_PER_SEC);
  } while (retries++ < 10);

  if (err != IDEVICE_E_SUCCESS)
    {
      g_debug ("Failed to initialise an idevice_t (%d)\n", err);
      return 0;
    }

  lerr = lockdownd_client_new (dev, &lockdown_cli, "gvfs-afc-volume-monitor");
  if (lerr != LOCKDOWN_E_SUCCESS)
    {
      g_debug ("Failed to create a client with handshake (%d)\n", lerr);
      idevice_free (dev);
      return 0;
    }

  if (g_strcmp0 (self->service, HOUSE_ARREST_SERVICE_PORT) == 0)
    {
      if (!_g_vfs_afc_volume_check_house_arrest_version (lockdown_cli))
        {
          g_debug ("Failed to check HouseArrest version\n");
          lockdownd_service_descriptor_free (lockdown_service);
          idevice_free (dev);
          return 0;
        }
    }

  /* try to use pretty device name */
  if (lockdownd_get_device_name (lockdown_cli, &display_name) == LOCKDOWN_E_SUCCESS)
    {
      g_free (self->name);
      if (g_strcmp0 (self->service, HOUSE_ARREST_SERVICE_PORT) == 0)
        {
          /* translators:
           * This is "Documents on foo" where foo is the device name, eg.:
           * Documents on Alan Smithee's iPhone */
          self->name = g_strdup_printf (_("Documents on %s"), display_name);
        }
      else
        self->name = display_name;
    }

  value = NULL;
  if (lockdownd_get_value (lockdown_cli, NULL, "DeviceClass", &value) == LOCKDOWN_E_SUCCESS)
    {
      /* set correct fd icon spec name depending on device model */
      model = NULL;
      plist_get_string_val(value, &model);
      if (g_str_equal(model, "iPod") != FALSE)
        {
          g_free (self->icon);
          self->icon = g_strdup ("multimedia-player-apple-ipod-touch");
          self->symbolic_icon = g_strdup ("multimedia-player-apple-ipod-touch-symbolic");
        }
      else if (g_str_equal(model, "iPad") != FALSE)
        {
          g_free (self->icon);
          self->icon = g_strdup ("computer-apple-ipad");
          self->symbolic_icon = g_strdup ("computer-apple-ipad-symbolic");
        }
      g_free (model);
      plist_free (value);
    }

  lockdownd_client_free (lockdown_cli);
  idevice_free (dev);

  return 1;
}

GVfsAfcVolume *
g_vfs_afc_volume_new (GVolumeMonitor *monitor,
                      const char     *uuid,
                      const char     *service)
{
  GVfsAfcVolume *self;
  GFile *root;
  char *uri;

  self = G_VFS_AFC_VOLUME(g_object_new (G_VFS_TYPE_AFC_VOLUME, NULL));
  self->monitor = monitor;
  self->uuid = g_strdup (uuid);
  self->service = g_strdup (service);

  if (service == NULL)
    uri = g_strdup_printf ("afc://%s", self->uuid);
  else
    uri = g_strdup_printf ("afc://%s:%s", self->uuid, service);

  root = g_file_new_for_uri (uri);
  g_free (uri);

  g_object_set_data_full (G_OBJECT(self), "root", root, g_object_unref);

  /* Get mount information here */
  if (!_g_vfs_afc_volume_update_metadata (self))
      return NULL;

  return self;
}

static char *
g_vfs_afc_volume_get_name (GVolume *volume)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (volume);
  char *name;

  name = g_strdup (afc_volume->name);

  return name;
}

static GIcon *
g_vfs_afc_volume_get_icon (GVolume *volume)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (volume);
  GIcon *icon;

  icon = g_themed_icon_new_with_default_fallbacks (afc_volume->icon);

  return icon;
}

static GIcon *
g_vfs_afc_volume_get_symbolic_icon (GVolume *volume)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (volume);
  GIcon *icon;

  icon = g_themed_icon_new_with_default_fallbacks (afc_volume->symbolic_icon);

  return icon;
}

static char *
g_vfs_afc_volume_get_uuid (GVolume *volume)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (volume);

  return g_strdup (afc_volume->uuid);
}

static gboolean
g_vfs_afc_volume_can_mount (GVolume *volume)
{
  return TRUE;
}

static gboolean
g_vfs_afc_volume_should_automount (GVolume *volume)
{
  return TRUE;
}

static GDrive *
g_vfs_afc_volume_get_drive (GVolume *volume)
{
  return NULL;
}

static GMount *
g_vfs_afc_volume_get_mount (GVolume *volume)
{
  return NULL;
}

typedef struct
{
  GVfsAfcVolume *enclosing_volume;
  GAsyncReadyCallback  callback;
  GFile *root;
  gpointer user_data;
} ActivationMountOp;

static void
mount_callback (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  ActivationMountOp *data = user_data;
  data->callback (G_OBJECT (data->enclosing_volume), res, data->user_data);
  g_object_unref (data->root);
  g_free (data);
}

static void
g_vfs_afc_volume_mount (GVolume             *volume,
                        GMountMountFlags     flags,
                        GMountOperation     *mount_operation,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (volume);
  ActivationMountOp *data;
  GFile *root;

  g_debug ("g_vfs_afc_volume_mount (can_mount=%d uuid=%s)",
           g_vfs_afc_volume_can_mount (volume),
           afc_volume->uuid);

  root = g_object_get_data (G_OBJECT (volume), "root");

  data = g_new0 (ActivationMountOp, 1);
  data->enclosing_volume = afc_volume;
  data->callback = callback;
  data->user_data = user_data;
  data->root = root;

  g_file_mount_enclosing_volume (root,
                                 0,
                                 mount_operation,
                                 cancellable,
                                 mount_callback,
                                 data);
}

static gboolean
g_vfs_afc_volume_mount_finish (GVolume       *volume,
                               GAsyncResult  *result,
                               GError       **error)
{
  GFile *root;
  gboolean res;

  root = g_object_get_data (G_OBJECT (volume), "root");
  res = g_file_mount_enclosing_volume_finish (root, result, error);

  return res;
}

static char *
g_vfs_afc_volume_get_identifier (GVolume              *volume,
                                 const char          *kind)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (volume);
  char *id;

  id = NULL;
  if (g_str_equal (kind, G_VOLUME_IDENTIFIER_KIND_UUID) != FALSE)
    id = g_strdup (afc_volume->uuid);

  return id;
}

static char **
g_vfs_afc_volume_enumerate_identifiers (GVolume *volume)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (volume);
  GPtrArray *res;

  res = g_ptr_array_new ();

  if (afc_volume->uuid && *afc_volume->uuid != 0)
    {
        g_ptr_array_add (res,
                         g_strdup (G_VOLUME_IDENTIFIER_KIND_UUID));
    }

  /* Null-terminate */
  g_ptr_array_add (res, NULL);

  return (char **)g_ptr_array_free (res, FALSE);
}

static GFile *
g_vfs_afc_volume_get_activation_root (GVolume *volume)
{
  GFile *root;

  root = g_object_get_data (G_OBJECT (volume), "root");
  if (root == NULL)
    return NULL;

  return g_object_ref (root);
}

static void
g_vfs_afc_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = g_vfs_afc_volume_get_name;
  iface->get_icon = g_vfs_afc_volume_get_icon;
  iface->get_symbolic_icon = g_vfs_afc_volume_get_symbolic_icon;
  iface->get_uuid = g_vfs_afc_volume_get_uuid;
  iface->get_drive = g_vfs_afc_volume_get_drive;
  iface->get_mount = g_vfs_afc_volume_get_mount;
  iface->can_mount = g_vfs_afc_volume_can_mount;
  iface->should_automount = g_vfs_afc_volume_should_automount;
  iface->mount_fn = g_vfs_afc_volume_mount;
  iface->mount_finish = g_vfs_afc_volume_mount_finish;
  iface->eject = NULL;
  iface->eject_finish = NULL;
  iface->get_identifier = g_vfs_afc_volume_get_identifier;
  iface->enumerate_identifiers = g_vfs_afc_volume_enumerate_identifiers;
  iface->get_activation_root = g_vfs_afc_volume_get_activation_root;
}

gboolean g_vfs_afc_volume_has_uuid(GVfsAfcVolume *volume, const char *uuid)
{
  GVfsAfcVolume *afc_volume = G_VFS_AFC_VOLUME (volume);
  g_return_val_if_fail (uuid != NULL, FALSE);
  return (g_strcmp0 (afc_volume->uuid, uuid) == 0);
}

/*
 * vim: sw=2 ts=8 cindent expandtab cinoptions=f0,>4,n2,{2,(0,^-2,t0 ai
 */
