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

#ifndef __G_VFS_KEYRING_H__
#define __G_VFS_KEYRING_H__

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean g_vfs_keyring_is_available    (void);
gboolean g_vfs_keyring_lookup_password (const gchar *username,
					const gchar *host,
					const gchar *domain,
					const gchar *protocol,
					const gchar *object,
					const gchar *authtype,
					guint32      port,
					gchar      **username_out,
					gchar      **domain_out,
					gchar      **password);
gboolean g_vfs_keyring_save_password   (const gchar  *username,
					const gchar  *host,
					const gchar  *domain,
					const gchar  *protocol,
					const gchar  *object,
					const gchar  *authtype,
					guint32       port,
					const gchar  *password,
					GPasswordSave flags);

G_END_DECLS

#endif /* __G_VFS_KEYRING_H__ */
