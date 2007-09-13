#include <config.h>

#include <string.h>

#include "gcontenttypeprivate.h"
#include "gdesktopappinfo.h"
#include "gioerror.h"
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

static void g_desktop_app_info_iface_init (GAppInfoIface *iface);

static GList *get_all_desktop_entries_for_mime_type (const char *base_mime_type);
static void mime_info_cache_reload (const char *dir);

struct _GDesktopAppInfo
{
  GObject parent_instance;

  char *desktop_id;
  char *filename;

  char *name;
  char *comment;
  gboolean nodisplay;
  char *icon;
  char **only_show_in;
  char **not_show_in;
  char *try_exec;
  char *exec;
  char *binary;
  char *path;
  gboolean terminal;
  gboolean startup_notify;
};

G_DEFINE_TYPE_WITH_CODE (GDesktopAppInfo, g_desktop_app_info, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_APP_INFO,
						g_desktop_app_info_iface_init))

static gpointer
search_path_init (gpointer data)
{
  char **args = NULL;
  const char * const *data_dirs;
  const char *user_data_dir;
  int i, length, j;

  data_dirs = g_get_system_data_dirs ();
  for (length = 0; data_dirs[length] != NULL; length++)
    ;
  
  args = g_new (char *, length + 2);
  
  j = 0;
  user_data_dir = g_get_user_data_dir ();
  args[j++] = g_build_filename (user_data_dir, "applications", NULL);
  for (i = 0; i < length; i++)
    args[j++] = g_build_filename (data_dirs[i],
				  "applications", NULL);
  args[j++] = NULL;
  
  return args;
}
  
static const char * const *
get_applications_search_path (void)
{
  static GOnce once_init = G_ONCE_INIT;
  return g_once (&once_init, search_path_init, NULL);
}

static void
g_desktop_app_info_finalize (GObject *object)
{
  GDesktopAppInfo *info;

  info = G_DESKTOP_APP_INFO (object);

  g_free (info->desktop_id);
  g_free (info->filename);
  g_free (info->name);
  g_free (info->comment);
  g_free (info->icon);
  g_strfreev (info->only_show_in);
  g_strfreev (info->not_show_in);
  g_free (info->try_exec);
  g_free (info->exec);
  g_free (info->binary);
  g_free (info->path);
  
  if (G_OBJECT_CLASS (g_desktop_app_info_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_desktop_app_info_parent_class)->finalize) (object);
}

static void
g_desktop_app_info_class_init (GDesktopAppInfoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_desktop_app_info_finalize;
}

static void
g_desktop_app_info_init (GDesktopAppInfo *local)
{
}

static GDesktopAppInfo *
g_desktop_app_info_new_from_filename (const char *filename,
				      gboolean *hidden)
{
  GDesktopAppInfo *info;
  GKeyFile *key_file;
  char *start_group;
  char *type;
  char *try_exec;

  *hidden = FALSE;
  
  key_file = g_key_file_new ();
  
  if (!g_key_file_load_from_file (key_file,
				  filename,
				  G_KEY_FILE_NONE,
				  NULL))
    return NULL;

  start_group = g_key_file_get_start_group (key_file);
  if (start_group == NULL || strcmp (start_group, "Desktop Entry") != 0)
    {
      g_free (start_group);
      return NULL;
    }
  g_free (start_group);
  
  
  type = g_key_file_get_string (key_file, "Desktop Entry", "Type", NULL);
  if (type == NULL || strcmp (type, "Application") != 0)
    {
      g_free (type);
      return NULL;
    }
  g_free (type);

  if (g_key_file_get_boolean (key_file, "Desktop Entry", "Hidden", NULL))
    {
      *hidden = TRUE;
      return NULL;
    }

  try_exec = g_key_file_get_string (key_file, "Desktop Entry", "TryExec", NULL);
  if (try_exec)
    {
      char *t;
      t = g_find_program_in_path (try_exec);
      if (t == NULL)
	{
	  g_free (try_exec);
	  return NULL;
	}
    }
  
  info = g_object_new (G_TYPE_DESKTOP_APP_INFO, NULL);
  info->filename = g_strdup (filename);

  info->name = g_key_file_get_locale_string (key_file, "Desktop Entry", "Name", NULL, NULL);
  info->comment = g_key_file_get_locale_string (key_file, "Desktop Entry", "Comment", NULL, NULL);
  info->nodisplay = g_key_file_get_boolean (key_file, "Desktop Entry", "NoDisplay", NULL);
  info->icon =  g_key_file_get_locale_string (key_file, "Desktop Entry", "Icon", NULL, NULL);
  info->only_show_in = g_key_file_get_string_list (key_file, "Desktop Entry", "OnlyShowIn", NULL, NULL);
  info->not_show_in = g_key_file_get_string_list (key_file, "Desktop Entry", "NotShowIn", NULL, NULL);
  info->try_exec = try_exec;
  info->exec = g_key_file_get_string (key_file, "Desktop Entry", "Exec", NULL);
  info->path = g_key_file_get_string (key_file, "Desktop Entry", "Path", NULL);
  info->terminal = g_key_file_get_boolean (key_file, "Desktop Entry", "Terminal", NULL);
  info->startup_notify = g_key_file_get_boolean (key_file, "Desktop Entry", "StartupNotify", NULL);

  if (info->exec)
    {
      char *p, *start;

      p = info->exec;
      while (*p == ' ')
	p++;
      start = p;
      while (*p != ' ' && *p != 0)
	p++;
      
      info->binary = g_strndup (start, p - start);
    }
  
  return info;
}

GAppInfo *
g_desktop_app_info_new (const char *desktop_id)
{
  GDesktopAppInfo *appinfo;
  const char * const *dirs;
  int i;
  gboolean hidden;

  
  dirs = get_applications_search_path ();

  for (i = 0; dirs[i] != NULL; i++)
    {
      char *basename;
      char *filename;
      char *p;

      basename = g_strdup (desktop_id);

      filename = g_build_filename (dirs[i], basename, NULL);
      appinfo = g_desktop_app_info_new_from_filename (filename, &hidden);
      g_free (filename);
      if (appinfo != NULL || hidden)
	{
	  g_free (basename);
	  goto found;
	}

      p = basename;
      while ((p = strchr (p, '-')) != NULL)
	{
	  *p = '/';
	  
	  filename = g_build_filename (dirs[i], basename, NULL);
	  appinfo = g_desktop_app_info_new_from_filename (filename, &hidden);
	  g_free (filename);
	  if (appinfo != NULL || hidden)
	    {
	      g_free (basename);
	      goto found;
	    }
	  *p = '-';
	  p++;
	}
    }
  
  return NULL;

 found:
  if (appinfo)
    appinfo->desktop_id = g_strdup (desktop_id);
  
  return G_APP_INFO (appinfo);
}

static GAppInfo *
g_desktop_app_info_dup (GAppInfo *appinfo)
{
  GDesktopAppInfo *info = G_DESKTOP_APP_INFO (appinfo);
  GDesktopAppInfo *new_info;
  
  new_info = g_object_new (G_TYPE_DESKTOP_APP_INFO, NULL);

  new_info->filename = g_strdup (info->filename);
  new_info->desktop_id = g_strdup (info->desktop_id);
  
  new_info->name = g_strdup (info->name);
  new_info->comment = g_strdup (info->comment);
  new_info->nodisplay = info->nodisplay;
  new_info->icon = g_strdup (info->icon);
  new_info->only_show_in = g_strdupv (info->only_show_in);
  new_info->not_show_in = g_strdupv (info->not_show_in);
  new_info->try_exec = g_strdup (info->try_exec);
  new_info->exec = g_strdup (info->exec);
  new_info->binary = g_strdup (info->binary);
  new_info->path = g_strdup (info->path);
  new_info->terminal = info->terminal;
  new_info->startup_notify = info->startup_notify;
  
  return G_APP_INFO (new_info);
}

static gboolean
g_desktop_app_info_equal (GAppInfo *appinfo1,
			  GAppInfo *appinfo2)
{
  GDesktopAppInfo *info1 = G_DESKTOP_APP_INFO (appinfo1);
  GDesktopAppInfo *info2 = G_DESKTOP_APP_INFO (appinfo2);

  if (info1->binary == NULL ||
      info2->binary == NULL)
    return FALSE;

  return strcmp (info1->binary, info2->binary) == 0;
}

static char *
g_desktop_app_info_get_name (GAppInfo *appinfo)
{
  GDesktopAppInfo *info = G_DESKTOP_APP_INFO (appinfo);

  if (info->name == NULL)
    return g_strdup (_("Unnamed"));
  //return g_strdup (info->name);
  return g_strdup_printf ("%s - %s", info->name, info->desktop_id);
}

static char *
g_desktop_app_info_get_description (GAppInfo *appinfo)
{
  GDesktopAppInfo *info = G_DESKTOP_APP_INFO (appinfo);
  
  return g_strdup (info->comment);
}

static char *
g_desktop_app_info_get_icon (GAppInfo *appinfo)
{
  GDesktopAppInfo *info = G_DESKTOP_APP_INFO (appinfo);

  /* TODO: How to handle icons */
  return g_strdup (info->icon);
}

static char *
expand_macro_single (char macro, const char *uri)
{
  char *result = NULL, *path;
  
  switch (macro)
    {
    case 'u':
    case 'U':	
      result = g_shell_quote (uri);
      break;
    case 'f':
    case 'F':
      path = g_filename_from_uri (uri, NULL, NULL);
      if (path)
	{
	  result = g_shell_quote (path);
	  g_free (path);
	}
      break;
    case 'd':
    case 'D':
      path = g_filename_from_uri (uri, NULL, NULL);
      if (path)
	{
	  result = g_shell_quote (g_path_get_dirname (path));
	  g_free (path);
	}
      break;
    case 'n':
    case 'N':
      path = g_filename_from_uri (uri, NULL, NULL);
      if (path)
	{
	  result = g_shell_quote (g_path_get_basename (path));
	  g_free (path);
	}
      break;
    }
  
  return result;
}

static void
expand_macro (char macro, GString *exec, GDesktopAppInfo *info, GList **uri_list)
{
  GList *uris = *uri_list;
  char *expanded;
  
  g_return_if_fail (uris != NULL);
  g_return_if_fail (exec != NULL);
  
  if (uris == NULL)
    return;
  
  switch (macro)
    {
    case 'u':
    case 'f':
    case 'd':
    case 'n':
      expanded = expand_macro_single (macro, uris->data);
      if (expanded)
	{
	  g_string_append (exec, expanded);
	  g_free (expanded);
	}
      uris = uris->next;
      break;
    case 'U':	
    case 'F':
    case 'D':
    case 'N':
      while (uris)
	{
	  expanded = expand_macro_single (macro, uris->data);
	  if (expanded)
	    {
	      g_string_append (exec, expanded);
	      g_free (expanded);
	    }
	  
	  uris = uris->next;
	  
	  if (uris != NULL && expanded)
	    g_string_append_c (exec, ' ');
	}
      break;
    case 'i':
      if (info->icon)
	{
	  g_string_append (exec, "--icon ");
	  g_string_append (exec, info->icon);
	}
      break;
    case 'c':
      if (info->name) 
	g_string_append (exec, info->name);
      break;
    case 'k':
      if (info->filename) 
	g_string_append (exec, info->filename);
      break;
    case 'm': /* deprecated */
      break;
    case '%':
      g_string_append_c (exec, '%');
      break;
    }
  
  *uri_list = uris;
}

static gboolean
expand_application_parameters (GDesktopAppInfo *info,
			       GList         **uris,
			       int            *argc,
			       char         ***argv,
			       GError        **error)
{
  GList *uri_list = *uris;
  const char *p = info->exec;
  GString *expanded_exec = g_string_new (NULL);
  gboolean res;
  
  if (info->exec == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Desktop file didn't specify Exec field"));
      return FALSE;
    }
  
  while (*p)
    {
      if (p[0] == '%' && p[1] != '\0')
	{
	  expand_macro (p[1], expanded_exec, info, uris);
	  p++;
	}
      else
	g_string_append_c (expanded_exec, *p);
      
      p++;
    }
  
  /* No file substitutions */
  if (uri_list == *uris && uri_list != NULL)
    {
      /* If there is no macro default to %f. This is also what KDE does */
      g_string_append_c (expanded_exec, ' ');
      expand_macro ('f', expanded_exec, info, uris);
    }
  
  res = g_shell_parse_argv (expanded_exec->str, argc, argv, error);
  g_string_free (expanded_exec, TRUE);
  return res;
}

static gboolean
g_desktop_app_info_launch (GAppInfo                *appinfo,
			   GList                   *filenames,
			   char                   **envp,
			   GError                 **error)
{
  GList *uris;
  char *uri;
  gboolean res;

  uris = NULL;
  while (filenames)
    {
      uri = g_filename_to_uri (filenames->data, NULL, NULL);
      if (uri == NULL)
	g_warning ("Invalid filename passed to g_desktop_app_info_launch");
      
      if (uri)
	uris = g_list_prepend (uris, uri);
    }
  
  uris = g_list_reverse (uris);
  
  res = g_app_info_launch_uris (appinfo, uris, envp, error);
  
  g_list_foreach  (uris, (GFunc)g_free, NULL);
  g_list_free (uris);
  
  return res;
}

static gboolean
g_desktop_app_info_supports_uris (GAppInfo *appinfo)
{
  GDesktopAppInfo *info = G_DESKTOP_APP_INFO (appinfo);
  
  return
    (strstr (info->exec, "%u") != NULL) ||
    (strstr (info->exec, "%U") != NULL);
}

static gboolean
prepend_terminal_to_vector (int *argc,
			    char ***argv)
{
#ifndef G_OS_WIN32
  char **real_argv;
  int real_argc;
  int i, j;
  char **term_argv = NULL;
  int term_argc = 0;
  char *check;
  char **the_argv;
  
  g_return_val_if_fail (argc != NULL, FALSE);
  g_return_val_if_fail (argv != NULL, FALSE);
	
  /* sanity */
  if(*argv == NULL)
    *argc = 0;
  
  the_argv = *argv;

  /* compute size if not given */
  if (*argc < 0)
    {
      for (i = 0; the_argv[i] != NULL; i++)
	;
      *argc = i;
    }
  
  term_argc = 2;
  term_argv = g_new0 (char *, 3);

  check = g_find_program_in_path ("gnome-terminal");
  if (check != NULL)
    {
      term_argv[0] = check;
      /* Note that gnome-terminal takes -x and
       * as -e in gnome-terminal is broken we use that. */
      term_argv[1] = g_strdup ("-x");
    }
  else
    {
      if (check == NULL)
	check = g_find_program_in_path ("nxterm");
      if (check == NULL)
	check = g_find_program_in_path ("color-xterm");
      if (check == NULL)
	check = g_find_program_in_path ("rxvt");
      if (check == NULL)
	check = g_find_program_in_path ("xterm");
      if (check == NULL)
	check = g_find_program_in_path ("dtterm");
      if (check == NULL)
	{
	  check = g_strdup ("xterm");
	  g_warning ("couldn't find a terminal, falling back to xterm");
	}
      term_argv[0] = check;
      term_argv[1] = g_strdup ("-e");
    }

  real_argc = term_argc + *argc;
  real_argv = g_new (char *, real_argc + 1);
  
  for (i = 0; i < term_argc; i++)
    real_argv[i] = term_argv[i];
  
  for (j = 0; j < *argc; j++, i++)
    real_argv[i] = (char *)the_argv[j];
  
  real_argv[i] = NULL;
  
  g_free (*argv);
  *argv = real_argv;
  *argc = real_argc;
  
  /* we use g_free here as we sucked all the inner strings
   * out from it into real_argv */
  g_free (term_argv);
  return TRUE;
#else
  return FALSE;
#endif /* G_OS_WIN32 */
}		  


static gboolean
g_desktop_app_info_launch_uris (GAppInfo *appinfo,
				GList *uris,
				char **envp,
				GError **error)
{
  GDesktopAppInfo *info = G_DESKTOP_APP_INFO (appinfo);
  char **argv;
  int argc;
  
  g_return_val_if_fail (appinfo != NULL, FALSE);
  g_return_val_if_fail (uris != NULL, FALSE);
  
  while (uris != NULL)
    {
      if (!expand_application_parameters (info, &uris,
					  &argc, &argv, error))
	return FALSE;
      
      if (info->terminal)
	{
	  if (!prepend_terminal_to_vector (&argc, &argv))
	    {
	      g_strfreev (argv);
	      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Unable to find terminal required for application"));
	      return FALSE;
	    }
	}
		
      if (!g_spawn_async (info->path,  /* working directory */
			  argv,
			  envp,
			  G_SPAWN_SEARCH_PATH /* flags */,
			  NULL /* child_setup */,
			  NULL /* data */,
			  NULL /* child_pid */,
			  error))
	{
	  g_strfreev (argv);
	  return FALSE;
	}
      
      g_strfreev (argv);
    }
  
  return TRUE;
}

static gboolean
g_desktop_app_info_should_show (GAppInfo *appinfo,
				const char *desktop_env)
{
  GDesktopAppInfo *info = G_DESKTOP_APP_INFO (appinfo);
  gboolean found;
  int i;

  if (info->nodisplay)
    return FALSE;

  if (info->only_show_in)
    {
      if (desktop_env == NULL)
	return FALSE;
      
      found = FALSE;
      for (i = 0; info->only_show_in[i] != NULL; i++)
	{
	  if (strcmp (info->only_show_in[i], desktop_env) == 0)
	    {
	      found = TRUE;
	      break;
	    }
	}
      if (!found)
	return FALSE;
    }

  if (info->not_show_in && desktop_env)
    {
      for (i = 0; info->not_show_in[i] != NULL; i++)
	{
	  if (strcmp (info->not_show_in[i], desktop_env) == 0)
	    return FALSE;
	}
    }
  
  return TRUE;
}

static gboolean
ensure_dir (const char *path)
{
  char **split_path;
  char *so_far;
  int i;

  if (g_file_test (path, G_FILE_TEST_IS_DIR))
    return TRUE;
  
  split_path = g_strsplit (path, G_DIR_SEPARATOR_S, 0);
  
  so_far = g_strdup ("/");
  for (i = 0; split_path[i] != NULL; i++)
    {
      char *to_check;
      
      to_check = g_build_filename (so_far, split_path[i], NULL);
      g_free (so_far);
      
      if (!g_file_test (to_check, G_FILE_TEST_IS_DIR))
	{
	  if (mkdir (to_check, S_IRWXU) != 0)
	    {
	      g_free (to_check);
	      g_strfreev (split_path);
	      return FALSE;
	    }
	  
	}
    
      so_far = to_check;
    }
  g_free (so_far);
  g_strfreev (split_path);
  
  return TRUE;
}

static gboolean
g_desktop_app_info_set_as_default_for_type (GAppInfo    *appinfo,
					    const char  *content_type,
					    GError     **error)
{
  GDesktopAppInfo *info = G_DESKTOP_APP_INFO (appinfo);
  char *dirname, *filename;
  GKeyFile *key_file;
  gboolean load_succeeded, res;;
  char **old_list;
  char **list;
  gsize length, data_size;
  char *data;
  int i, j;

  dirname = g_build_filename (g_get_user_data_dir (), "applications", NULL);

  if (!ensure_dir (dirname))
    {
      /* TODO: Should ensure dirname is UTF8 in error message */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Can't create user applications dir (%s)"), dirname);
      g_free (dirname);
      return FALSE;
    }

  filename = g_build_filename (dirname, "defaults.list", NULL);
  g_free (dirname);

  key_file = g_key_file_new ();
  load_succeeded = g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, NULL);
  if (!load_succeeded || !g_key_file_has_group (key_file, "Default Applications"))
    {
      g_key_file_free (key_file);
      key_file = g_key_file_new ();
    }

  old_list = g_key_file_get_string_list (key_file, "Default Applications",
					 content_type, &length, NULL);

  list = g_new (char *, 1 + length + 1);

  i = 0;
  list[i++] = g_strdup (info->desktop_id);
  if (old_list)
    {
      for (j = 0; old_list[j] != NULL; j++)
	{
	  if (strcmp (old_list[j], info->desktop_id) != 0)
	    list[i++] = g_strdup (old_list[j]);
	}
    }
  list[i] = NULL;
  
  g_strfreev (old_list);

  g_key_file_set_string_list (key_file,
			      "Default Applications",
			      content_type,
			      (const char * const *)list, i);

  g_strfreev (list);
  
  data = g_key_file_to_data (key_file, &data_size, error);
  if (data == NULL)
    {
      g_free (filename);
      return FALSE;
    }
  
  res = g_file_set_contents (filename, data, data_size, error);

  mime_info_cache_reload (NULL);
			  
  g_free (filename);
  g_free (data);
  
  return res;
}

static void
g_desktop_app_info_iface_init (GAppInfoIface *iface)
{
  iface->dup = g_desktop_app_info_dup;
  iface->equal = g_desktop_app_info_equal;
  iface->get_name = g_desktop_app_info_get_name;
  iface->get_description = g_desktop_app_info_get_description;
  iface->get_icon = g_desktop_app_info_get_icon;
  iface->launch = g_desktop_app_info_launch;
  iface->supports_uris = g_desktop_app_info_supports_uris;
  iface->launch_uris = g_desktop_app_info_launch_uris;
  iface->should_show = g_desktop_app_info_should_show;
  iface->set_as_default_for_type = g_desktop_app_info_set_as_default_for_type;
}

static gboolean
app_info_in_list (GAppInfo *info, GList *l)
{
  while (l != NULL)
    {
      if (g_app_info_equal (info, l->data))
	return TRUE;
      l = l->next;
    }
  return FALSE;
}

GList *
g_get_all_app_info_for_type (const char *content_type)
{
  GList *desktop_entries, *l;
  GList *infos;
  GAppInfo *info;
  
  desktop_entries = get_all_desktop_entries_for_mime_type (content_type);

  infos = NULL;
  for (l = desktop_entries; l != NULL; l = l->next)
    {
      char *desktop_entry = l->data;

      info = g_desktop_app_info_new (desktop_entry);
      if (info)
	{
	  if (app_info_in_list (info, infos))
	    g_object_unref (info);
	  else
	    infos = g_list_prepend (infos, info);
	}
      g_free (desktop_entry);
    }

  g_list_free (desktop_entries);
  
  return g_list_reverse (infos);
}

GAppInfo *
g_get_default_app_info_for_type (const char *content_type)
{
  GList *desktop_entries, *l;
  GAppInfo *info;
  
  desktop_entries = get_all_desktop_entries_for_mime_type (content_type);

  info = NULL;
  for (l = desktop_entries; l != NULL; l = l->next)
    {
      char *desktop_entry = l->data;

      info = g_desktop_app_info_new (desktop_entry);
      if (info)
	break;
    }

  g_list_foreach  (desktop_entries, (GFunc)g_free, NULL);
  g_list_free (desktop_entries);
  
  return info;
}

static void
get_apps_from_dir (GHashTable *apps, const char *dirname, const char *prefix)
{
  GDir *dir;
  const char *basename;
  char *filename, *subprefix, *desktop_id;
  gboolean hidden;
  GDesktopAppInfo *appinfo;
  
  dir = g_dir_open (dirname, 0, NULL);
  if (dir)
    {
      while ((basename = g_dir_read_name (dir)) != NULL)
	{
	  filename = g_build_filename (dirname, basename, NULL);
	  if (g_str_has_suffix (basename, ".desktop"))
	    {
	      desktop_id = g_strconcat (prefix, basename, NULL);

	      /* Use _extended so we catch NULLs too (hidden) */
	      if (!g_hash_table_lookup_extended (apps, desktop_id, NULL, NULL))
		{
		  appinfo = g_desktop_app_info_new_from_filename (filename, &hidden);

		  /* Don't return apps that don't take arguments */
		  if (appinfo &&
		      strstr (appinfo->exec,"%U") == NULL &&
		      strstr (appinfo->exec,"%u") == NULL &&
		      strstr (appinfo->exec,"%f") == NULL &&
		      strstr (appinfo->exec,"%F") == NULL)
		    {
		      g_object_unref (appinfo);
		      appinfo = NULL;
		      hidden = TRUE;
		    }
				      
		  if (appinfo != NULL || hidden)
		    {
		      g_hash_table_insert (apps, g_strdup (desktop_id), appinfo);

		      if (appinfo)
			{
			  /* Reuse instead of strdup here */
			  appinfo->desktop_id = desktop_id;
			  desktop_id = NULL;
			}
		    }
		}
	      g_free (desktop_id);
	    }
	  else
	    {
	      if (g_file_test (filename, G_FILE_TEST_IS_DIR))
		{
		  subprefix = g_strconcat (prefix, basename, "-", NULL);
		  get_apps_from_dir (apps, filename, subprefix);
		  g_free (subprefix);
		}
	    }
	  g_free (filename);
	}
      g_dir_close (dir);
    }
}

static void
collect_apps (gpointer  key,
	      gpointer  value,
	      gpointer  user_data)
{
  GList **infos = user_data;

  if (value)
    *infos = g_list_prepend (*infos, value);
}


GList *
g_get_all_app_info (void)
{
  const char * const *dirs;
  GHashTable *apps;
  int i;
  GList *infos;

  dirs = get_applications_search_path ();

  apps = g_hash_table_new_full (g_str_hash, g_str_equal,
				g_free, NULL);

  
  for (i = 0; dirs[i] != NULL; i++)
    get_apps_from_dir (apps, dirs[i], "");


  infos = NULL;
  g_hash_table_foreach (apps,
			collect_apps,
			&infos);

  g_hash_table_destroy (apps);

  return g_list_reverse (infos);
}

/* Cacheing of mimeinfo.cache and defaults.list files */

typedef struct {
  char *path;
  GHashTable *mime_info_cache_map;
  GHashTable *defaults_list_map;
  time_t mime_info_cache_timestamp;
  time_t defaults_list_timestamp;
} MimeInfoCacheDir;

typedef struct {
  GList *dirs;                       /* mimeinfo.cache and defaults.list */
  GHashTable *global_defaults_cache; /* global results of defaults.list lookup and validation */
  time_t last_stat_time;
  guint should_ping_mime_monitor : 1;
} MimeInfoCache;

static MimeInfoCache *mime_info_cache = NULL;
G_LOCK_DEFINE_STATIC (mime_info_cache);

static void mime_info_cache_dir_add_desktop_entries (MimeInfoCacheDir *dir,
						     const char *mime_type,
						     char **new_desktop_file_ids);

static MimeInfoCache * mime_info_cache_new (void);

static void
destroy_info_cache_value (gpointer key, GList *value, gpointer data)
{
  g_list_foreach (value, (GFunc)g_free, NULL);
  g_list_free (value);
}

static void
destroy_info_cache_map (GHashTable *info_cache_map)
{
  g_hash_table_foreach (info_cache_map, (GHFunc)destroy_info_cache_value, NULL);
  g_hash_table_destroy (info_cache_map);
}

static gboolean
mime_info_cache_dir_out_of_date (MimeInfoCacheDir *dir,
				 const char *cache_file,
				 time_t *timestamp)
{
  struct stat buf;
  char *filename;
  
  filename = g_build_filename (dir->path, cache_file, NULL);
  
  if (g_stat (filename, &buf) < 0)
    {
      g_free (filename);
      return TRUE;
    }
  g_free (filename);

  if (buf.st_mtime != *timestamp) 
    return TRUE;
  
  return FALSE;
}

/* Call with lock held */
static gboolean
remove_all (gpointer  key,
	    gpointer  value,
	    gpointer  user_data)
{
  return TRUE;
}


static void
mime_info_cache_blow_global_cache (void)
{
  g_hash_table_foreach_remove (mime_info_cache->global_defaults_cache,
			       remove_all, NULL);
}

static void
mime_info_cache_dir_init (MimeInfoCacheDir *dir)
{
  GError *load_error;
  GKeyFile *key_file;
  gchar *filename, **mime_types;
  int i;
  struct stat buf;
  
  load_error = NULL;
  mime_types = NULL;
  
  if (dir->mime_info_cache_map != NULL &&
      !mime_info_cache_dir_out_of_date (dir, "mimeinfo.cache",
					&dir->mime_info_cache_timestamp))
    return;
  
  if (dir->mime_info_cache_map != NULL)
    destroy_info_cache_map (dir->mime_info_cache_map);
  
  dir->mime_info_cache_map = g_hash_table_new_full (g_str_hash, g_str_equal,
						    (GDestroyNotify) g_free,
						    NULL);
  
  key_file = g_key_file_new ();
  
  filename = g_build_filename (dir->path, "mimeinfo.cache", NULL);
  
  if (g_stat (filename, &buf) < 0)
    goto error;
  
  if (dir->mime_info_cache_timestamp > 0) 
    mime_info_cache->should_ping_mime_monitor = TRUE;
  
  dir->mime_info_cache_timestamp = buf.st_mtime;
  
  g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, &load_error);
  
  g_free (filename);
  filename = NULL;
  
  if (load_error != NULL)
    goto error;
  
  mime_types = g_key_file_get_keys (key_file, "MIME Cache",
				    NULL, &load_error);
  
  if (load_error != NULL)
    goto error;
  
  for (i = 0; mime_types[i] != NULL; i++)
    {
      gchar **desktop_file_ids;
      char *unaliased_type;
      desktop_file_ids = g_key_file_get_string_list (key_file,
						     "MIME Cache",
						     mime_types[i],
						     NULL,
						     NULL);
      
      if (desktop_file_ids == NULL)
	continue;

      unaliased_type = _g_unix_content_type_unalias (mime_types[i]);
      mime_info_cache_dir_add_desktop_entries (dir,
					       unaliased_type,
					       desktop_file_ids);
      g_free (unaliased_type);
    
      g_strfreev (desktop_file_ids);
    }
  
  g_strfreev (mime_types);
  g_key_file_free (key_file);
  
  return;
 error:
  if (filename)
    g_free (filename);
  
  g_key_file_free (key_file);
  
  if (mime_types != NULL)
    g_strfreev (mime_types);
  
  if (load_error)
    g_error_free (load_error);
}

static void
mime_info_cache_dir_init_defaults_list (MimeInfoCacheDir *dir)
{
  GKeyFile *key_file;
  GError *load_error;
  gchar *filename, **mime_types;
  char *unaliased_type;
  char **desktop_file_ids;
  int i;
  struct stat buf;

  load_error = NULL;
  mime_types = NULL;

  if (dir->defaults_list_map != NULL &&
      !mime_info_cache_dir_out_of_date (dir, "defaults.list",
					&dir->defaults_list_timestamp))
    return;
  
  if (dir->defaults_list_map != NULL)
    g_hash_table_destroy (dir->defaults_list_map);

  dir->defaults_list_map = g_hash_table_new_full (g_str_hash, g_str_equal,
						  g_free, (GDestroyNotify)g_strfreev);

  key_file = g_key_file_new ();
  
  filename = g_build_filename (dir->path, "defaults.list", NULL);
  if (g_stat (filename, &buf) < 0)
    goto error;

  if (dir->defaults_list_timestamp > 0) 
    mime_info_cache->should_ping_mime_monitor = TRUE;

  dir->defaults_list_timestamp = buf.st_mtime;

  g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, &load_error);
  g_free (filename);
  filename = NULL;

  if (load_error != NULL)
    goto error;

  mime_types = g_key_file_get_keys (key_file, "Default Applications",
				    NULL, &load_error);

  if (load_error != NULL)
    goto error;

  for (i = 0; mime_types[i] != NULL; i++)
    {
      desktop_file_ids = g_key_file_get_string_list (key_file,
						     "Default Applications",
						     mime_types[i],
						     NULL,
						     NULL);
      if (desktop_file_ids == NULL)
	continue;

      unaliased_type = _g_unix_content_type_unalias (mime_types[i]);
      g_hash_table_replace (dir->defaults_list_map,
			    unaliased_type,
			    desktop_file_ids);
    }

  g_strfreev (mime_types);
  g_key_file_free (key_file);
  
  return;
 error:
  if (filename)
    g_free (filename);
  
  g_key_file_free (key_file);
  
  if (mime_types != NULL)
    g_strfreev (mime_types);
  
  if (load_error)
    g_error_free (load_error);
}

static MimeInfoCacheDir *
mime_info_cache_dir_new (const char *path)
{
  MimeInfoCacheDir *dir;

  dir = g_new0 (MimeInfoCacheDir, 1);
  dir->path = g_strdup (path);
  
  return dir;
}

static void
mime_info_cache_dir_free (MimeInfoCacheDir *dir)
{
  if (dir == NULL)
    return;
  
  if (dir->mime_info_cache_map != NULL)
    {
      destroy_info_cache_map (dir->mime_info_cache_map);
      dir->mime_info_cache_map = NULL;
      
  }
  
  if (dir->defaults_list_map != NULL)
    {
      g_hash_table_destroy (dir->defaults_list_map);
      dir->defaults_list_map = NULL;
    }
  
  g_free (dir);
}

static void
mime_info_cache_dir_add_desktop_entries (MimeInfoCacheDir *dir,
					 const char *mime_type,
					 char **new_desktop_file_ids)
{
  GList *desktop_file_ids;
  int i;
  
  desktop_file_ids = g_hash_table_lookup (dir->mime_info_cache_map,
					  mime_type);
  
  for (i = 0; new_desktop_file_ids[i] != NULL; i++)
    {
      if (!g_list_find (desktop_file_ids, new_desktop_file_ids[i]))
	desktop_file_ids = g_list_append (desktop_file_ids,
					  g_strdup (new_desktop_file_ids[i]));
    }
  
  g_hash_table_insert (dir->mime_info_cache_map, g_strdup (mime_type), desktop_file_ids);
}

static void
mime_info_cache_init_dir_lists (void)
{
  const char * const *dirs;
  int i;
  
  mime_info_cache = mime_info_cache_new ();
  
  dirs = get_applications_search_path ();
  
  for (i = 0; dirs[i] != NULL; i++)
    {
      MimeInfoCacheDir *dir;
      
      dir = mime_info_cache_dir_new (dirs[i]);
      
      if (dir != NULL)
	{
	  mime_info_cache_dir_init (dir);
	  mime_info_cache_dir_init_defaults_list (dir);
	  
	  mime_info_cache->dirs = g_list_append (mime_info_cache->dirs, dir);
	}
    }
}

static void
mime_info_cache_update_dir_lists (void)
{
  GList *tmp;
  
  tmp = mime_info_cache->dirs;
  
  while (tmp != NULL)
    {
      MimeInfoCacheDir *dir = (MimeInfoCacheDir *) tmp->data;

      /* No need to do this if we had file monitors... */
      mime_info_cache_blow_global_cache ();
      mime_info_cache_dir_init (dir);
      mime_info_cache_dir_init_defaults_list (dir);
      
      tmp = tmp->next;
    }
}

static void
mime_info_cache_init (void)
{
	G_LOCK (mime_info_cache);
	if (mime_info_cache == NULL)
	  mime_info_cache_init_dir_lists ();
	else
	  {
	    time_t now;
	    
	    time (&now);
	    if (now >= mime_info_cache->last_stat_time + 10)
	      {
		mime_info_cache_update_dir_lists ();
		mime_info_cache->last_stat_time = now;
	      }
	  }

	if (mime_info_cache->should_ping_mime_monitor)
	  {
	    /* g_idle_add (emit_mime_changed, NULL); */
	    mime_info_cache->should_ping_mime_monitor = FALSE;
	  }
	
	G_UNLOCK (mime_info_cache);
}

static MimeInfoCache *
mime_info_cache_new (void)
{
  MimeInfoCache *cache;
  
  cache = g_new0 (MimeInfoCache, 1);
  
  cache->global_defaults_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
							(GDestroyNotify) g_free,
							(GDestroyNotify) g_free);
  return cache;
}

static void
mime_info_cache_free (MimeInfoCache *cache)
{
  if (cache == NULL)
    return;
  
  g_list_foreach (cache->dirs,
		  (GFunc) mime_info_cache_dir_free,
		  NULL);
  g_list_free (cache->dirs);
  g_hash_table_destroy (cache->global_defaults_cache);
  g_free (cache);
}

/**
 * mime_info_cache_reload:
 * @dir: directory path which needs reloading.
 * 
 * Reload the mime information for the @dir.
 */
static void
mime_info_cache_reload (const char *dir)
{
  /* FIXME: just reload the dir that needs reloading,
   * don't blow the whole cache
   */
  if (mime_info_cache != NULL)
    {
      G_LOCK (mime_info_cache);
      mime_info_cache_free (mime_info_cache);
      mime_info_cache = NULL;
      G_UNLOCK (mime_info_cache);
    }
}

static GList *
append_desktop_entry (GList *list, const char *desktop_entry)
{
  /* Add if not already in list, and valid */
  if (!g_list_find_custom (list, desktop_entry, (GCompareFunc) strcmp))
    list = g_list_prepend (list, g_strdup (desktop_entry));
  
  return list;
}

/**
 * get_all_desktop_entries_for_mime_type:
 * @mime_type: a mime type.
 *
 * Returns all the desktop filenames for @mime_type. The desktop files
 * are listed in an order so that default applications are listed before
 * non-default ones, and handlers for inherited mimetypes are listed
 * after the base ones.
 *
 * Return value: a #GList containing the desktop filenames containing the
 * @mime_type.
 */
static GList *
get_all_desktop_entries_for_mime_type (const char *base_mime_type)
{
  GList *desktop_entries, *list, *dir_list, *tmp;
  MimeInfoCacheDir *dir;
  char *mime_type;
  char **mime_types;
  char **default_entries;
  int i,j;
  
  mime_info_cache_init ();

  mime_types = _g_unix_content_type_get_parents (base_mime_type);
  G_LOCK (mime_info_cache);
  
  desktop_entries = NULL;
  for (i = 0; mime_types[i] != NULL; i++)
    {
      mime_type = mime_types[i];

      /* Go through all apps listed as defaults */
      for (dir_list = mime_info_cache->dirs;
	   dir_list != NULL;
	   dir_list = dir_list->next)
	{
	  dir = dir_list->data;
	  default_entries = g_hash_table_lookup (dir->defaults_list_map, mime_type);
	  for (j = 0; default_entries != NULL && default_entries[j] != NULL; j++)
	    desktop_entries = append_desktop_entry (desktop_entries, default_entries[j]);
	}

      /* Go through all entries that support the mimetype */
      for (dir_list = mime_info_cache->dirs;
	   dir_list != NULL;
	   dir_list = dir_list->next) {
	dir = dir_list->data;
	
	list = g_hash_table_lookup (dir->mime_info_cache_map, mime_type);
	for (tmp = list; tmp != NULL; tmp = tmp->next) {
	  desktop_entries = append_desktop_entry (desktop_entries, tmp->data);
	}
      }
    }
  
  G_UNLOCK (mime_info_cache);

  g_strfreev (mime_types);
  
  desktop_entries = g_list_reverse (desktop_entries);
  
  return desktop_entries;
}
