/*
 * Copyright (C) 2004 Nokia Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __OVU_CAP_PARSER_H__
#define __OVU_CAP_PARSER_H__

#include <glib.h>

typedef struct _OvuCaps       OvuCaps;
typedef struct _OvuCapsMemory OvuCapsMemory;

OvuCaps *     ovu_caps_parser_parse       (const gchar       *buf,
					   gint               len,
					   GError           **error);

GList *          ovu_caps_get_memory_entries        (OvuCaps         *caps);
OvuCapsMemory   *ovu_caps_get_memory_type           (OvuCaps         *caps,
						     const gchar     *mem_type);
void             ovu_caps_free                      (OvuCaps         *caps);
OvuCapsMemory *  ovu_caps_memory_new                (const gchar     *type,
						     goffset          free,
						     goffset          used,
						     gboolean         has_free,
						     gboolean         has_used,
						     gboolean         case_sensitive);
void             ovu_caps_memory_free               (OvuCapsMemory   *memory);
gboolean         ovu_caps_memory_equal              (OvuCapsMemory   *m1,
						     OvuCapsMemory   *m2);
const gchar *    ovu_caps_memory_get_type           (OvuCapsMemory   *memory);
goffset          ovu_caps_memory_get_used           (OvuCapsMemory   *memory);
goffset          ovu_caps_memory_get_free           (OvuCapsMemory   *memory);
gboolean         ovu_caps_memory_has_used           (OvuCapsMemory   *memory);
gboolean         ovu_caps_memory_has_free           (OvuCapsMemory   *memory);
gboolean         ovu_caps_memory_get_case_sensitive (OvuCapsMemory   *memory);

#endif /* __OVU_CAP_PARSER_H__ */

