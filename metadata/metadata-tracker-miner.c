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

#include "config.h"
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "metatree.h"
#include "metabuilder.h"

#include "metadata-tracker-miner.h"

#include <libtracker-sparql/tracker-sparql.h>


#define METADATA_TRACKER_MINER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), METADATA_TYPE_TRACKER_MINER, MetadataTrackerMinerPrivate))
       
struct MetadataTrackerMinerPrivate
{
  TrackerSparqlConnection *connection;
  guint running_connection_attempt;
  
  GQueue *queue;
  guint running_queue_id;
};

typedef enum {
  METADATA_OP_SET,
  METADATA_OP_UNSET,
  METADATA_OP_REMOVE,
} MetadataOperation;


/* Forward declarations */
static void queue_push_data_to_tracker (MetadataTrackerMiner *miner,
                                        const char *filename,
                                        MetadataOperation op,
                                        const char *keyname,
                                        MetaKeyType type,
                                        gpointer value);




void    
metadata_tracker_miner_set_string  (MetadataTrackerMiner *miner,
                                    MetaTree   *tree,
                                    const char *path,
                                    const char *key,
                                    const char *value)
{
  char *real_path;
  
  real_path = meta_tree_get_real_path (tree, path);
  
  queue_push_data_to_tracker (miner, real_path, METADATA_OP_SET, 
                              key, META_KEY_TYPE_STRING, (gpointer) value);
  
//  scan_treefile_for_changes (miner, meta_tree_get_filename (tree), 0);

  g_free (real_path);
}

void    
metadata_tracker_miner_set_stringv (MetadataTrackerMiner *miner,
                                    MetaTree    *tree,
                                    const char  *path,
                                    const char  *key,
                                    char       **value)
{
  char *real_path;
  
  real_path = meta_tree_get_real_path (tree, path);

  queue_push_data_to_tracker (miner, real_path, METADATA_OP_SET,
                              key, META_KEY_TYPE_STRINGV, (gpointer) value);
  
  g_free (real_path);
}

void
metadata_tracker_miner_unset (MetadataTrackerMiner *miner,
                              MetaTree   *tree,
                              const char *path,
                              const char *key)
{
  char *real_path;
  
  real_path = meta_tree_get_real_path (tree, path);

  queue_push_data_to_tracker (miner, real_path, METADATA_OP_UNSET,
                              key, META_KEY_TYPE_NONE, NULL);
  
  g_free (real_path);
}

void
metadata_tracker_miner_remove (MetadataTrackerMiner *miner,
                               MetaTree   *tree,
                               const char *path)
{
  char *real_path;
  
  real_path = meta_tree_get_real_path (tree, path);

  queue_push_data_to_tracker (miner, real_path, METADATA_OP_REMOVE,
                              NULL, META_KEY_TYPE_NONE, NULL);
  
  g_free (real_path);
}






typedef struct {
  MetadataTrackerMiner *miner;
  char *path;
  MetaTree *tree;
} TreeScanDirEntry;

typedef struct {
  MetadataTrackerMiner *miner;
  char *real_path;
  MetaTree *tree;
} TreeScanKeyEntry;

static gboolean 
scan_enumerate_keys_callback (const char *key,
                              MetaKeyType type,
                              gpointer value,
                              gpointer user_data)
{
  TreeScanKeyEntry *entry;
  
  entry = user_data;
  
  switch (type)
    {
      case META_KEY_TYPE_NONE:
        g_warning ("found key of type META_KEY_TYPE_NONE: '%s', file '%s ", key, entry->real_path);
        break;
      case META_KEY_TYPE_STRING:
      case META_KEY_TYPE_STRINGV:
        queue_push_data_to_tracker (entry->miner, entry->real_path, METADATA_OP_SET, 
                                    key, type, value);
        break;
    }
  
  return TRUE;
}

static gboolean 
scan_enumerate_dir_callback (const char *entry_name,
                             guint64 last_changed,
                             gboolean has_children,
                             gboolean has_data,
                             gpointer user_data)
{
  TreeScanDirEntry *entry, *new_entry;
  TreeScanKeyEntry *key_entry;

  entry = user_data;

  if (has_children)
    {
      new_entry = g_new0 (TreeScanDirEntry, 1);
      new_entry->tree = entry->tree;  /* FIXME: ref? */
      new_entry->path = g_build_path (G_DIR_SEPARATOR_S, entry->path, entry_name, NULL);
      new_entry->miner = g_object_ref (entry->miner); 

      meta_tree_enumerate_dir (new_entry->tree, new_entry->path, scan_enumerate_dir_callback, new_entry);

      g_object_unref (new_entry->miner);
      g_free (new_entry->path);
      g_free (new_entry);
    }
  
  if (has_data)
    {
      char *local_path;
      
      local_path = g_build_filename (entry->path, entry_name, NULL);

      key_entry = g_new0 (TreeScanKeyEntry, 1);
      key_entry->tree = entry->tree;  /* FIXME: ref? */
      key_entry->real_path = meta_tree_get_real_path (entry->tree, local_path);
      key_entry->miner = g_object_ref (entry->miner); 

      meta_tree_enumerate_keys (key_entry->tree, local_path, scan_enumerate_keys_callback, key_entry);

      g_object_unref (key_entry->miner);
      g_free (key_entry->real_path);
      g_free (key_entry);

      g_free (local_path);
    }
  
  return TRUE;
}

void
scan_treefile_for_changes (MetadataTrackerMiner *miner,
                           const char *treefile,
                           guint64 last_update)
{
  MetaTree *tree;
  TreeScanDirEntry *entry;
  
  /* TODO: implement last_update */

  tree = meta_tree_open (treefile, FALSE);
  if (tree == NULL)
    {
      g_warning ("can't open file '%s'", treefile);
      return;
    }
   
  entry = g_new0 (TreeScanDirEntry, 1);
  entry->tree = tree;  /* FIXME: ref? */
  entry->path = g_strdup ("/");
  entry->miner = g_object_ref (miner); 
  meta_tree_enumerate_dir (tree, entry->path, 
                           scan_enumerate_dir_callback, entry);
  g_object_unref (entry->miner);
  g_free (entry->path);
  g_free (entry);

  meta_tree_unref (tree);
}






typedef struct {
  char *filename;
  MetadataOperation op;
  char *key;
  MetaKeyType type;
  gpointer value;
} MetadataTrackerMinerEntry;

static gboolean process_queue (MetadataTrackerMiner *miner);
static void schedule_queue_pickup (MetadataTrackerMiner *miner);


static void
push_to_tracker_cb (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  GError *error;

  error = NULL;

  tracker_sparql_connection_update_finish (TRACKER_SPARQL_CONNECTION (source), result, &error);
  if (error != NULL) 
    {
      g_critical ("Could not push metadata to Tracker: %s", error->message);
      g_error_free (error);
    }

  /* continue with next item or bail out if no more available */
  g_idle_add ((GSourceFunc) process_queue, user_data); 
}

static gboolean
process_queue (MetadataTrackerMiner *miner)
{
  MetadataTrackerMinerEntry *entry;
  TrackerSparqlBuilder *sparql;
  GFile *file;
  char *data;

  g_return_val_if_fail (miner->priv->connection != NULL, FALSE);
  
  g_print ("process_queue: Picking up queue\n");
  
  entry = g_queue_pop_head (miner->priv->queue);
  if (entry == NULL)
    {
      miner->priv->running_queue_id = 0;
      return FALSE;
    }
  

  g_print ("process_queue: Pushing '%s' key for '%s'\n", entry->key, entry->filename);
  file = g_file_new_for_path (entry->filename);
  
  /*
   *  tracker-sparql -q 'SELECT ?file ?comm WHERE { ?file nie:comment ?comm }'
   *  tracker-sparql -u -q "insert { <file:///mnt/flash/105E4003.NIC> a nie:InformationElement ; nie:comment 'ahoj' }"
   * 
   */

  /* FIXME: I hope there will be soon a SPARQL command to just update a
   * value instead to delete and re-insert it
   */

  sparql = tracker_sparql_builder_new_update ();

  
  if (entry->op == METADATA_OP_SET ||
      entry->op == METADATA_OP_UNSET ||
      entry->op == METADATA_OP_REMOVE)
    {
      tracker_sparql_builder_delete_open (sparql, NULL);
      tracker_sparql_builder_subject_iri (sparql, g_file_get_uri (file));
      tracker_sparql_builder_predicate (sparql, "nie:comment");
      tracker_sparql_builder_object_variable (sparql, "unknown");
      tracker_sparql_builder_delete_close (sparql);

      tracker_sparql_builder_where_open (sparql);
      tracker_sparql_builder_subject_iri (sparql, g_file_get_uri (file));
      tracker_sparql_builder_predicate (sparql, "nie:comment");
      tracker_sparql_builder_object_variable (sparql, "unknown");
      tracker_sparql_builder_where_close (sparql);
    }

  if (entry->op == METADATA_OP_SET)
    {
      data = NULL;
      switch (entry->type)
        {
          case META_KEY_TYPE_NONE:
            g_assert_not_reached();
            break;
          case META_KEY_TYPE_STRING:
            data = g_strdup_printf ("%s=%s", entry->key, (char *) entry->value);
            break;
          case META_KEY_TYPE_STRINGV:
            data = g_strdup_printf ("%s=%s", entry->key, *((char **) entry->value));   /* FIXME */
            break;
        }

      tracker_sparql_builder_insert_open (sparql, NULL);
      tracker_sparql_builder_subject_iri (sparql, g_file_get_uri (file));
      tracker_sparql_builder_predicate (sparql, "a");
    //  tracker_sparql_builder_object (sparql, "nfo:FileDataObject");
      tracker_sparql_builder_object (sparql, "nie:InformationElement");
      tracker_sparql_builder_predicate (sparql, "nie:comment");
      tracker_sparql_builder_object_string (sparql, data);
      tracker_sparql_builder_insert_close (sparql);

      g_free (data);
    }
    
  tracker_sparql_connection_update_async (miner->priv->connection,
                                          tracker_sparql_builder_get_result (sparql),
                                          G_PRIORITY_DEFAULT,
                                          NULL,
                                          push_to_tracker_cb,
                                          miner);
  g_object_unref (sparql);
  g_object_unref (file);

  /* free the entry data */ 
  g_free (entry->filename);
  g_free (entry->key);
  switch (entry->type)
    {
      case META_KEY_TYPE_NONE:
        break;
      case META_KEY_TYPE_STRING:
        g_free (entry->value);
        break;
      case META_KEY_TYPE_STRINGV:
        g_strfreev (entry->value);
        break;
    }
  
  return FALSE;
}


static void
queue_push_data_to_tracker (MetadataTrackerMiner *miner,
                            const char *filename,
                            MetadataOperation op,
                            const char *keyname,
                            MetaKeyType type,
                            gpointer value)
{
  MetadataTrackerMinerEntry *entry;
  
  g_print ("queue_push_string_to_tracker: pushing '%s' for '%s'\n", keyname, filename);
  
  entry = g_new0 (MetadataTrackerMinerEntry, 1);
  entry->filename = g_strdup (filename);
  entry->op = op;
  entry->key = g_strdup (keyname);
  entry->type = type;
  switch (entry->type)
    {
      case META_KEY_TYPE_NONE:
        g_assert (entry->op != METADATA_OP_SET);
        break;
      case META_KEY_TYPE_STRING:
        entry->value = g_strdup (value);
        break;
      case META_KEY_TYPE_STRINGV:
        entry->value = g_strdupv (value);
        break;
    }
  
  /* FIXME: mutex? */
  g_queue_push_head (miner->priv->queue, entry);
  schedule_queue_pickup (miner);
}

static void
create_connection_cb (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
  TrackerSparqlConnection *connection;
  MetadataTrackerMiner *miner;
  GError *error;

  miner = user_data;
  error = NULL;
  
  connection = tracker_sparql_connection_get_finish (res, &error);
  if (error != NULL)
    {
      g_critical ("Could not initialize Tracker: %s", error->message);
      g_error_free (error);
    }
  miner->priv->connection = connection;
  miner->priv->running_connection_attempt = 0;
  g_print ("create_connection_cb: Initialized.\n");
 
  schedule_queue_pickup (miner);
}

static gboolean
create_connection_timeout (MetadataTrackerMiner *miner)
{
  tracker_sparql_connection_get_async (NULL, create_connection_cb, miner);

  return FALSE;
}

static void
schedule_queue_pickup (MetadataTrackerMiner *miner)
{
  if (miner->priv->connection != NULL)
    {
      /* FIXME: mutex? */
      if (miner->priv->running_queue_id == 0)
        miner->priv->running_queue_id = g_idle_add ((GSourceFunc) process_queue, miner);
    }
  else
    {
      if (miner->priv->running_connection_attempt == 0)
        miner->priv->running_connection_attempt = g_timeout_add_seconds (5, (GSourceFunc) create_connection_timeout, miner);
    }
}








G_DEFINE_TYPE (MetadataTrackerMiner, metadata_tracker_miner, G_TYPE_OBJECT)

static void
metadata_tracker_miner_finalize (GObject *object)
{
  MetadataTrackerMinerPrivate *priv;

  priv = METADATA_TRACKER_MINER_GET_PRIVATE (object);

  g_queue_free (priv->queue);
 
  if (priv->connection)
    g_object_unref (priv->connection);

  G_OBJECT_CLASS (metadata_tracker_miner_parent_class)->finalize (object);
}

static void
metadata_tracker_miner_class_init (MetadataTrackerMinerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = metadata_tracker_miner_finalize;

  g_type_class_add_private (object_class, sizeof (MetadataTrackerMinerPrivate));
}

static void
metadata_tracker_miner_init (MetadataTrackerMiner *object)
{
  object->priv = METADATA_TRACKER_MINER_GET_PRIVATE (object);

  object->priv->running_queue_id = 0;
  
  object->priv->queue = g_queue_new ();

  object->priv->running_connection_attempt = 1;  /* spawn manually now */
  /* FIXME: do we need cancellable? */
  tracker_sparql_connection_get_async (NULL, create_connection_cb, object);
}

MetadataTrackerMiner *
metadata_tracker_miner_new ()
{
  MetadataTrackerMiner *result;
  
  result = g_object_new (METADATA_TYPE_TRACKER_MINER, NULL);

  return result;
}
