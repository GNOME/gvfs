/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#ifndef _trashwatcher_h_
#define _trashwatcher_h_

#include "trashitem.h"

typedef struct  OPAQUE_TYPE__TrashWatcher     TrashWatcher;

TrashWatcher   *trash_watcher_new            (TrashRoot    *root);
void            trash_watcher_free           (TrashWatcher *watcher);

void            trash_watcher_watch          (TrashWatcher *watcher);
void            trash_watcher_unwatch        (TrashWatcher *watcher);
void            trash_watcher_rescan         (TrashWatcher *watcher);

#endif /* _trashitem_h_ */
