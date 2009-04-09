/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
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
 */


#ifndef __POLKIT_H__
#define __POLKIT_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <polkit/polkit.h>

G_BEGIN_DECLS

void _obtain_authz (const gchar        *action_id,
                    GCancellable       *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer            user_data);

gboolean _obtain_authz_finish (GAsyncResult  *res,
                               GError       **error);

G_END_DECLS

#endif /* __POLKIT_H__ */
