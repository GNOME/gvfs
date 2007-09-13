#include <config.h>
#include <string.h>

#include <gvfsmapuri.h>


static char *
normalize_smb_name (const char *name, gssize len)
{
  gboolean valid_utf8;

  valid_utf8 = g_utf8_validate (name, len, NULL);
  
  if (valid_utf8)
    return g_utf8_casefold (name, len);
  else
    return g_ascii_strdown (name, len);
}

static void
smb_from_uri (GDecodedUri *uri,
	      GMountSpec **spec_out,
	      char **path_out)
{
  GMountSpec *spec;
  char *path, *tmp;
  const char *p;
  const char *share, *share_end;
  
  if (uri->host == NULL || strlen (uri->host) == 0)
    {
      /* uri form: smb:/// or smb:///$path */
      spec = g_mount_spec_new ("smb-network");
      if (uri->path == NULL || *uri->path == 0)
	path = g_strdup ("/");
      else
	path = g_strdup (uri->path);
    }
  else
    {
      /* host set */
      p = uri->path;
      while (p && *p == '/')
	p++;
      
      if (p == NULL || *p == 0)
	{
	  /* uri form: smb://$host/ */
	  spec = g_mount_spec_new ("smb-server");
	  tmp = normalize_smb_name (uri->host, -1);
	  g_mount_spec_set  (spec, "server", tmp);
	  g_free (tmp);
	  path = g_strdup ("/");
	}
      else
	{
	  share = p;
	  share_end = strchr (share, '/');
	  if (share_end == NULL)
	    share_end = share + strlen (share);
	  
	  p = share_end;
	  
	  while (*p == '/')
	    p++;
	  
	  if (*p == 0)
	    {
	      /* uri form: smb://$host/$share/
	       * Here we special case smb-server files by adding "._" to the names in the uri */
	      if (share[0] == '.' && share[1] == '_')
		{
		  spec = g_mount_spec_new ("smb-server");
		  tmp = normalize_smb_name (uri->host, -1);
		  g_mount_spec_set  (spec, "server", tmp);
		  g_free (tmp);
		  tmp = normalize_smb_name (share + 2, share_end - (share + 2));
		  path = g_strconcat ("/", tmp, NULL);
		  g_free (tmp);
		}
	      else
		{
		  spec = g_mount_spec_new ("smb-share");
		  tmp = normalize_smb_name (uri->host, -1);
		  g_mount_spec_set  (spec, "server", tmp);
		  g_free (tmp);
		  tmp = normalize_smb_name (share, share_end - share);
		  g_mount_spec_set  (spec, "share", tmp);
		  g_free (tmp);
		  path = g_strdup ("/");
		}
	    }
	  else
	    {
	      spec = g_mount_spec_new ("smb-share");
	      
	      tmp = normalize_smb_name (uri->host, -1);
	      g_mount_spec_set  (spec, "server", tmp);
	      g_free (tmp);
	      
	      tmp = normalize_smb_name (share, share_end - share);
	      g_mount_spec_set  (spec, "share", tmp);
	      g_free (tmp);
	      
	      path = g_strconcat ("/", p, NULL);
	    }
	}
    }
  
  if (uri->userinfo)
    {
      const char *user = uri->userinfo;
      p = strchr (uri->userinfo, ';');
      if (p)
	{
	  if (p != user)
	    g_mount_spec_set_with_len  (spec, "domain", user, p - user);
	  user = p + 1;
	}
      if (*user != 0)
	g_mount_spec_set  (spec, "user", user);
    }

  *spec_out = spec;
  *path_out = path;
}

static void
smb_to_uri (GMountSpec *spec,
	    char *path,
	    GDecodedUri *uri_out)
{
  const char *type;
  const char *server;
  const char *share;

  type = g_mount_spec_get_type (spec);

  uri_out->scheme = g_strdup ("smb");
  
  if (strcmp (type, "smb-network") == 0)
    {
      uri_out->path = g_strdup (path);
    }
  else if (strcmp (type, "smb-server") == 0)
    {
      server = g_mount_spec_get (spec, "server");
      uri_out->host = g_strdup (server);

      /* Map the mountables in server to ._share because the actual share mount maps to smb://server/share */
      if (path && path[0] == '/' && path[1] != 0)
	uri_out->path = g_strconcat ("/._", path + 1, NULL);
      else
	uri_out->path = g_strdup ("/");
    }
  else if (strcmp (type, "smb-share") == 0)
    {
      server = g_mount_spec_get (spec, "server");
      uri_out->host = g_strdup (server);
      share = g_mount_spec_get (spec, "share");
      if (path[0] == '/')
	uri_out->path = g_strconcat ("/", share, path, NULL);
      else
	uri_out->path = g_strconcat ("/", share, "/", path, NULL);
    }
}

GVfsMapFromUri G_VFS_MAP_FROM_URI_TABLE_NAME [] = {
  { "smb", smb_from_uri },
  { NULL }
};

GVfsMapToUri G_VFS_MAP_TO_URI_TABLE_NAME [] = {
  { "smb-network", smb_to_uri },
  { "smb-server", smb_to_uri },
  { "smb-share", smb_to_uri },
  { NULL }
};
