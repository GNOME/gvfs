/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#ifndef _dirwatch_h_
#define _dirwatch_h_

#include <gio/gio.h>

typedef void          (*DirWatchFunc)           (gpointer       user_data);
typedef struct          OPAQUE_TYPE__DirWatch    DirWatch;

DirWatch               *dir_watch_new           (GFile         *directory,
                                                 GFile         *topdir,
                                                 DirWatchFunc   create,
                                                 DirWatchFunc   check,
                                                 DirWatchFunc   destroy,
                                                 gpointer       user_data);

void                    dir_watch_check         (DirWatch      *watch);

void                    dir_watch_free          (DirWatch      *watch);

#endif /* _dirwatch_h_ */
