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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __MAIN_HELPER_H__
#define __MAIN_HELPER_H__

#include "gmountsource.h"

G_BEGIN_DECLS

void          daemon_init       (void);
GMountSpec   *daemon_parse_args (int         argc,
				 char       *argv[],
				 const char *default_type);
void	      daemon_setup	(void);
void          daemon_main       (int         argc,
				 char       *argv[],
				 int max_job_threads,
				 const char *default_type,
				 const char *mountable_name,
				 const char *first_type_name,
				 ...);

G_END_DECLS

#endif /* __MAIN_HELPER__ */
