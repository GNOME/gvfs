/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#ifndef _trashexpunger_h_
#define _trashexpunger_h_

#include <gio/gio.h>

typedef struct OPAQUE_TYPE__TrashExpunger TrashExpunger;
void trash_expunge (GFile *expunge_directory);

#endif /* _trashexpunger_h_ */
