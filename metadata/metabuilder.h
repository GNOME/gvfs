/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Red Hat, Inc.
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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __META_BUILDER_H__
#define __META_BUILDER_H__

#include <glib.h>

typedef struct _MetaBuilder MetaBuilder;
typedef struct _MetaFile MetaFile;
typedef struct _MetaData MetaData;

struct _MetaBuilder {
  MetaFile *root;

  guint32 root_pointer;
  gint64 time_t_base;
};

struct _MetaFile {
  char *name;
  GSequence *children;
  gint64 last_changed;
  GSequence *data;

  guint32 metadata_pointer;
  guint32 children_pointer;
};

struct _MetaData {
  char *key;
  gboolean is_list;
  char *value;
  GList *values;
};

MetaBuilder *meta_builder_new       (void);
void         meta_builder_free      (MetaBuilder *builder);
void         meta_builder_print     (MetaBuilder *builder);
MetaFile *   meta_builder_lookup    (MetaBuilder *builder,
				     const char  *path,
				     gboolean     create);
void         meta_builder_remove    (MetaBuilder *builder,
				     const char  *path,
				     guint64      mtime);
void         meta_builder_copy      (MetaBuilder *builder,
				     const char  *source_path,
				     const char  *dest_path,
				     guint64      mtime);
gboolean     meta_builder_write     (MetaBuilder *builder,
				     const char  *filename);
gboolean     meta_builder_create_new_journal (const char *filename,
				     guint32      random_tag);
char *       meta_builder_get_journal_filename (const char *tree_filename,
				     guint32      random_tag);
gboolean     meta_builder_is_on_nfs (const char  *filename);
MetaFile *   metafile_new           (const char  *name,
				     MetaFile    *parent);
void         metafile_free          (MetaFile    *file);
void         metafile_set_mtime     (MetaFile    *file,
				     guint64      mtime);
MetaFile *   metafile_lookup_child  (MetaFile    *metafile,
				     const char  *name,
				     gboolean     create);
MetaData *   metafile_key_lookup    (MetaFile    *file,
				     const char  *key,
				     gboolean     create);
void         metafile_key_unset     (MetaFile    *metafile,
				     const char  *key);
void         metafile_key_set_value (MetaFile    *metafile,
				     const char  *key,
				     const char  *value);
void         metafile_key_list_set  (MetaFile    *metafile,
				     const char  *key);
void         metafile_key_list_add  (MetaFile    *metafile,
				     const char  *key,
				     const char  *value);

#endif /* __META_BUILDER_H__ */
