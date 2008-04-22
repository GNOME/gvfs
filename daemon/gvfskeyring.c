/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Carlos Garcia Campos
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
 * Author: Carlos Garcia Campos <carlosgc@gnome.org>
 */

#include <config.h>

#ifdef HAVE_KEYRING
#include <gnome-keyring.h>
#endif

#include "gvfskeyring.h"

gboolean
g_vfs_keyring_is_available (void)
{
#ifdef HAVE_KEYRING
  return gnome_keyring_is_available ();
#else
  return FALSE;
#endif
}

gboolean
g_vfs_keyring_lookup_password (const gchar *username,
                               const gchar *host,
                               const gchar *domain,
                               const gchar *protocol,
			       const gchar *object,
			       const gchar *authtype,
			       guint32      port, 
                               gchar      **username_out,
                               gchar      **domain_out,
                               gchar      **password_out)
{
#ifdef HAVE_KEYRING
  GnomeKeyringNetworkPasswordData *pwd_data;
  GnomeKeyringResult               result;
  GList                           *plist;

  if (!gnome_keyring_is_available ())
    return FALSE;

  result = gnome_keyring_find_network_password_sync (
    username,
    domain,
    host,
    object,
    protocol,
    authtype,
    port,
    &plist);
  
  if (result != GNOME_KEYRING_RESULT_OK || plist == NULL)
    return FALSE;

  /* We use the first result, which is the least specific match */
  pwd_data = (GnomeKeyringNetworkPasswordData *)plist->data;

  *password_out = g_strdup (pwd_data->password);

  if (username_out)
    *username_out = g_strdup (pwd_data->user);
  
  if (domain_out)
    *domain_out = g_strdup (pwd_data->domain);
      
  gnome_keyring_network_password_list_free (plist);
  
  return TRUE;
#else
  return FALSE;
#endif /* HAVE_KEYRING */
}

gboolean
g_vfs_keyring_save_password (const gchar  *username,
                             const gchar  *host,
                             const gchar  *domain,
                             const gchar  *protocol,
			     const gchar  *object,
			     const gchar  *authtype,
			     guint32       port,
                             const gchar  *password,
                             GPasswordSave flags)
{
#ifdef HAVE_KEYRING
  GnomeKeyringResult result;
  const gchar       *keyring;
  guint32            item_id;
  
  if (!gnome_keyring_is_available ())
    return FALSE;

  if (flags == G_PASSWORD_SAVE_NEVER)
    return FALSE;

  keyring = (flags == G_PASSWORD_SAVE_FOR_SESSION) ? "session" : NULL;

  result = gnome_keyring_set_network_password_sync (
    keyring,
    username,
    domain,
    host,
    object,
    protocol,
    authtype,
    port,
    password,
    &item_id);

  return (result == GNOME_KEYRING_RESULT_OK);
#else
  return FALSE;
#endif /* HAVE_KEYRING */
}
