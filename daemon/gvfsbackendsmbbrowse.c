/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsbackendsmbbrowse.h"
#include "gvfsjobmountmountable.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gvfskeyring.h"
#include "gmounttracker.h"
#include "gvfsbackendsmbprivate.h"

#include <libsmbclient.h>

/* The magic "default workgroup" hostname */
#define DEFAULT_WORKGROUP_NAME "X-GNOME-DEFAULT-WORKGROUP"

/* Time in seconds before we mark dirents cache outdated */
#define DEFAULT_CACHE_EXPIRATION_TIME 10

typedef struct {
  unsigned int smbc_type;
  char *name;
  char *name_normalized;
  char *name_utf8;
  char *comment;
} BrowseEntry;

struct _GVfsBackendSmbBrowse
{
  GVfsBackend parent_instance;

  char *user;
  char *domain;
  char *server;
  char *mounted_server; /* server or DEFAULT_WORKGROUP_NAME */
  char *default_workgroup;
  int port;
  SMBCCTX *smb_context;

  char *last_user;
  char *last_domain;
  char *last_password;

  GMountSource *mount_source;
  int mount_try;
  gboolean mount_try_again;
  gboolean mount_cancelled;
  
  gboolean password_in_keyring;
  GPasswordSave password_save;

  GMutex entries_lock;
  GMutex update_cache_lock;
  time_t last_entry_update;
  GList *entries;
};

static GMountTracker *mount_tracker = NULL;

typedef struct {
  char *server_name;
  char *share_name;
  char *domain;
  char *username;
} CachedServer;

G_DEFINE_TYPE (GVfsBackendSmbBrowse, g_vfs_backend_smb_browse, G_VFS_TYPE_BACKEND)

static char *
normalize_smb_name_helper (const char *name, gssize len, gboolean valid_utf8)
{
  if (valid_utf8)
    return g_utf8_casefold (name, len);
  else
    return g_ascii_strdown (name, len);
}

static char *
normalize_smb_name (const char *name, gssize len)
{
  gboolean valid_utf8;

  valid_utf8 = g_utf8_validate (name, len, NULL);
  return normalize_smb_name_helper (name, len, valid_utf8);
}

static char *
smb_name_to_utf8 (const char *name, gboolean *valid_utf8_out)
{
  GString *string;
  const gchar *remainder, *invalid;
  gint remaining_bytes, valid_bytes;
  gboolean valid_utf8;
      
  remainder = name;
  remaining_bytes = strlen (name);
  valid_utf8 = TRUE;
  
  string = g_string_sized_new (remaining_bytes);
  while (remaining_bytes != 0) 
    {
      if (g_utf8_validate (remainder, remaining_bytes, &invalid)) 
	break;
      valid_utf8 = FALSE;
      
      valid_bytes = invalid - remainder;
      
      g_string_append_len (string, remainder, valid_bytes);
      /* append U+FFFD REPLACEMENT CHARACTER */
      g_string_append (string, "\357\277\275");
      
      remaining_bytes -= valid_bytes + 1;
      remainder = invalid + 1;
    }
  
  g_string_append (string, remainder);
  
  if (valid_utf8_out)
    *valid_utf8_out = valid_utf8;
  
  return g_string_free (string, FALSE);
}

static void
browse_entry_free (BrowseEntry *entry)
{
  g_free (entry->name);
  g_free (entry->comment);
  g_free (entry);
}

static void
g_vfs_backend_smb_browse_finalize (GObject *object)
{
  GVfsBackendSmbBrowse *backend;

  backend = G_VFS_BACKEND_SMB_BROWSE (object);

  g_free (backend->user);
  g_free (backend->domain);
  g_free (backend->mounted_server);
  g_free (backend->server);
  g_free (backend->default_workgroup);
  
  g_mutex_clear (&backend->entries_lock);
  g_mutex_clear (&backend->update_cache_lock);

  smbc_free_context (backend->smb_context, TRUE);
  
  g_list_free_full (backend->entries, (GDestroyNotify)browse_entry_free);
  
  if (G_OBJECT_CLASS (g_vfs_backend_smb_browse_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_smb_browse_parent_class)->finalize) (object);
}

static void
g_vfs_backend_smb_browse_init (GVfsBackendSmbBrowse *backend)
{
  char *workgroup;
  GSettings *settings;

  g_mutex_init (&backend->entries_lock);
  g_mutex_init (&backend->update_cache_lock);

  if (mount_tracker == NULL)
    mount_tracker = g_mount_tracker_new (NULL, FALSE);

  /* Get default workgroup name */
  settings = g_settings_new ("org.gnome.system.smb");

  workgroup = g_settings_get_string (settings, "workgroup");
  if (workgroup && workgroup[0])
    backend->default_workgroup = workgroup;
  else
    g_free (workgroup);

  g_object_unref (settings);

  g_debug ("g_vfs_backend_smb_browse_init: default workgroup = '%s'\n", backend->default_workgroup ? backend->default_workgroup : "NULL");
}

/**
 * Authentication callback function type (method that includes context)
 * 
 * Type for the the authentication function called by the library to
 * obtain authentication credentals
 *
 * @param context   Pointer to the smb context
 * @param srv       Server being authenticated to
 * @param shr       Share being authenticated to
 * @param wg        Pointer to buffer containing a "hint" for the
 *                  workgroup to be authenticated.  Should be filled in
 *                  with the correct workgroup if the hint is wrong.
 * @param wglen     The size of the workgroup buffer in bytes
 * @param un        Pointer to buffer containing a "hint" for the
 *                  user name to be use for authentication. Should be
 *                  filled in with the correct workgroup if the hint is
 *                  wrong.
 * @param unlen     The size of the username buffer in bytes
 * @param pw        Pointer to buffer containing to which password 
 *                  copied
 * @param pwlen     The size of the password buffer in bytes
 *           
 */
static void
auth_callback (SMBCCTX *context,
	       const char *server_name, const char *share_name,
	       char *domain_out, int domainmaxlen,
	       char *username_out, int unmaxlen,
	       char *password_out, int pwmaxlen)
{
  GVfsBackendSmbBrowse *backend;
  char *ask_password, *ask_user, *ask_domain;
  gboolean handled, abort;

  backend = smbc_getOptionUserData (context);

  strncpy (password_out, "", pwmaxlen);

  if (backend->domain)
    strncpy (domain_out, backend->domain, domainmaxlen);
  if (backend->user)
    strncpy (username_out, backend->user, unmaxlen);

  if (backend->mount_cancelled)
    {
      /*  Don't prompt for credentials, let smbclient finish the mount loop  */
      strncpy (username_out, "ABORT", unmaxlen);
      strncpy (password_out, "", pwmaxlen);
      g_debug ("auth_callback - mount_cancelled\n");
      return;
    }

  if (backend->mount_source == NULL)
    {
      /* Not during mount, use last password */
      if (backend->last_user)
        strncpy (username_out, backend->last_user, unmaxlen);
      if (backend->last_domain)
        strncpy (domain_out, backend->last_domain, domainmaxlen);
      if (backend->last_password)
        strncpy (password_out, backend->last_password, pwmaxlen);
      return;
    }

  if (backend->mount_try == 0 &&
      backend->user == NULL &&
      backend->domain == NULL)
    {
      /* Try again if kerberos login + anonymous fallback fails */
      backend->mount_try_again = TRUE;
      g_debug ("auth_callback - anonymous pass\n");
    }
  else
    {
      gboolean in_keyring = FALSE;

      g_debug ("auth_callback - normal pass\n");

      if (!backend->password_in_keyring)
        {
	  in_keyring = g_vfs_keyring_lookup_password (backend->user,
						      backend->server,
						      backend->domain,
						      "smb",
						      NULL,
						      NULL,
						      backend->port != -1 ? backend->port : 0,
						      &ask_user,
						      &ask_domain,
						      &ask_password);
	  backend->password_in_keyring = in_keyring;

	  if (in_keyring)
            g_debug ("auth_callback - reusing keyring credentials: user = '%s', domain = '%s'\n",
                     ask_user ? ask_user : "NULL",
                     ask_domain ? ask_domain : "NULL");
	}

      if (!in_keyring)
        {
	  GAskPasswordFlags flags = G_ASK_PASSWORD_NEED_PASSWORD;
	  char *message;

	  if (g_vfs_keyring_is_available ())
	    flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;
	  if (backend->domain == NULL)
	    flags |= G_ASK_PASSWORD_NEED_DOMAIN;
	  if (backend->user == NULL)
	    flags |= G_ASK_PASSWORD_NEED_USERNAME;

          g_debug ("auth_callback - asking for password...\n");

          if (backend->user)
            {
              /* Translators: %s is a server name */
              message = g_strdup_printf (_("Authentication Required\nEnter password for “%s”:"),
                                         server_name);
            }
          else
            {
              /* Translators: %s is a server name */
              message = g_strdup_printf (_("Authentication Required\nEnter user and password for “%s”:"),
                                         server_name);
            }

	  handled = g_mount_source_ask_password (backend->mount_source,
						 message,
						 username_out,
						 domain_out,
						 flags,
						 &abort,
						 &ask_password,
						 &ask_user,
						 &ask_domain,
						 NULL,
						 &(backend->password_save));
	  g_free (message);
	  if (!handled)
	    goto out;

	  if (abort)
	    {
	      strncpy (username_out, "ABORT", unmaxlen);
	      strncpy (password_out, "", pwmaxlen);
	      backend->mount_cancelled = TRUE;
	      goto out;
	    }
	}

      /* Try again if this fails */
      backend->mount_try_again = TRUE;

      strncpy (password_out, ask_password, pwmaxlen);
      if (ask_user && *ask_user)
	strncpy (username_out, ask_user, unmaxlen);
      if (ask_domain && *ask_domain)
	strncpy (domain_out, ask_domain, domainmaxlen);

    out:
      g_free (ask_password);
      g_free (ask_user);
      g_free (ask_domain);
    }

  backend->last_user = g_strdup (username_out);
  backend->last_domain = g_strdup (domain_out);
  backend->last_password = g_strdup (password_out);
  g_debug ("auth_callback - out: last_user = '%s', last_domain = '%s'\n",
           backend->last_user, backend->last_domain);
}

static gboolean
update_cache (GVfsBackendSmbBrowse *backend, SMBCFILE *supplied_dir)
{
  char *uri;
  char dirents[1024*4];
  struct smbc_dirent *dirp;
  GList *entries;
  SMBCFILE *dir;
  int res;
  smbc_opendir_fn smbc_opendir;
  smbc_getdents_fn smbc_getdents;
  smbc_closedir_fn smbc_closedir;


  entries = NULL;
  res = -1;

  g_mutex_lock (&backend->update_cache_lock);
  
  g_debug ("update_cache - updating...\n");
  
  /* Update Cache */

  smbc_opendir = smbc_getFunctionOpendir (backend->smb_context);
  smbc_getdents = smbc_getFunctionGetdents (backend->smb_context);
  smbc_closedir = smbc_getFunctionClosedir (backend->smb_context);

  uri = create_smb_uri (backend->server, backend->port, NULL, NULL);
  dir = supplied_dir ? supplied_dir : smbc_opendir (backend->smb_context, uri);
  g_free (uri);
  if (dir == NULL)
    {
      goto out;
    }

  while (TRUE)
    {
      res = smbc_getdents (backend->smb_context, dir, (struct smbc_dirent *)dirents, sizeof (dirents));
      if (res <= 0)
        {
          if (res < 0)
            g_debug ("update_cache - smbc_getdents returned %d, errno = [%d] %s\n",
                     res, errno, g_strerror (errno));
	  break;
	}  
      
      dirp = (struct smbc_dirent *)dirents;
      while (res > 0)
	{
	  unsigned int dirlen;

	  if (dirp->smbc_type != SMBC_IPC_SHARE &&
	      dirp->smbc_type != SMBC_COMMS_SHARE &&
	      dirp->smbc_type != SMBC_PRINTER_SHARE &&
	      strcmp (dirp->name, ".") != 0 &&
	      strcmp (dirp->name, "..") != 0)
	    {
	      BrowseEntry *entry = g_new (BrowseEntry, 1);
	      gboolean valid_utf8;

	      entry->smbc_type = dirp->smbc_type;
	      entry->name = g_strdup (dirp->name);
	      entry->name_utf8 = smb_name_to_utf8 (dirp->name, &valid_utf8);
	      entry->name_normalized = normalize_smb_name_helper (dirp->name, -1, valid_utf8);
	      entry->comment = smb_name_to_utf8 (dirp->comment, NULL);
	      
	      entries = g_list_prepend (entries, entry);
	    }
		  
	  dirlen = dirp->dirlen;
	  dirp = (struct smbc_dirent *) (((char *)dirp) + dirlen);
	  res -= dirlen;
	}

      entries = g_list_reverse (entries);
    }

  if (! supplied_dir)
    smbc_closedir (backend->smb_context, dir);


 out:

  g_mutex_lock (&backend->entries_lock);
  
  /* Clear old cache */
  g_list_free_full (backend->entries, (GDestroyNotify)browse_entry_free);
  backend->entries = entries;
  backend->last_entry_update = time (NULL);

  g_debug ("update_cache - done.\n");

  g_mutex_unlock (&backend->entries_lock);
  g_mutex_unlock (&backend->update_cache_lock);

  return (res >= 0);
}

static BrowseEntry *
find_entry_unlocked (GVfsBackendSmbBrowse *backend,
		     const char *filename)
{
  BrowseEntry *entry, *found;
  GList *l;
  char *end;
  int len;
  char *normalized;

  while (*filename == '/')
    filename++;

  end = strchr (filename, '/');
  if (end)
    {
      len = end - filename;

      while (*end == '/')
	end++;

      if (*end != 0)
	return NULL;
    }
  else
    len = strlen (filename);

  /* First look for an exact filename match */
  found = NULL;
  for (l = backend->entries; l != NULL; l = l->next)
    {
      entry = l->data;
      
      if (strncmp (filename, entry->name, len) == 0 &&
	  strlen (entry->name) == len)
	{
	  found = entry;
	  break;
	}
    }

  if (found == NULL)
    {
      /* That failed, try normalizing the filename */
      normalized = normalize_smb_name (filename, len);
      
      for (l = backend->entries; l != NULL; l = l->next)
	{
	  entry = l->data;
	  
	  if (strcmp (normalized, entry->name_normalized) == 0)
	    {
	      found = entry;
	      break;
	    }
	}
      g_free (normalized);
    }
  
  return found;
}

static GMountSpec *
get_mount_spec_for_share (const char *server,
			  int port,
			  const char *share)
{
  GMountSpec *mount_spec;
  char *normalized;
  char *port_str;
  
  mount_spec = g_mount_spec_new ("smb-share");
  normalized = normalize_smb_name (server, -1);
  g_mount_spec_set (mount_spec, "server", normalized);
  g_free (normalized);
  normalized = normalize_smb_name (share, -1);
  g_mount_spec_set (mount_spec, "share", normalized);
  g_free (normalized);
  if (port != -1)
    {
      port_str = g_strdup_printf ("%d", port);
      g_mount_spec_set (mount_spec, "port", port_str);
      g_free (port_str);
    }

  return mount_spec;
}

static gboolean
is_root (const char *filename)
{
  const char *p;

  p = filename;
  while (*p == '/')
    p++;

  return *p == 0;
}

static gboolean
has_name (GVfsBackendSmbBrowse *backend,
	  const char *filename)
{
  gboolean res;
  
  g_mutex_lock (&backend->entries_lock);
  res = (find_entry_unlocked (backend, filename) != NULL);
  g_mutex_unlock (&backend->entries_lock);
  return res;
}

static gboolean
cache_needs_updating (GVfsBackendSmbBrowse *backend)
{
  time_t now;
  gboolean res;

  /*  If there's already cache update in progress, lock and wait until update is finished, then recheck  */
  g_mutex_lock (&backend->update_cache_lock);
  now = time (NULL);
  res = now < backend->last_entry_update ||
    (now - backend->last_entry_update) > DEFAULT_CACHE_EXPIRATION_TIME;
  g_mutex_unlock (&backend->update_cache_lock);
  
  return res; 
}

static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);
  SMBCCTX *smb_context;
  SMBCFILE *dir;
  char *display_name;
  const char *debug;
  int debug_val;
  char *icon;
  char *symbolic_icon;
  gchar *port_str;
  char *uri;
  gboolean res;
  GMountSpec *browse_mount_spec;
  smbc_opendir_fn smbc_opendir;
  smbc_closedir_fn smbc_closedir;
  int errsv;
  
  smb_context = smbc_new_context ();
  if (smb_context == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Internal Error (%s)"), "Failed to allocate smb context");
      return;
    }

  smbc_setOptionUserData (smb_context, backend);

  debug = g_getenv ("GVFS_SMB_DEBUG");
  if (debug)
    debug_val = atoi (debug);
  else
    debug_val = 0;

  smbc_setDebug (smb_context, debug_val);
  smbc_setFunctionAuthDataWithContext (smb_context, auth_callback);
  
  if (op_backend->default_workgroup != NULL)
    smbc_setWorkgroup (smb_context, op_backend->default_workgroup);

  /* Initial settings: 
   *   - use Kerberos (always) 
   *   - in case of no username specified, try anonymous login 
   */
  smbc_setOptionUseKerberos (smb_context, 1);
  smbc_setOptionFallbackAfterKerberos (smb_context, op_backend->user != NULL);
  smbc_setOptionNoAutoAnonymousLogin (smb_context, op_backend->user != NULL);

#if 0
  smbc_setOptionDebugToStderr (smb_context, 1);
#endif
  
  if (!smbc_init_context (smb_context))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Internal Error (%s)"), "Failed to initialize smb context");
      smbc_free_context (smb_context, FALSE);
      return;
    }

  op_backend->smb_context = smb_context;

  /* Convert DEFAULT_WORKGROUP_NAME to real domain */
  if (op_backend->mounted_server != NULL &&
      g_ascii_strcasecmp (op_backend->mounted_server, DEFAULT_WORKGROUP_NAME) == 0)
    op_backend->server = g_strdup (smbc_getWorkgroup (smb_context));
  else
    op_backend->server = g_strdup (op_backend->mounted_server);

  icon = NULL;
  symbolic_icon = NULL;
  if (op_backend->server == NULL)
    {
      display_name = g_strdup (_("Windows Network"));
      browse_mount_spec = g_mount_spec_new ("smb-network");
      icon = "network-workgroup";
      symbolic_icon = "network-workgroup-symbolic";
    }
  else
    {
      /* translators: Name for the location that lists the smb shares
	 availible on a server (%s is the name of the server) */
      display_name = g_strdup_printf (_("Windows shares on %s"), op_backend->server);
      browse_mount_spec = g_mount_spec_new ("smb-server");
      g_mount_spec_set (browse_mount_spec, "server", op_backend->mounted_server);
      if (op_backend->port != -1)
        {
          port_str = g_strdup_printf ("%d", op_backend->port);
          g_mount_spec_set (browse_mount_spec, "port", port_str);
          g_free (port_str);
        }
      icon = "network-server";
      symbolic_icon = "network-server-symbolic";
    }

  if (op_backend->user)
    g_mount_spec_set (browse_mount_spec, "user", op_backend->user);
  if (op_backend->domain)
    g_mount_spec_set (browse_mount_spec, "domain", op_backend->domain);
  
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  if (icon)
    g_vfs_backend_set_icon_name (backend, icon);
  if (symbolic_icon)
    g_vfs_backend_set_symbolic_icon_name (backend, symbolic_icon);
  g_vfs_backend_set_user_visible (backend, FALSE);  
  g_vfs_backend_set_mount_spec (backend, browse_mount_spec);
  g_mount_spec_unref (browse_mount_spec);

  op_backend->mount_source = mount_source;
  op_backend->mount_try = 0;
  op_backend->password_save = G_PASSWORD_SAVE_NEVER;

  smbc_opendir = smbc_getFunctionOpendir (smb_context);
  smbc_closedir = smbc_getFunctionClosedir (smb_context);

  uri = create_smb_uri (op_backend->server, op_backend->port, NULL, NULL);

  g_debug ("do_mount - URI = %s\n", uri);

  errsv = 0;

  do
    {
      op_backend->mount_try_again = FALSE;
      op_backend->mount_cancelled = FALSE;

      g_debug ("do_mount - try #%d \n", op_backend->mount_try);

      dir = smbc_opendir (smb_context, uri);

      errsv = errno;
      g_debug ("do_mount - [%s; %d] dir = %p, cancelled = %d, errno = [%d] '%s' \n",
             uri, op_backend->mount_try, dir, op_backend->mount_cancelled,
             errsv, g_strerror (errsv));

      if (errsv == EINVAL && op_backend->mount_try == 0 && op_backend->user == NULL)
        {
          /* EINVAL is "expected" when kerberos is misconfigured, see:
           * https://gitlab.gnome.org/GNOME/gvfs/-/issues/611
           */
        }
      else if (dir == NULL &&
               (op_backend->mount_cancelled || (errsv != EPERM && errsv != EACCES)))
        {
          g_debug ("do_mount - (errno != EPERM && errno != EACCES), cancelled = %d, breaking\n", op_backend->mount_cancelled);
	  break;
	}

      if (dir != NULL)
        {
          /*  Let update_cache() do enumeration, check for the smbc_getdents() result */
          res = update_cache (op_backend, dir);
          smbc_closedir (smb_context, dir);
          g_debug ("do_mount - login successful, res = %d\n", res);
          if (res)
            break;
        }
      else
        {
          /* Purge the cache, we need to have clean playground for next auth try */
          smbc_getFunctionPurgeCachedServers (smb_context)(smb_context);
        }

      /* The first round is Kerberos-only.  Only if this fails do we enable
       * NTLMSSP fallback (turning off anonymous fallback, which we've
       * already tried and failed with).
       */
      if (op_backend->mount_try == 0)
        {
          g_debug ("do_mount - after anon, enabling NTLMSSP fallback\n");
          smbc_setOptionFallbackAfterKerberos (op_backend->smb_context, 1);
          smbc_setOptionNoAutoAnonymousLogin (op_backend->smb_context, 1);
        }
      op_backend->mount_try++;
    }
  while (op_backend->mount_try_again);

  g_free (uri);

  op_backend->mount_source = NULL;

  if (dir == NULL)
    {
      if (op_backend->mount_cancelled)
        g_vfs_job_failed (G_VFS_JOB (job),
                         G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED,
                         _("Password dialog cancelled"));
      else
        g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_FAILED,
			  /* translators: We tried to mount a windows (samba) share, but failed */
			  _("Failed to retrieve share list from server: %s"), g_strerror (errsv));

      return;
    }

  g_vfs_keyring_save_password (op_backend->last_user,
			       op_backend->server,
			       op_backend->last_domain,
			       "smb",
			       NULL,
			       NULL,
			       0,
			       op_backend->last_password,
			       op_backend->password_save);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount (GVfsBackend *backend,
	   GVfsJobMount *job,
	   GMountSpec *mount_spec,
	   GMountSource *mount_source,
	   gboolean is_automount)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);
  const char *server;
  const char *user, *domain;
  const char *port;
  int port_num;

  if (strcmp (g_mount_spec_get_type (mount_spec), "smb-network") == 0)
    server = NULL;
  else
    {
      server = g_mount_spec_get (mount_spec, "server");
      if (server == NULL)
	{
	  g_vfs_job_failed (G_VFS_JOB (job),
			    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			    "No server specified for smb-server share");
	  return TRUE;
	}
    }

  user = g_mount_spec_get (mount_spec, "user");
  domain = g_mount_spec_get (mount_spec, "domain");
  port = g_mount_spec_get (mount_spec, "port");

  if (is_automount &&
      ((user != NULL) || (domain != NULL)))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Can't automount smb browsing with specified user or domain");
      return TRUE;
    }
  
  op_backend->user = g_strdup (user);
  op_backend->domain = g_strdup (domain);
  op_backend->mounted_server = g_strdup (server);
  if (port && (port_num = atoi (port)))
      op_backend->port = port_num;
  else
      op_backend->port = -1;
  
  return FALSE;
}

static void
run_mount_mountable (GVfsBackendSmbBrowse *backend,
		     GVfsJobMountMountable *job,
		     const char *filename,
		     GMountSource *mount_source)
{
  BrowseEntry *entry;
  GError *error = NULL;
  GMountSpec *mount_spec;

  g_mutex_lock (&backend->entries_lock);
  
  entry = find_entry_unlocked (backend, filename);

  if (entry)
    {
      if (backend->server != NULL &&
	  entry->smbc_type == SMBC_FILE_SHARE)
	{
	  mount_spec = get_mount_spec_for_share (backend->server, backend->port, entry->name);
	  g_vfs_job_mount_mountable_set_target (job, mount_spec, "/", TRUE);
	  g_mount_spec_unref (mount_spec);
	}
      else
	g_set_error_literal (&error,
			     G_IO_ERROR, G_IO_ERROR_NOT_MOUNTABLE_FILE,
			     _("Not a mountable file"));
    }
  else
    g_set_error_literal (&error,
			 G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			 _("File doesn’t exist"));
      
  g_mutex_unlock (&backend->entries_lock);

  if (error)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_mount_mountable (GVfsBackend *backend,
		    GVfsJobMountMountable *job,
		    const char *filename,
		    GMountSource *mount_source)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  update_cache (op_backend, NULL);

  run_mount_mountable (op_backend,
		       job,
		       filename,
		       mount_source);
}

static gboolean
try_mount_mountable (GVfsBackend *backend,
		     GVfsJobMountMountable *job,
		     const char *filename,
		     GMountSource *mount_source)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  if (is_root (filename))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_MOUNTABLE_FILE,
			_("Not a mountable file"));
      return TRUE;
    }
  
  if (cache_needs_updating (op_backend))
    return FALSE;

  run_mount_mountable (op_backend,
		       job,
		       filename,
		       mount_source);
  return TRUE;
}

static void 
run_open_for_read (GVfsBackendSmbBrowse *backend,
		    GVfsJobOpenForRead *job,
		    const char *filename)
{
  if (has_name (backend, filename))
    g_vfs_job_failed (G_VFS_JOB (job),
		      G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
		      _("Not a regular file"));
  else
    g_vfs_job_failed (G_VFS_JOB (job),
		      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		      _("File doesn’t exist"));
}

static void
do_open_for_read (GVfsBackend *backend,
	       GVfsJobOpenForRead *job,
	       const char *filename)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  update_cache (op_backend, NULL);

  run_open_for_read (op_backend, job, filename);
}

static gboolean
try_open_for_read (GVfsBackend *backend,
		   GVfsJobOpenForRead *job,
		   const char *filename)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  if (cache_needs_updating (op_backend))
    return FALSE;
 
  run_open_for_read (op_backend, job, filename);

  return TRUE;
}

static gboolean
try_read (GVfsBackend *backend,
	  GVfsJobRead *job,
	  GVfsBackendHandle handle,
	  char *buffer,
	  gsize bytes_requested)
{
  g_vfs_job_failed (G_VFS_JOB (job),
		    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    "Invalid argument");
  
  return TRUE;
}

static gboolean
try_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  g_vfs_job_failed (G_VFS_JOB (job),
		    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    "Invalid argument");
  return TRUE;
}

static gboolean
try_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  g_vfs_job_failed (G_VFS_JOB (job),
		    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    "Invalid argument");
  return TRUE;
}

static void
get_file_info_from_entry (GVfsBackendSmbBrowse *backend, BrowseEntry *entry, GFileInfo *info)
{
  GMountSpec *mount_spec;
  GString *uri;
  GIcon *icon;
  GIcon *symbolic_icon;
  
  g_file_info_set_name (info, entry->name);
  g_file_info_set_display_name (info, entry->name_utf8);
  g_file_info_set_edit_name (info, entry->name_utf8);
  g_file_info_set_attribute_string (info, "smb::comment", entry->comment);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
  g_file_info_set_content_type (info, "inode/directory");

  icon = NULL;
  if (entry->smbc_type == SMBC_WORKGROUP)
    {
      icon = g_themed_icon_new ("network-workgroup");
      symbolic_icon = g_themed_icon_new ("network-workgroup-symbolic");
    }
  else if (entry->smbc_type == SMBC_SERVER)
    {
      icon = g_themed_icon_new ("network-server");
      symbolic_icon = g_themed_icon_new ("network-server-symbolic");
    }
  else
    {
      icon = g_themed_icon_new ("folder-remote");
      symbolic_icon = g_themed_icon_new ("folder-remote-symbolic");
    }

  if (icon)
    {
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
    }
  if (symbolic_icon)
    {
      g_file_info_set_symbolic_icon (info, symbolic_icon);
      g_object_unref (symbolic_icon);
    }
  
  mount_spec = NULL;
  if (backend->server)
    {
      /* browsing server/workgroup */
      if (entry->smbc_type == SMBC_WORKGROUP ||
	  entry->smbc_type == SMBC_SERVER)
	{
	  uri = g_string_new ("smb://");
          g_string_append_uri_escaped (uri, entry->name, NULL, FALSE);
	  g_string_append_c (uri, '/');
	}
      else
	{
	  mount_spec = get_mount_spec_for_share (backend->server, backend->port, entry->name);

	  uri = g_string_new ("smb://");
          g_string_append_uri_escaped (uri, backend->server, NULL, FALSE);
	  g_string_append_c (uri, '/');
          g_string_append_uri_escaped (uri, entry->name, NULL, FALSE);
	}
    }
  else
    {
      /* browsing network */
      uri = g_string_new ("smb://");
      g_string_append_uri_escaped (uri, entry->name, NULL, FALSE);
      g_string_append_c (uri, '/');

      /* these are auto-mounted, so no CAN_MOUNT/UNMOUNT */
    }

  if (mount_spec)
    {
      g_file_info_set_file_type (info, G_FILE_TYPE_MOUNTABLE);
      if (g_mount_tracker_has_mount_spec (mount_tracker, mount_spec))
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, FALSE);
      else
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, FALSE);
      g_mount_spec_unref (mount_spec);
    }
  else
    g_file_info_set_file_type (info, G_FILE_TYPE_SHORTCUT);
    
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, uri->str);
  g_string_free (uri, TRUE);
}

static void
run_query_info (GVfsBackendSmbBrowse *backend,
		GVfsJobQueryInfo *job,
		const char *filename,
		GFileInfo *info,
		GFileAttributeMatcher *matcher)
{
  BrowseEntry *entry;

  g_mutex_lock (&backend->entries_lock);
  
  entry = find_entry_unlocked (backend, filename);

  if (entry)
    get_file_info_from_entry (backend, entry, info);
      
  g_mutex_unlock (&backend->entries_lock);

  if (entry)
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    g_vfs_job_failed (G_VFS_JOB (job),
		      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		      _("File doesn’t exist"));
}

static void
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *matcher)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  update_cache (op_backend, NULL);

  run_query_info (op_backend, job, filename, info, matcher);
}


static gboolean
try_query_info (GVfsBackend *backend,
		GVfsJobQueryInfo *job,
		const char *filename,
		GFileQueryInfoFlags flags,
		GFileInfo *info,
		GFileAttributeMatcher *matcher)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);
  GIcon *icon;

  if (is_root (filename))
    {
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_name (info, "/");
      g_file_info_set_display_name (info, g_vfs_backend_get_display_name (backend));
      g_file_info_set_content_type (info, "inode/directory");
      icon = g_vfs_backend_get_icon (backend);
      if (icon != NULL)
        g_file_info_set_icon (info, icon);
      icon = g_vfs_backend_get_symbolic_icon (backend);
      if (icon != NULL)
        g_file_info_set_symbolic_icon (info, icon);
      g_vfs_job_succeeded (G_VFS_JOB (job));
      
      return TRUE;
    }
  
  if (cache_needs_updating (op_backend))
    return FALSE;

  run_query_info (op_backend, job, filename, info, matcher);
  
  return TRUE;
}

static void
run_enumerate (GVfsBackendSmbBrowse *backend,
	       GVfsJobEnumerate *job,
	       const char *filename,
	       GFileAttributeMatcher *matcher)
{
  GList *files, *l;
  GFileInfo *info;

  if (!is_root (filename))
    {
      if (has_name (backend, filename))
	g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
			  _("Not a directory"));
      else
	g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			  _("File doesn’t exist"));
      return;
    }
  
  /* TODO: limit requested to what we support */
  g_vfs_job_succeeded (G_VFS_JOB (job));

  files = NULL;
  g_mutex_lock (&backend->entries_lock);
  for (l = backend->entries; l != NULL; l = l->next)
    {
      BrowseEntry *entry = l->data;

      info = g_file_info_new ();
      get_file_info_from_entry (backend, entry, info);

      files = g_list_prepend (files, info);
    }
  g_mutex_unlock (&backend->entries_lock);
  
  files = g_list_reverse (files);

  g_vfs_job_enumerate_add_infos (job, files);
  g_list_free_full (files, g_object_unref);

  g_vfs_job_enumerate_done (job);
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      GFileAttributeMatcher *matcher,
	      GFileQueryInfoFlags flags)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  update_cache (op_backend, NULL);

  run_enumerate (op_backend, job, filename, matcher);
}

static gboolean
try_enumerate (GVfsBackend *backend,
	       GVfsJobEnumerate *job,
	       const char *filename,
	       GFileAttributeMatcher *matcher,
	       GFileQueryInfoFlags flags)
{
  GVfsBackendSmbBrowse *op_backend = G_VFS_BACKEND_SMB_BROWSE (backend);

  if (cache_needs_updating (op_backend))
    return FALSE;
  
  run_enumerate (op_backend, job, filename, matcher);

  return TRUE;
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "cifs");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_NEVER);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static void
g_vfs_backend_smb_browse_class_init (GVfsBackendSmbBrowseClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_smb_browse_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->mount_mountable = do_mount_mountable;
  backend_class->try_mount_mountable = try_mount_mountable;
  backend_class->open_for_read = do_open_for_read;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_read = try_read;
  backend_class->try_seek_on_read = try_seek_on_read;
  backend_class->try_close_read = try_close_read;
  backend_class->query_info = do_query_info;
  backend_class->try_query_info = try_query_info;
  backend_class->try_query_fs_info = try_query_fs_info;
  backend_class->enumerate = do_enumerate;
  backend_class->try_enumerate = try_enumerate;
}

void
g_vfs_smb_browse_daemon_init (void)
{
  g_set_application_name (_("Windows Network File System Service"));
}
