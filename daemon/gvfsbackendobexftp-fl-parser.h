/*
 * Copyright (C) 2004 Nokia Corporation.
 * Copyright (C) 2008 Bastien Nocera <hadess@hadess.net> (gio port)
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

#ifndef __GVFSBACKENDOBEXFTP_FL_PARSER_H__
#define __GVFSBACKENDOBEXFTP_FL_PARSER_H__

#include <glib.h>

gboolean     gvfsbackendobexftp_fl_parser_parse             (const gchar *buf,
							     gint         len,
							     GList      **elements,
							     GError     **error);
void         gvfsbackendobexftp_fl_parser_free_element_list (GSList      *elements);



guint        om_mem_type_id_from_string     (const gchar *memtype);
const gchar *om_mem_type_id_to_string       (guint        mem_id);

#endif /* __GVFSBACKENDOBEXFTP_FL_PARSER_H__ */
