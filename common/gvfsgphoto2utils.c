/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Martin Pitt <martin.pitt@ubuntu.com>
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "gvfsgphoto2utils.h"
#include <string.h>
#include <glib/gi18n-lib.h>

static int hexdigit (char c)
{
  if (c >= 'a')
    return c - 'a' + 10;
  if (c >= 'A')
   return c - 'A' + 10;
  g_return_val_if_fail (c >= '0' && c <= '9', 0);
  return c - '0';
}

/* Do not free result, it's a static buffer */
static const char*
udev_decode_string (const char* encoded)
{
  int len;
  const char* s;
  static char decoded[4096];

  if (encoded == NULL)
    return NULL;

  for (len = 0, s = encoded; *s && len < sizeof (decoded) - 1; ++len, ++s) {
    /* need to check for NUL terminator in advance */
    if (s[0] == '\\' && s[1] == 'x' && s[2] >= '0' && s[3] >= '0') {
      decoded[len] = (hexdigit (s[2]) << 4) | hexdigit (s[3]);
      s += 3;
    } else if (s[0] == '_' || s[0] == '-') {
      decoded[len] = ' ';
    } else {
      decoded[len] = *s;
    }
  }
  decoded[len] = '\0';

  return decoded;
}

char *
g_vfs_get_volume_name (GUdevDevice *device, const char *device_id)
{
  const char *gphoto_name;
  const char *product = NULL;
  const char *vendor;
  const char *model;

  /* our preference: device_id > ID_MEDIA_PLAYER_{VENDOR,PRODUCT} > product >
   * ID_{VENDOR,MODEL} */

  gphoto_name = g_udev_device_get_property (device, device_id);
  if (gphoto_name != NULL && strcmp (gphoto_name, "1") != 0)
    return g_strdup (gphoto_name);

  vendor = g_udev_device_get_property (device, "ID_MEDIA_PLAYER_VENDOR");
  if (vendor == NULL)
    vendor = g_udev_device_get_property (device, "ID_VENDOR_ENC");
  model = g_udev_device_get_property (device, "ID_MEDIA_PLAYER_MODEL");
  if (model == NULL) {
    model = g_udev_device_get_property (device, "ID_MODEL_ENC");
    product = g_udev_device_get_sysfs_attr (device, "product");
  }

  if (product != NULL && strlen (product) > 0)
    return g_strdup (udev_decode_string (product));
  else if (vendor == NULL)
    {
      if (model != NULL)
        return g_strdup (udev_decode_string (model));
    }
  else
    {
      if (model != NULL)
        {
          /* we can't call udev_decode_string() twice in one g_strdup_printf(),
           * it returns a static buffer */
          gchar *temp = g_strconcat (vendor, " ", model, NULL);
          gchar *name = g_strdup (udev_decode_string (temp));
          g_free (temp);
          return name;
        }
      else
        {
          if (g_udev_device_has_property (device, "ID_MEDIA_PLAYER"))
            {
              /* Translators: %s is the device vendor */
              return g_strdup_printf (_("%s Audio Player"), udev_decode_string (vendor));
            }
          else
            {
              /* Translators: %s is the device vendor */
              return g_strdup_printf (_("%s Camera"), udev_decode_string (vendor));
            }
        }
    }

  return g_strdup (_("Camera"));
}

char *
g_vfs_get_volume_icon (GUdevDevice *device)
{
  if (g_udev_device_has_property (device, "ID_MEDIA_PLAYER_ICON_NAME"))
    return g_strdup (g_udev_device_get_property (device, "ID_MEDIA_PLAYER_ICON_NAME"));
  else if (g_udev_device_has_property (device, "ID_MEDIA_PLAYER"))
    return g_strdup ("phone");
  else
    return g_strdup ("camera-photo");
}

char *
g_vfs_get_volume_symbolic_icon (GUdevDevice *device)
{
  if (g_udev_device_has_property (device, "ID_MEDIA_PLAYER_ICON_NAME"))
    return g_strconcat (g_udev_device_get_property (device, "ID_MEDIA_PLAYER_ICON_NAME"), "-symbolic", NULL);
  else if (g_udev_device_has_property (device, "ID_MEDIA_PLAYER"))
    return g_strdup ("phone-symbolic");
  else
    return g_strdup ("camera-photo-symbolic");
}

char **
g_vfs_get_x_content_types (GUdevDevice *device)
{
  char *camera_x_content_types[] = {"x-content/image-dcf", NULL};
  char *media_player_x_content_types[] = {"x-content/audio-player", NULL};

  if (g_udev_device_has_property (device, "ID_MEDIA_PLAYER"))
    return g_strdupv (media_player_x_content_types);
  else
    return g_strdupv (camera_x_content_types);
}
