/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 *         Christian Kellner <gicmo@gnome.org>
 */

#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "string.h"

#include "hal-utils.h"

static const struct {
  const char *disc_type;
  const char *icon_name;
  char *ui_name;
  char *ui_name_blank;
} disc_data[] = {
  {"cd_rom",        "media-optical-cd-rom", N_("CD-ROM Disc"), N_("Blank CD-ROM Disc")},
  {"cd_r",          "media-optical-cd-r", N_("CD-R Disc"), N_("Blank CD-R Disc")},
  {"cd_rw",         "media-optical-cd-rw", N_("CD-RW Disc"), N_("Blank CD-RW Disc")},
  {"dvd_rom",       "media-optical-dvd-rom", N_("DVD-ROM Disc"), N_("Blank DVD-ROM Disc")},
  {"dvd_ram",       "media-optical-dvd-ram", N_("DVD-RAM Disc"), N_("Blank DVD-RAM Disc")},
  {"dvd_r",         "media-optical-dvd-r", N_("DVD-ROM Disc"), N_("Blank DVD-ROM Disc")},
  {"dvd_rw",        "media-optical-dvd-rw", N_("DVD-RW Disc"), N_("Blank DVD-RW Disc")},
  {"dvd_plus_r",    "media-optical-dvd-r-plus", N_("DVD+R Disc"), N_("Blank DVD+R Disc")},
  {"dvd_plus_rw",   "media-optical-dvd-rw-plus",  N_("DVD+RW Disc"), N_("Blank DVD+RW Disc")},
  {"dvd_plus_r_dl", "media-optical-dvd-dl-r-plus", N_("DVD+R DL Disc"), N_("Blank DVD+R DL Disc")},
  {"bd_rom",        "media-optical-bd-rom", N_("Blu-Ray Disc"), N_("Blank Blu-Ray Disc")},
  {"bd_r",          "media-optical-bd-r", N_("Blu-Ray R Disc"), N_("Blank Blu-Ray R Disc")},
  {"bd_re",         "media-optical-bd-re", N_("Blu-Ray RW Disc"), N_("Blank Blu-Ray RW Disc")},
  {"hddvd_rom",     "media-optical-hddvd-rom", N_("HD DVD Disc"), N_("Blank HD DVD Disc")},
  {"hddvd_r",       "media-optical-hddvd-r", N_("HD DVD-R Disc"), N_("Blank HD DVD-R Disc")},
  {"hddvd_rw",      "media-optical-hddvd-rw", N_("HD DVD-RW Disc"), N_("Blank HD DVD-RW Disc")},
  {"mo",            "media-optical-mo", N_("MO Disc"), N_("Blank MO Disc")},
  {NULL,            "media-optical", N_("Disc"), N_("Blank Disc")}
};

const char *
get_disc_icon (const char *disc_type)
{
  int n;

  for (n = 0; disc_data[n].disc_type != NULL; n++)
    {
      if (strcmp (disc_data[n].disc_type, disc_type) == 0)
        break;
    }

  return disc_data[n].icon_name;
}

const char *
get_disc_name (const char *disc_type, gboolean is_blank)
{
  int n;

  for (n = 0; disc_data[n].disc_type != NULL; n++)
    {
      if (strcmp (disc_data[n].disc_type, disc_type) == 0)
        break;
    }

  if (is_blank)
    return dgettext (GETTEXT_PACKAGE, disc_data[n].ui_name_blank);
  else
    return dgettext (GETTEXT_PACKAGE, disc_data[n].ui_name);
}

/*
 * Creates a GThemedIcon from icon_name and creates default
 * fallbacks from fallbacks. Is smart in the case that icon_name
 * and fallbacks are identically.
 * Note: See the GThemedIcon documentation for more information
 * on default fallbacks
 */
GIcon *
get_themed_icon_with_fallbacks (const char *icon_name,
                                const char *fallbacks)
{
  int i = 0, dashes = 0;
  const char *p;
  char *dashp;
  char *last;
  char **names;
  GIcon *icon;

  if (G_UNLIKELY (icon_name == NULL))
    return NULL;

  if (fallbacks == NULL)
    return g_themed_icon_new (icon_name);

  p = fallbacks;
  while (*p)
    {
      if (*p == '-')
        dashes++;
      p++;
    }

  if (strcmp (icon_name, fallbacks))
    {
      names = g_new (char *, dashes + 3);
      names[i++] = g_strdup (icon_name);
    }
  else
    names = g_new (char *, dashes + 2);

  names[i++] = last = g_strdup (fallbacks);

  while ((dashp = strrchr (last, '-')) != NULL)
    names[i++] = last = g_strndup (last, dashp - last);

  names[i++] = NULL;
  icon =  g_themed_icon_new_from_names (names, -1);
  g_strfreev (names);

  return icon;
}

char **
dupv_and_uniqify (char **str_array)
{
  int n, m, o;
  int len;
  char **result;

  result = g_strdupv (str_array);
  len = g_strv_length (result);

  for (n = 0; n < len; n++)
    {
      char *s = result[n];
      for (m = n + 1; m < len; m++)
        {
          char *p = result[m];
          if (strcmp (s, p) == 0)
            {
              for (o = m + 1; o < len; o++)
                result[o - 1] = result[o];
              len--;
              result[len] = NULL;
              m--;
            }
        }
    }

  return result;
}
