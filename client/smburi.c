#include <config.h>
#include <string.h>

#include <gio/giomodule.h>
#include <gvfsurimapper.h>

typedef struct _GVfsUriMapperSmb GVfsUriMapperSmb;
typedef struct _GVfsUriMapperSmbClass GVfsUriMapperSmbClass;

struct _GVfsUriMapperSmb
{
  GVfsUriMapper parent;
};

struct _GVfsUriMapperSmbClass
{
  GVfsUriMapperClass parent_class;
};

static GType g_vfs_uri_mapper_smb_get_type  (GTypeModule *module);
static void g_vfs_uri_mapper_smb_class_init (GVfsUriMapperSmbClass *class);
static void g_vfs_uri_mapper_smb_init (GVfsUriMapperSmb *mapper);


static GType g_vfs_uri_mapper_smb_type = 0;

static GType
g_vfs_uri_mapper_smb_get_type (GTypeModule *module)
{
  if (!g_vfs_uri_mapper_smb_type)
    {
      static const GTypeInfo type_info =
	{
	  sizeof (GVfsUriMapperSmbClass),
	  (GBaseInitFunc) NULL,
	  (GBaseFinalizeFunc) NULL,
	  (GClassInitFunc) g_vfs_uri_mapper_smb_class_init,
	  NULL,           /* class_finalize */
	  NULL,           /* class_data     */
	  sizeof (GVfsUriMapperSmb),
	  0,              /* n_preallocs    */
	  (GInstanceInitFunc) g_vfs_uri_mapper_smb_init
	};

      g_vfs_uri_mapper_smb_type =
        g_type_module_register_type (module, G_VFS_TYPE_URI_MAPPER,
                                     "GVfsUriMapperSmb", &type_info, 0);
    }
  
  return g_vfs_uri_mapper_smb_type;
}

static void
g_vfs_uri_mapper_smb_init (GVfsUriMapperSmb *vfs)
{
}

void
g_io_module_load (GIOModule *module)
{
  g_vfs_uri_mapper_smb_get_type (G_TYPE_MODULE (module));
}

void
g_io_module_unload (GIOModule   *module)
{
}


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

static const char **
smb_get_handled_schemes (GVfsUriMapper *mapper)
{
  static const char *schemes[] = {
    "smb",
    NULL
  };
  return schemes;
}

static gboolean
smb_from_uri (GVfsUriMapper *mapper,
	      const char *uri_str,
	      GMountSpec **spec_out,
	      char **path_out)
{
  GMountSpec *spec;
  char *path, *tmp;
  const char *p;
  const char *share, *share_end;
  GDecodedUri *uri;

  uri = g_decode_uri (uri_str);
  if (uri == NULL)
    return FALSE;
 
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

  return spec != NULL;
}

static const char **
smb_get_handled_mount_types (GVfsUriMapper *mapper)
{
  static const char *types[] = {
    "smb-network", 
    "smb-server", 
    "smb-share",
    NULL
  };
  return types;
}

static char *
smb_to_uri (GVfsUriMapper *mapper,
	    GMountSpec *spec,
	    char *path,
	    gboolean allow_utf8)
{
  const char *type;
  const char *server;
  const char *share;
  char *s;
  GDecodedUri *uri;

  uri = g_new0 (GDecodedUri, 1);
  
  type = g_mount_spec_get_type (spec);

  uri->scheme = g_strdup ("smb");
  
  if (strcmp (type, "smb-network") == 0)
    {
      uri->path = g_strdup (path);
    }
  else if (strcmp (type, "smb-server") == 0)
    {
      server = g_mount_spec_get (spec, "server");
      uri->host = g_strdup (server);

      /* Map the mountables in server to ._share because the actual share mount maps to smb://server/share */
      if (path && path[0] == '/' && path[1] != 0)
	uri->path = g_strconcat ("/._", path + 1, NULL);
      else
	uri->path = g_strdup ("/");
    }
  else if (strcmp (type, "smb-share") == 0)
    {
      server = g_mount_spec_get (spec, "server");
      uri->host = g_strdup (server);
      share = g_mount_spec_get (spec, "share");
      if (path[0] == '/')
	uri->path = g_strconcat ("/", share, path, NULL);
      else
	uri->path = g_strconcat ("/", share, "/", path, NULL);
    }

  s = g_encode_uri (uri, allow_utf8);
  g_decoded_uri_free (uri);
  return s;
}


static void
g_vfs_uri_mapper_smb_class_init (GVfsUriMapperSmbClass *class)
{
  GObjectClass *object_class;
  GVfsUriMapperClass *mapper_class;
  
  object_class = (GObjectClass *) class;

  mapper_class = G_VFS_URI_MAPPER_CLASS (class);
  mapper_class->get_handled_schemes = smb_get_handled_schemes;
  mapper_class->from_uri = smb_from_uri;
  mapper_class->get_handled_mount_types = smb_get_handled_mount_types;
  mapper_class->to_uri = smb_to_uri;
}
