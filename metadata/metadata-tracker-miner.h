/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2010 Red Hat, Inc.
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
 * Author: Tomas Bzatek <tbzatek@redhat.com>
 */

#ifndef __TRACKER_MINER_H__
#define __TRACKER_MINER_H__

#include <glib.h>
#include <glib-object.h>

#include "metatree.h"


G_BEGIN_DECLS


/* TODO:
 *  - queue messages, send them in a separate thread
 *  - if sending fails, set a flag to manually reindex, start watching for tracker-store to come up
 *  - what to do if connection to tracker fails? don't queue messages to prevent overflow and increasing memory consumption?
 *  - offline indexer: how to delete old metadata, not available in the metatree file? (no way to determine which ones have been deleted)
 *  - handle moves
 *  - move scan_treefile_for_changes() to thread? (time consuming operation) 
 * 
 */

#define METADATA_TYPE_TRACKER_MINER         (metadata_tracker_miner_get_type())
#define METADATA_TRACKER_MINER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), METADATA_TYPE_TRACKER_MINER, MetadataTrackerMiner))
#define METADATA_TRACKER_MINER_CLASS(c)     (G_TYPE_CHECK_CLASS_CAST ((c), METADATA_TYPE_TRACKER_MINER, MetadataTrackerMinerClass))
#define METADATA_IS_TRACKER_MINER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), METADATA_TYPE_TRACKER_MINER))
#define METADATA_IS_TRACKER_MINER_CLASS(c)  (G_TYPE_CHECK_CLASS_TYPE ((c),  METADATA_TYPE_TRACKER_MINER))
#define METADATA_TRACKER_MINER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), METADATA_TYPE_TRACKER_MINER, MetadataTrackerMinerClass))

typedef struct MetadataTrackerMinerPrivate MetadataTrackerMinerPrivate;

typedef struct {
  GObject parent;
  MetadataTrackerMinerPrivate *priv;
} MetadataTrackerMiner;

typedef struct {
  GObjectClass parent_class;
} MetadataTrackerMinerClass;


GType                  metadata_tracker_miner_get_type   (void);

MetadataTrackerMiner * metadata_tracker_miner_new (void);

void                   metadata_tracker_miner_set_string  (MetadataTrackerMiner *miner,
                                                           MetaTree   *tree,
                                                           const char *path,
                                                           const char *key,
                                                           const char *value);
void                   metadata_tracker_miner_set_stringv (MetadataTrackerMiner *miner,
                                                           MetaTree   *tree,
                                                           const char *path,
                                                           const char *key,
                                                           char       **value);
void                   metadata_tracker_miner_unset       (MetadataTrackerMiner *miner,
                                                           MetaTree   *tree,
                                                           const char *path,
                                                           const char *key);
void                   metadata_tracker_miner_remove      (MetadataTrackerMiner *miner,
                                                           MetaTree   *tree,
                                                           const char *path);

void                   scan_treefile_for_changes          (MetadataTrackerMiner *miner,
                                                           const char *treefile,
                                                           guint64 last_update);

G_END_DECLS

#endif /* __TRACKER_MINER_H__ */
