/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>
#include <string.h>
#include <glib/gi18n-lib.h>

#ifdef HAVE_BLURAY
#include <langinfo.h>
#include <libbluray/bluray.h>
#include <libbluray/meta_data.h>
#endif /* HAVE_BLURAY */

#include "gvfsmountinfo.h"

#define VOLUME_INFO_GROUP "Volume Info"

static GFile *
_g_find_file_insensitive_finish (GFile        *parent,
                                 GAsyncResult *result,
                                 GError      **error);

static void
_g_find_file_insensitive_async (GFile              *parent,
                                const gchar        *name,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data);

static void
on_icon_file_located (GObject       *source_object,
                      GAsyncResult  *res,
                      gpointer      user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GFile *icon_file;
  GError *error;

  error = NULL;
  icon_file = _g_find_file_insensitive_finish (G_FILE (source_object),
                                               res,
                                               &error);
  if (icon_file != NULL)
    {
      g_simple_async_result_set_op_res_gpointer (simple, g_file_icon_new (icon_file), NULL);
      g_object_unref (icon_file);
    }
  else
    {
      g_simple_async_result_set_from_error (simple, error);
      g_error_free (error);
    }
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
on_autorun_loaded (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GFile *autorun_file;
  gchar *content;
  gchar *relative_icon_path;
  gsize content_length;
  GError *error;

  relative_icon_path = NULL;

  autorun_file = G_FILE (source_object);

  error = NULL;
  if (g_file_load_contents_finish (autorun_file,
                                   res,
                                   &content,
                                   &content_length,
                                   NULL,
                                   &error))
    {
      /* Scan through for an "icon=" line. Can't use GKeyFile,
       * because .inf files aren't always valid key files
       **/
      GRegex *icon_regex;
      GMatchInfo *match_info;

      /* [^,] is because sometimes the icon= line
       * has a comma at the end
       **/
      icon_regex = g_regex_new ("icon\\s*=\\s*+([^,\\r\\n]+)",
                                G_REGEX_CASELESS, 0, NULL);
      g_regex_match (icon_regex, content, 0,
                     &match_info);

      /* Even if there are multiple matches, pick only the
       * first.
       **/
      if (g_match_info_matches (match_info))
        {
          gchar *chr;
          gchar *word = g_match_info_fetch (match_info, 1);

          /* Replace '\' with '/' */
          while ((chr = strchr (word, '\\')) != NULL)
            *chr = '/';

          /* If the file name's not valid UTF-8,
           * don't even try to load it
           **/
          if (g_utf8_validate (word, -1, NULL))
            {
              relative_icon_path = word;
            }
          else
            {
              /* TODO: mark for translation. Strictly, this isn't very important; this string
               * will never be displayed since all current users of g_vfs_mount_info_query_autorun_info()
               * passes NULL for the GError**.
               */
              error = g_error_new_literal (G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           "Icon name is not valid UTF-8");
              g_free (word);
            }
        }

      g_match_info_free (match_info);

      g_regex_unref (icon_regex);
      g_free (content);
    }

  /* some autorun.in points to the .exe file for the icon; make sure we avoid using that */
  if (relative_icon_path != NULL)
    {
      if (!g_str_has_suffix (relative_icon_path, ".exe"))
        {
          GFile *root;

          root = g_file_get_parent (autorun_file);

          _g_find_file_insensitive_async (root,
                                          relative_icon_path,
                                          NULL,
                                          on_icon_file_located,
                                          simple);

          g_object_unref (root);
        }
      else
        {
          /* TODO: mark for translation. Strictly, this isn't very important; this string
           * will never be displayed since all current users of g_vfs_mount_info_query_autorun_info()
           * passes NULL for the GError**.
           */
          error = g_error_new_literal (G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "Icon is an .exe file");
        }
    }

  if (error != NULL)
    {
      g_simple_async_result_set_from_error (simple, error);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      g_error_free (error);
    }

  g_free (relative_icon_path);
}

static void
on_autorun_located (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GFile *autorun_path;
  GError *error;

  error = NULL;
  autorun_path = _g_find_file_insensitive_finish (G_FILE (source_object),
                                                  res,
                                                  &error);
  if (error != NULL)
    {
      g_simple_async_result_set_from_error (simple, error);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      g_error_free (error);
    }
  else
    {
      g_file_load_contents_async (autorun_path,
                                  g_object_get_data (G_OBJECT (simple), "cancellable"),
                                  on_autorun_loaded,
                                  simple);
      g_object_unref (autorun_path);
    }
}

void
g_vfs_mount_info_query_autorun_info (GFile               *directory,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GSimpleAsyncResult *simple;

  simple = g_simple_async_result_new (G_OBJECT (directory),
                                      callback,
                                      user_data,
                                      g_vfs_mount_info_query_autorun_info);

  if (cancellable != NULL)
    g_object_set_data_full (G_OBJECT (simple), "cancellable", g_object_ref (cancellable), g_object_unref);

  _g_find_file_insensitive_async (directory,
                                  "autorun.inf",
                                  cancellable,
                                  on_autorun_located,
                                  simple);
}

GIcon *
g_vfs_mount_info_query_autorun_info_finish (GFile          *directory,
                                            GAsyncResult   *res,
                                            GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GIcon *ret;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == g_vfs_mount_info_query_autorun_info);

  ret = NULL;

  if (g_simple_async_result_propagate_error (simple, error))
    goto out;

  ret = g_simple_async_result_get_op_res_gpointer (simple);

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_xdg_volume_info_loaded (GObject      *source_object,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GFile *xdg_volume_info_file;
  gchar *content;
  gsize content_length;
  GError *error;
  GKeyFile *key_file;
  gchar *name;
  gchar *icon_name;
  gchar *icon_file;
  GIcon *icon;

  content = NULL;
  key_file = NULL;
  name = NULL;
  icon_name = NULL;
  icon_file = NULL;

  xdg_volume_info_file = G_FILE (source_object);

  error = NULL;
  if (g_file_load_contents_finish (xdg_volume_info_file,
                                   res,
                                   &content,
                                   &content_length,
                                   NULL,
                                   &error))
    {
      key_file = g_key_file_new ();
      if (!g_key_file_load_from_data (key_file,
                                      content,
                                      content_length,
                                      G_KEY_FILE_NONE,
                                      &error))
        goto out;


      name = g_key_file_get_locale_string (key_file,
                                           VOLUME_INFO_GROUP,
                                           "Name",
                                           NULL,
                                           NULL);

      icon_name = g_key_file_get_string (key_file,
                                         VOLUME_INFO_GROUP,
                                         "Icon",
                                         NULL);
      
      icon_file = g_key_file_get_string (key_file,
                                         VOLUME_INFO_GROUP,
                                         "IconFile",
                                         NULL);

      icon = NULL;
      
      if (icon_file != NULL)
        {
          GFile *dir, *f;

          dir = g_file_get_parent (xdg_volume_info_file);
          if (dir)
            {
              f = g_file_resolve_relative_path (dir, icon_file);
              if (f)
                {
                  icon = g_file_icon_new (f);
                  g_object_unref (f);
                }
              
              g_object_unref (dir);
            }
        }
            
      if (icon == NULL && icon_name != NULL)
        {
          icon = g_themed_icon_new (icon_name);
          g_themed_icon_append_name (G_THEMED_ICON (icon), "drive-removable-media");
          g_themed_icon_append_name (G_THEMED_ICON (icon), "drive-removable");
          g_themed_icon_append_name (G_THEMED_ICON (icon), "drive");
        }

      g_simple_async_result_set_op_res_gpointer (simple, icon, NULL);
      g_object_set_data_full (G_OBJECT (simple), "name", name, g_free);
      name = NULL; /* steals name */
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
    }

 out:

  if (key_file != NULL)
    g_key_file_free (key_file);

  if (error != NULL)
    {
      g_simple_async_result_set_from_error (simple, error);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      g_error_free (error);
    }

  g_free (name);
  g_free (icon_name);
  g_free (icon_file);
  g_free (content);
}

void
g_vfs_mount_info_query_xdg_volume_info (GFile               *directory,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  GSimpleAsyncResult *simple;
  GFile *file;

  simple = g_simple_async_result_new (G_OBJECT (directory),
                                      callback,
                                      user_data,
                                      g_vfs_mount_info_query_xdg_volume_info);

  file = g_file_resolve_relative_path (directory, ".xdg-volume-info");
  g_file_load_contents_async (file,
                              cancellable,
                              on_xdg_volume_info_loaded,
                              simple);
  g_object_unref (file);
}

GIcon *g_vfs_mount_info_query_xdg_volume_info_finish (GFile          *directory,
                                                      GAsyncResult   *res,
                                                      gchar         **out_name,
                                                      GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GIcon *ret;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == g_vfs_mount_info_query_xdg_volume_info);

  ret = NULL;

  if (g_simple_async_result_propagate_error (simple, error))
    goto out;

  ret = g_simple_async_result_get_op_res_gpointer (simple);

  if (out_name != NULL)
    *out_name = g_strdup (g_object_get_data (G_OBJECT (simple), "name"));

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

#ifdef HAVE_BLURAY
static const char *
get_iso_639_3_for_locale (void)
{
  const char *lang = NULL;

#ifdef HAVE_NL_ADDRESS_LANG_TERM
  lang = nl_langinfo (_NL_ADDRESS_LANG_TERM);
  if (lang == NULL || *lang == '\0')
    {
#ifdef HAVE_NL_ADDRESS_COUNTRY_AB3
      lang = nl_langinfo (_NL_ADDRESS_COUNTRY_AB3);
      if (lang == NULL || *lang == '\0')
#endif
        return NULL;
    }
#endif

  return lang;
}

static const char *
get_icon (const META_DL *meta)
{
  const char *icon;
  guint i;
  guint size;

  icon = NULL;
  size = 0;

  for (i = 0; i < meta->thumb_count; i++)
    {
      if (meta->thumbnails[i].xres > size)
        {
          icon = meta->thumbnails[i].path;
          size = meta->thumbnails[i].xres;
        }
    }

  return icon;
}

static void
bdmv_metadata_thread (GSimpleAsyncResult *result,
                      GObject *object,
                      GCancellable *cancellable)
{
  BLURAY *bd;
  const META_DL *meta;
  GError *error;
  GFile *file;
  char *disc_root;
  char *icon;
  char *name;
  const char *lang;
  char *path;

  file = G_FILE (object);

  disc_root = g_file_get_path (file);
  if (!disc_root)
    {
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "Device is not a Blu-Ray disc");
      goto error;
    }

  path = g_build_filename (disc_root, "BDMV", NULL);
  if (!g_file_test (path, G_FILE_TEST_IS_DIR))
    {
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "Device is not a Blu-Ray disc");
      goto error;
    }
  g_free (path);

  bd = bd_open (disc_root, NULL);
  g_free (disc_root);

  if (bd == NULL)
    {
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "Device is not a Blu-Ray disc");
      goto error;
    }

  lang = get_iso_639_3_for_locale ();
  if (lang != NULL)
    bd_set_player_setting_str (bd, BLURAY_PLAYER_SETTING_MENU_LANG, lang);

  meta = bd_get_meta (bd);
  if (meta == NULL)
    {
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "Device is not a Blu-Ray disc, or has no metadata");
      bd_close (bd);
      goto error;
    }
  name = icon = NULL;

  if (meta != NULL)
    {
      if (meta->di_name && *meta->di_name)
        name = g_strdup (meta->di_name);
      icon = g_strdup (get_icon (meta));
    }

  /* We're missing either an icon, or the name */
  if (!name || !icon)
    {
      bd_set_player_setting_str (bd, BLURAY_PLAYER_SETTING_MENU_LANG, "eng");
      meta = bd_get_meta (bd);

      if (meta != NULL && name == NULL && meta->di_name && *meta->di_name)
        name = g_strdup (meta->di_name);

      if (meta != NULL && icon == NULL)
        icon = g_strdup (get_icon (meta));
    }

  /* Set the results */
  if (icon != NULL)
    {
      char *icon_path;
      GFile *icon_file;

      icon_path = g_strdup_printf ("BDMV/META/DL/%s", icon);
      g_free (icon);
      icon_file = g_file_resolve_relative_path (file, icon_path);
      g_free (icon_path);

      g_simple_async_result_set_op_res_gpointer (result,
                                                 g_file_icon_new (icon_file),
                                                 NULL);
      g_object_unref (icon_file);
    }
  else
    {
      g_simple_async_result_set_op_res_gpointer (result, NULL, NULL);
    }

  if (name != NULL)
    g_object_set_data_full (G_OBJECT (result), "name", name, g_free);

  bd_close (bd);

  return;

error:
  g_simple_async_result_set_from_error (result, error);
  g_simple_async_result_set_op_res_gpointer (result, NULL, NULL);
  g_error_free (error);
}
#endif /* HAVE_BLURAY */

void
g_vfs_mount_info_query_bdmv_volume_info (GFile               *directory,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
#ifdef HAVE_BLURAY
  GSimpleAsyncResult *simple;

  simple = g_simple_async_result_new (G_OBJECT (directory),
                                      callback,
                                      user_data,
                                      g_vfs_mount_info_query_bdmv_volume_info);
  g_simple_async_result_run_in_thread (simple,
                                       (GSimpleAsyncThreadFunc) bdmv_metadata_thread,
                                       G_PRIORITY_DEFAULT,
                                       cancellable);
  g_object_unref (simple);
#else
  GSimpleAsyncResult *simple;

  simple = g_simple_async_result_new (G_OBJECT (directory),
                                      callback,
                                      user_data,
                                      g_vfs_mount_info_query_bdmv_volume_info);
  g_simple_async_result_set_error (simple,
                                   G_IO_ERROR,
                                   G_IO_ERROR_NOT_SUPPORTED,
                                   "gvfs built without Expat support, no BDMV support");
  g_object_unref (simple);
#endif /* HAVE_BLURAY */
}

GIcon *g_vfs_mount_info_query_bdmv_volume_info_finish (GFile          *directory,
                                                       GAsyncResult   *res,
                                                       gchar         **out_name,
                                                       GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GIcon *ret;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == g_vfs_mount_info_query_bdmv_volume_info);

  ret = NULL;

  if (g_simple_async_result_propagate_error (simple, error))
    goto out;

  ret = g_simple_async_result_get_op_res_gpointer (simple);

  if (out_name != NULL)
    *out_name = g_strdup (g_object_get_data (G_OBJECT (simple), "name"));

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

#define INSENSITIVE_SEARCH_ITEMS_PER_CALLBACK 100

static void
enumerated_children_callback (GObject *source_object, GAsyncResult *res,
                              gpointer user_data);

static void
more_files_callback (GObject *source_object, GAsyncResult *res,
                     gpointer user_data);

static void
find_file_insensitive_exists_callback (GObject *source_object,
                                       GAsyncResult *res,
                                       gpointer user_data);

typedef struct _InsensitiveFileSearchData
{
        GFile *root;
        gchar *original_path;
        gchar **split_path;
        gint index;
        GFileEnumerator *enumerator;
        GFile *current_file;

        GCancellable *cancellable;
        GAsyncReadyCallback callback;
        gpointer user_data;
} InsensitiveFileSearchData;

static void
_g_find_file_insensitive_async (GFile              *parent,
                                const gchar        *name,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
  InsensitiveFileSearchData *data;
  GFile *direct_file = g_file_get_child (parent, name);

  data = g_new0 (InsensitiveFileSearchData, 1);
  data->cancellable = cancellable;
  data->callback = callback;
  data->user_data = user_data;
  data->root = g_object_ref (parent);
  data->original_path = g_strdup (name);

  g_file_query_info_async (direct_file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                           G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                           cancellable,
                           find_file_insensitive_exists_callback, data);


}

static void
clear_find_file_insensitive_state (InsensitiveFileSearchData *data)
{
  if (data->root)
    g_object_unref (data->root);
  g_free (data->original_path);
  if (data->split_path)
    g_strfreev (data->split_path);
  if (data->enumerator)
    g_object_unref (data->enumerator);
  if (data->current_file)
    g_object_unref (data->current_file);
  g_free (data);
}

static void
find_file_insensitive_exists_callback (GObject *source_object,
                                       GAsyncResult *res,
                                       gpointer user_data)
{
  GFileInfo *info;
  InsensitiveFileSearchData *data = (InsensitiveFileSearchData *) (user_data);

  /* The file exists and can be found with the given path, no need to search. */
  if ((info = g_file_query_info_finish (G_FILE (source_object), res, NULL)))
    {
      GSimpleAsyncResult *simple;

      simple = g_simple_async_result_new (G_OBJECT (data->root),
                                          data->callback,
                                          data->user_data,
                                          _g_find_file_insensitive_async);

      g_simple_async_result_set_op_res_gpointer (simple, g_object_ref (source_object), g_object_unref);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      clear_find_file_insensitive_state (data);
      g_object_unref (info);
    }

  else
    {
      data->split_path = g_strsplit (data->original_path, G_DIR_SEPARATOR_S, -1);
      data->index = 0;
      data->enumerator = NULL;
      data->current_file = g_object_ref (data->root);

      /* Skip any empty components due to multiple slashes */
      while (data->split_path[data->index] != NULL &&
             *data->split_path[data->index] == 0)
        data->index++;

      g_file_enumerate_children_async (data->current_file,
                                       G_FILE_ATTRIBUTE_STANDARD_NAME,
                                       0, G_PRIORITY_DEFAULT,
                                       data->cancellable,
                                       enumerated_children_callback, data);
    }

  g_object_unref (source_object);
}

static void
enumerated_children_callback (GObject *source_object, GAsyncResult *res,
                              gpointer user_data)
{
  GFileEnumerator *enumerator;
  InsensitiveFileSearchData *data = (InsensitiveFileSearchData *) (user_data);

  enumerator = g_file_enumerate_children_finish (G_FILE (source_object),
                                                 res, NULL);

  if (enumerator == NULL)
    {
      GSimpleAsyncResult *simple;
      GFile *file;

      simple = g_simple_async_result_new (G_OBJECT (data->root),
                                          data->callback,
                                          data->user_data,
                                          _g_find_file_insensitive_async);

      file = g_file_get_child (data->root, data->original_path);

      g_simple_async_result_set_op_res_gpointer (simple, g_object_ref (file), g_object_unref);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      clear_find_file_insensitive_state (data);
      return;
    }

  data->enumerator = enumerator;
  g_file_enumerator_next_files_async (enumerator,
                                      INSENSITIVE_SEARCH_ITEMS_PER_CALLBACK,
                                      G_PRIORITY_DEFAULT,
                                      data->cancellable,
                                      more_files_callback,
                                      data);
}

static void
more_files_callback (GObject *source_object, GAsyncResult *res,
                     gpointer user_data)
{
  InsensitiveFileSearchData *data = (InsensitiveFileSearchData *) (user_data);
  GList *files, *l;
  gchar *filename = NULL, *component, *case_folded_name,
    *name_collation_key;
  gboolean end_of_files, is_utf8;

  files = g_file_enumerator_next_files_finish (data->enumerator,
                                               res, NULL);

  end_of_files = files == NULL;

  component = data->split_path[data->index];
  g_return_if_fail (component != NULL);

  is_utf8 = (g_utf8_validate (component, -1, NULL));
  if (is_utf8)
    {
      case_folded_name = g_utf8_casefold (component, -1);
      name_collation_key = g_utf8_collate_key (case_folded_name, -1);
      g_free (case_folded_name);
    }

  else
    {
      name_collation_key = g_ascii_strdown (component, -1);
    }

  for (l = files; l != NULL; l = l->next)
    {
      GFileInfo *info;
      const gchar *this_name;
      gchar *key;

      info = l->data;
      this_name = g_file_info_get_name (info);

      if (is_utf8 && g_utf8_validate (this_name, -1, NULL))
        {
          gchar *case_folded;
          case_folded = g_utf8_casefold (this_name, -1);
          key = g_utf8_collate_key (case_folded, -1);
          g_free (case_folded);
        }
      else
        {
          key = g_ascii_strdown (this_name, -1);
        }

      if (strcmp (key, name_collation_key) == 0)
          filename = g_strdup (this_name);
      g_free (key);

      if (filename)
        break;
    }

  g_list_free_full (files, g_object_unref);
  g_free (name_collation_key);

  if (filename)
    {
      GFile *next_file;

      g_file_enumerator_close_async (data->enumerator,
                                     G_PRIORITY_DEFAULT,
                                     data->cancellable,
                                     NULL, NULL);
      g_object_unref (data->enumerator);
      data->enumerator = NULL;

      /* Set the current file and continue searching */
      next_file = g_file_get_child (data->current_file, filename);
      g_free (filename);
      g_object_unref (data->current_file);
      data->current_file = next_file;

      data->index++;
      /* Skip any empty components due to multiple slashes */
      while (data->split_path[data->index] != NULL &&
             *data->split_path[data->index] == 0)
        data->index++;

      if (data->split_path[data->index] == NULL)
       {
          /* Search is complete, file was found */
          GSimpleAsyncResult *simple;

          simple = g_simple_async_result_new (G_OBJECT (data->root),
                                              data->callback,
                                              data->user_data,
                                              _g_find_file_insensitive_async);

          g_simple_async_result_set_op_res_gpointer (simple, g_object_ref (data->current_file), g_object_unref);
          g_simple_async_result_complete_in_idle (simple);
          g_object_unref (simple);
          clear_find_file_insensitive_state (data);
          return;
        }

      /* Continue searching down the tree */
      g_file_enumerate_children_async (data->current_file,
                                       G_FILE_ATTRIBUTE_STANDARD_NAME,
                                       0, G_PRIORITY_DEFAULT,
                                       data->cancellable,
                                       enumerated_children_callback,
                                       data);
      return;
    }

  if (end_of_files)
    {
      /* Could not find the given file, abort the search */
      GSimpleAsyncResult *simple;
      GFile *file;

      g_object_unref (data->enumerator);
      data->enumerator = NULL;

      simple = g_simple_async_result_new (G_OBJECT (data->root),
                                          data->callback,
                                          data->user_data,
                                          _g_find_file_insensitive_async);

      file = g_file_get_child (data->root, data->original_path);
      g_simple_async_result_set_op_res_gpointer (simple, file, g_object_unref);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      clear_find_file_insensitive_state (data);
      return;
    }

  /* Continue enumerating */
  g_file_enumerator_next_files_async (data->enumerator,
                                      INSENSITIVE_SEARCH_ITEMS_PER_CALLBACK,
                                      G_PRIORITY_DEFAULT,
                                      data->cancellable,
                                      more_files_callback,
                                      data);
}

static GFile *
_g_find_file_insensitive_finish (GFile        *parent,
                                 GAsyncResult *result,
                                 GError      **error)
{
  GSimpleAsyncResult *simple;
  GFile *file;

  g_return_val_if_fail (G_IS_SIMPLE_ASYNC_RESULT (result), NULL);

  simple = G_SIMPLE_ASYNC_RESULT (result);

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  file = G_FILE (g_simple_async_result_get_op_res_gpointer (simple));
  return g_object_ref (file);
}
