/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#ifndef _trashdir_h_
#define _trashdir_h_

#include <gio/gio.h>

#include "trashitem.h"

typedef struct          OPAQUE_TYPE__TrashDir    TrashDir;

TrashDir               *trash_dir_new           (TrashRoot  *root,
                                                 gboolean    watching,
                                                 gboolean    is_homedir,
                                                 const char *mount_point,
                                                 const char *format,
                                                 ...);

void                    trash_dir_free          (TrashDir   *dir);

void                    trash_dir_watch         (TrashDir   *dir);
void                    trash_dir_unwatch       (TrashDir   *dir);
void                    trash_dir_rescan        (TrashDir   *dir);

#endif /* _trashdir_h_ */
