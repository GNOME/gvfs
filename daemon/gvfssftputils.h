/* gvfssftputils.h
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 * SPDX-FileCopyrightText: 2026 Red Hat, Inc.
 */

#pragma once

#include <glib.h>

gboolean g_vfs_sftp_validate_reply (const guint8 *data, gsize size);
