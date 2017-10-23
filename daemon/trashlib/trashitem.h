/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#ifndef _trashitem_h_
#define _trashitem_h_

#include <gio/gio.h>

typedef struct  OPAQUE_TYPE__TrashRoot        TrashRoot;
typedef struct  OPAQUE_TYPE__TrashItem        TrashItem;

typedef void  (*trash_item_notify)           (TrashItem          *item,
                                              gpointer            user_data);
typedef void  (*trash_size_change)           (gpointer            user_data);

/* trash root -- the set of all toplevel trash items */
TrashRoot      *trash_root_new               (trash_item_notify   create,
                                              trash_item_notify   delete,
                                              trash_size_change   size_change,
                                              gpointer            user_data);
void            trash_root_free              (TrashRoot          *root);

/* add/remove trash items (safe only from one thread) */
void            trash_root_add_item          (TrashRoot          *root,
                                              GFile              *file,
                                              GFile              *topdir,
                                              gboolean            in_homedir);
void            trash_root_remove_item       (TrashRoot          *root,
                                              GFile              *file,
                                              gboolean            in_homedir);
void            trash_root_thaw              (TrashRoot          *root);

/* query trash items, holding references (safe from any thread) */
int             trash_root_get_n_items       (TrashRoot          *root);
GList          *trash_root_get_items         (TrashRoot          *root);
TrashItem      *trash_root_lookup_item       (TrashRoot          *root,
                                              const char         *escaped);

void            trash_item_list_free         (GList              *list);
void            trash_item_unref             (TrashItem          *item);

/* query a trash item (safe while holding a reference to it) */
const char     *trash_item_get_escaped_name  (TrashItem          *item);
const char     *trash_item_get_delete_date   (TrashItem          *item);
GFile          *trash_item_get_original      (TrashItem          *item);
GFile          *trash_item_get_file          (TrashItem          *item);

/* delete a trash item (safe while holding a reference to it) */
gboolean        trash_item_delete            (TrashItem          *item,
                                              GError            **error);
gboolean        trash_item_restore           (TrashItem          *item,
                                              GFile              *dest,
					      GFileCopyFlags      flags,
                                              GError            **error);

#endif /* _trashitem_h_ */
