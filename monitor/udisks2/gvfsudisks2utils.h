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

#ifndef __GVFS_UDISKS2_UTILS_H__
#define __GVFS_UDISKS2_UTILS_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "gvfsudisks2volumemonitor.h"

G_BEGIN_DECLS

void gvfs_udisks2_utils_udisks_error_to_gio_error (GError *error);

G_END_DECLS

#endif /* __GVFS_UDISKS2_UTILS_H__ */
