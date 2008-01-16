/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Red Hat, Inc.
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
 * Author: Christian Kellner <gicmo@gnome.org>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <libsoup/soup.h>

/* LibXML2 includes */
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "gvfsbackenddav.h"
#include "gvfsjobmount.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"

#include "soup-input-stream.h"


struct _GVfsBackendDav
{
  GVfsBackendHttp parent_instance;

  /* Only used during mount: */
  char         *last_good_path;
  GMountSource *mount_source;
};

G_DEFINE_TYPE (GVfsBackendDav, g_vfs_backend_dav, G_VFS_TYPE_BACKEND_HTTP);

static void
g_vfs_backend_dav_finalize (GObject *object)
{
  GVfsBackendDav *backend;

  backend = G_VFS_BACKEND_DAV (object);

  if (G_OBJECT_CLASS (g_vfs_backend_dav_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_dav_parent_class)->finalize) (object);
}

static void
g_vfs_backend_dav_init (GVfsBackendDav *backend)
{
}

/* ************************************************************************* */
/*  */

static inline gboolean
sm_has_header (SoupMessage *msg, const char *header)
{
  return soup_message_headers_get (msg->response_headers, header) != NULL;
}

static inline void
send_message (GVfsBackend         *backend,
              SoupMessage         *message, 
              SoupSessionCallback  callback,
              gpointer             user_data)
{

  soup_session_queue_message (G_VFS_BACKEND_HTTP (backend)->session,
                              message,
                              callback, user_data);
}

static char *
path_get_parent_dir (const char *path)
{
    char   *parent;
    size_t  len;

    if ((len = strlen (path)) < 1)
      return NULL;

    /* maybe this should be while, but then again
     * I should be reading the uri rfc and see 
     * what the deal was with multiple slashes */

    if (path[len - 1] == '/')
      len--;

    parent = g_strrstr_len (path, len, "/");

    if (parent == NULL)
      return NULL;

    return g_strndup (path, (parent - path) + 1);
}

static char *
uri_get_basename (const char *uri_str)
{
    const char *parent;
    const char *path;
    char       *to_free;
    char       *basename;
    size_t      len;

    if (uri_str == NULL || *uri_str == '\0')
      return NULL;

    path =  uri_str;

    /* remove any leading slashes */
    while (*path == '/' || *path == ' ')
        path++;

    len = strlen (path);

    if (len == 0)
      return g_strdup ("/");

    /* remove any trailing slashes */
    while (path[len - 1] == '/' || path[len - 1] == ' ')
        len--;

    parent = g_strrstr_len (path, len, "/");

    if (parent)
      {
        parent++; /* skip the found / char */
        to_free = g_strndup (parent, (len - (parent - path)));
      }
    else
      to_free = g_strndup (path, len);

    basename = soup_uri_decode (to_free);
    g_free (to_free);

    return basename;
}

/* ************************************************************************* */
/*  */

static inline gboolean
node_has_name (xmlNodePtr node, const char *name)
{
  g_return_val_if_fail (node != NULL, FALSE);

  return ! strcmp ((char *) node->name, name);
}

static xmlDocPtr
multistatus_parse_xml (SoupMessage *msg, xmlNodePtr *root, GError **error)
{
  xmlDocPtr  doc;

  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("HTTP Error: %s"), msg->reason_phrase);
      return NULL;
    }

  doc = xmlReadMemory (msg->response_body->data,
                       msg->response_body->length,
                       "response.xml",
                       NULL,
                       0);
  if (doc == NULL)
    { 
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Could not parse response"));
      return NULL;
    }

  *root = xmlDocGetRootElement (doc);

  if (doc == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Empty response"));
      return NULL;
    }

  if (strcmp ((char *) (*root)->name, "multistatus"))
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("Unexpected reply from server"));
      return NULL;
    }

  return doc;
}

static GFileType
parse_resourcetype (xmlNodePtr rt)
{
  xmlNodePtr node;
  GFileType  type;

  for (node = rt->children; node; node = node->next)
    { 
      if (node->type == XML_ELEMENT_NODE &&
          node->name != NULL)
          break;
    }

  if (node == NULL)
    return G_FILE_TYPE_REGULAR;

  if (! strcmp ((char *) node->name, "collection"))
    type = G_FILE_TYPE_DIRECTORY;
  else if (! strcmp ((char *) node->name, "redirectref"))
    type = G_FILE_TYPE_SYMBOLIC_LINK;
  else
    type = G_FILE_TYPE_UNKNOWN;

  return type;
}

static GFileInfo *
mulitstatus_parse_prop_node (xmlDocPtr doc, xmlNodePtr prop)
{
  GFileInfo  *info;
  xmlNodePtr  node;

  info = g_file_info_new ();

  for (node = prop->children; node; node = node->next)
    {
     if (node->type != XML_ELEMENT_NODE ||
          node->name == NULL)
        {
          continue;
        }
     else if (!strcmp ((char *) node->name, "resourcetype"))
        {
           GFileType type = parse_resourcetype (node);
           g_file_info_set_file_type (info, type);
        }
     else if (!strcmp ((char *) node->name, "displayname"))
        {
          xmlChar *text;
          text = xmlNodeGetContent (node);
          g_file_info_set_display_name (info, (char *) text);
          xmlFree (text);
        }
     else if (!strcmp ((char *) node->name, "getetag"))
        {

        }
     else if (!strcmp ((char *) node->name, "creationdate"))
        {
          
        }
     else if (!strcmp ((char *) node->name, "getcontenttype"))
        {
          xmlChar *text;
          text = xmlNodeGetContent (node);
          g_file_info_set_content_type (info, (char *) text);
          xmlFree (text);
        }
     else if (!strcmp ((char *) node->name, "getcontentlength"))
        {
          gint64 size; 
          xmlChar *text;
          text = xmlNodeGetContent (node);
          size = g_ascii_strtoll ((char *) text, NULL, 10);
          xmlFree (text);
          g_file_info_set_size (info, size);
        }
    }
  return info;
}

static GFileInfo *
multistatus_parse_response (xmlDocPtr    doc,
                            xmlNodePtr   resp,
                            SoupURI     *base)
{
  GFileInfo  *info;
  xmlNodePtr  node;
  char       *name;
  gboolean    res;
 
  info = NULL;
  name = NULL;

  for (node = resp->children; node; node = node->next)
    {
      if (node->type != XML_ELEMENT_NODE ||
          node->name == NULL)
        {
          continue;
        }
      else if (! strcmp ((char *) node->name, "href"))
        {
          xmlChar *text;

          text = xmlNodeGetContent (node);
          name = uri_get_basename ((char *) text);
          xmlFree (text);
        }

      else if (! strcmp ((char *) node->name, "propstat"))
        {
          xmlNodePtr  iter;
          xmlNodePtr  prop;
          xmlChar    *status_text;
          guint       code;

          status_text = NULL;
          prop = NULL;
        
          for (iter = node->children; iter; iter = iter->next)
            {
                if (node->type != XML_ELEMENT_NODE ||
                    node->name == NULL)
                    continue;
                else if (! strcmp ((char *) iter->name, "status"))
                  {
                    status_text = xmlNodeGetContent (iter);
                  }
                else if (! strcmp ((char *) iter->name, "prop"))
                  {
                    prop = iter;
                  }

                if (status_text && prop)
                  break;
            }

          if (status_text == NULL || prop == NULL)
            {
              if (status_text)
                xmlFree (status_text);

              continue;
            } 

          res = soup_headers_parse_status_line ((char *) status_text,
                                                NULL,
                                                &code,
                                                NULL);
          xmlFree (status_text);

          if (res == FALSE || !SOUP_STATUS_IS_SUCCESSFUL (code))
            continue;

          info = mulitstatus_parse_prop_node (doc, prop); 
        }
    }

  /* after this loop we should have a non-null info object
   * and the right name */
  
  if (info && name)
    {
      g_file_info_set_name (info, name);
      g_file_info_set_edit_name (info, name);
    }
  else
    {
      if (info)
        g_object_unref (info);
      
      g_free (name);
      info = NULL;
    }

  return info;
}

static char *
create_propfind_request (GFileAttributeMatcher *matcher, gulong *size)
{
    xmlOutputBufferPtr   buf;
    xmlNodePtr           node;
    xmlNodePtr           root;
    xmlDocPtr            doc;
    xmlNsPtr             nsdav;
    char                *res;

    doc = xmlNewDoc ((xmlChar *) "1.0");
    root = xmlNewNode (NULL, (xmlChar *) "propfind");
    nsdav = xmlNewNs (root, (xmlChar *) "DAV:", (xmlChar *) "D");
    xmlSetNs (root, nsdav);

    node = xmlNewTextChild (root, nsdav, (xmlChar *) "prop", NULL);

    /* FIXME: we should just ask for properties that 
     * the matcher tells us to ask for
     * nota bene: <D:reftarget/>  */
    xmlNewTextChild (node, nsdav, (xmlChar *) "resourcetype", NULL);
    xmlNewTextChild (node, nsdav, (xmlChar *) "displayname", NULL);
    xmlNewTextChild (node, nsdav, (xmlChar *) "getetag", NULL);
    xmlNewTextChild (node, nsdav, (xmlChar *) "getlastmodified", NULL);
    xmlNewTextChild (node, nsdav, (xmlChar *) "creationdate", NULL);
    xmlNewTextChild (node, nsdav, (xmlChar *) "getcontenttype", NULL);
    xmlNewTextChild (node, nsdav, (xmlChar *) "getcontentlength", NULL);

    buf = xmlAllocOutputBuffer (NULL);
    xmlNodeDumpOutput (buf, doc, root, 0, 1, NULL);
    xmlOutputBufferFlush (buf);

    res = g_strndup ((char *) buf->buffer->content, buf->buffer->use);
    *size = buf->buffer->use;

    xmlOutputBufferClose (buf);
    xmlFreeDoc (doc);
    return res;
}

/* ************************************************************************* */
/*  */
static void
soup_authenticate (SoupSession *session,
                   SoupMessage *msg,
	           SoupAuth    *auth,
                   gboolean     retrying,
                   gpointer     data)
{
  GVfsBackendDav *backend;
  GVfsJobMount   *job;
  SoupURI        *mount_base;
  gboolean        res;
  gboolean        aborted;
  const char     *auth_realm;
  char           *prompt;
  char           *new_password;
  char           *new_user;

  job = G_VFS_JOB_MOUNT (data);
  backend = G_VFS_BACKEND_DAV (job->backend);
  mount_base = G_VFS_BACKEND_HTTP (backend)->mount_base;

  auth_realm = soup_auth_get_realm (auth);

  if (auth_realm == NULL)
    auth_realm = _("WebDAV share");

  prompt = g_strdup_printf (_("Enter password for %s"), auth_realm);

  new_user = new_password = NULL;
  
  res = g_mount_source_ask_password (backend->mount_source,
                                     prompt,
                                     mount_base->user,
                                     NULL,
                                     G_ASK_PASSWORD_NEED_PASSWORD |
                                     G_ASK_PASSWORD_NEED_USERNAME,
                                     &aborted,
                                     &new_password,
                                     &new_user,
                                     NULL);
  if (res && !aborted)
    {
      soup_auth_authenticate (auth, new_user, new_password);
    }

  g_free (new_user);
  g_free (new_password);
  g_free (prompt);
}

static void discover_mount_root_ready (SoupSession *session,
                                       SoupMessage *msg,
                                       gpointer     user_data);
static void
discover_mount_root (GVfsBackendDav *backend, GVfsJobMount *job)
{
  GVfsBackendHttp *http_backend;
  SoupMessage     *msg;
  SoupSession     *session;
  SoupURI         *mount_base;

  http_backend = G_VFS_BACKEND_HTTP (backend);
  mount_base = http_backend->mount_base;
  session = http_backend->session;

  msg = soup_message_new_from_uri (SOUP_METHOD_OPTIONS, mount_base);
  soup_message_headers_append (msg->request_headers, "User-Agent", "gvfs/" VERSION);
  soup_session_queue_message (session, msg, discover_mount_root_ready, job);
}

static void
discover_mount_root_ready (SoupSession *session,
                           SoupMessage *msg,
                           gpointer     user_data)
{
  GVfsBackendDav *backend;
  GVfsJobMount   *job;
  GMountSpec     *mount_spec;
  SoupURI        *mount_base;
  gboolean        is_success;
  gboolean        is_dav;

  job = G_VFS_JOB_MOUNT (user_data);
  backend = G_VFS_BACKEND_DAV (job->backend);
  mount_base = G_VFS_BACKEND_HTTP (backend)->mount_base;

  is_success = SOUP_STATUS_IS_SUCCESSFUL (msg->status_code);
  is_dav = sm_has_header (msg, "DAV");
  
  g_print ("+ discover_mount_root_ready \n");

  if (is_success && is_dav)
    {
      backend->last_good_path = mount_base->path;
      mount_base->path = path_get_parent_dir (mount_base->path);

      if (mount_base->path)
        {
          discover_mount_root (backend, job);
          return;
        } 
    }

  /* we have reached the end of paths we are allowed to
   * chdir up to (or couldn't chdir up at all) */
  
  /* check if we at all have a good path */
  if (backend->last_good_path == NULL)
    {
      if (!is_success) 
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,G_IO_ERROR_FAILED,
                          _("HTTP Error: %s"), msg->reason_phrase);
      else
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,G_IO_ERROR_FAILED,
                          _("Not a WebDAV enabled share"));

      return;
    }

  mount_spec = g_mount_spec_new ("dav"); 
  g_mount_spec_set (mount_spec, "host", mount_base->host);

  if (mount_base->user)
    g_mount_spec_set (mount_spec, "user", mount_base->user);

  if (mount_base->scheme == SOUP_URI_SCHEME_HTTP)
    g_mount_spec_set (mount_spec, "ssl", "false");
  else if (mount_base->scheme == SOUP_URI_SCHEME_HTTPS)
    g_mount_spec_set (mount_spec, "ssl", "true");

  g_free (mount_base->path);
  mount_base->path = backend->last_good_path;

  g_mount_spec_set_mount_prefix (mount_spec, mount_base->path);
  g_vfs_backend_set_mount_spec (G_VFS_BACKEND (backend), mount_spec);
  g_vfs_backend_set_icon_name (G_VFS_BACKEND (backend), "folder-remote");

  g_mount_spec_unref (mount_spec);
  g_print ("- discover_mount_root_ready success: %s \n", mount_base->path);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}



static void
mount (GVfsBackend  *backend,
       GVfsJobMount *job,
       GMountSpec   *mount_spec,
       GMountSource *mount_source,
       gboolean      is_automount)
{
  GVfsBackendDav *op_backend;
  SoupSession    *session;

  g_print ("+ mount\n");

  op_backend = G_VFS_BACKEND_DAV (backend);
  session = G_VFS_BACKEND_HTTP (backend)->session;

  g_signal_connect (session, "authenticate",
                    G_CALLBACK (soup_authenticate), job);

  op_backend->mount_source = mount_source;
  discover_mount_root (op_backend, job);
  g_print ("- mount\n");
}

static gboolean
try_mount (GVfsBackend  *backend,
           GVfsJobMount *job,
           GMountSpec   *mount_spec,
           GMountSource *mount_source,
           gboolean      is_automount)
{
  GVfsBackendDav *op_backend;
  SoupURI        *uri;
  const char     *host;
  const char     *user;
  const char     *port;
  const char     *ssl;
  guint           port_num;

  /* We have to override this method since the http backend
   * has its own try_mount and we don't want that to be used.
   * We also need to do the dav mounting stuff inside a
   * thread since the auth callback from soup is getting
   * called in the main thread otherwise and we would block */

  op_backend = G_VFS_BACKEND_DAV (backend);

  host = g_mount_spec_get (mount_spec, "host");
  user = g_mount_spec_get (mount_spec, "user");
  port = g_mount_spec_get (mount_spec, "port");
  ssl  = g_mount_spec_get (mount_spec, "ssl");
  
  if (host == NULL || *host == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Invalid mount spec"));
      
      return TRUE;
    }

  uri = soup_uri_new (NULL);

  if (ssl != NULL && (strcmp (ssl, "true") == 0))
    soup_uri_set_scheme (uri, SOUP_URI_SCHEME_HTTPS);
  else
    soup_uri_set_scheme (uri, SOUP_URI_SCHEME_HTTP);

  soup_uri_set_user (uri, user);

  if (port && (port_num = atoi (port)))
    soup_uri_set_port (uri, port_num);

  soup_uri_set_host (uri, host);
  soup_uri_set_path (uri, mount_spec->mount_prefix);

  G_VFS_BACKEND_HTTP (backend)->mount_base = uri; 

  return FALSE;
}

static void
query_info_ready (SoupSession *session,
                  SoupMessage *msg,
                  gpointer     user_data)
{
  GVfsBackendDav    *backend;
  GVfsJobQueryInfo  *job;
  SoupURI           *base;
  GFileInfo         *info;
  GError            *error;
  xmlDocPtr          doc;
  xmlNodePtr         root;
  xmlNodePtr         node;
 
  job     = G_VFS_JOB_QUERY_INFO (user_data);
  backend = G_VFS_BACKEND_DAV (job->backend);
  base    = G_VFS_BACKEND_HTTP (backend)->mount_base;
  error   = NULL;
  info    = NULL;

  doc = multistatus_parse_xml (msg, &root, &error);

  if (doc == NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  for (node = root->children; node; node = node->next)
    {
      if (node->type != XML_ELEMENT_NODE ||
          node->name == NULL ||
          strcmp ((char *) node->name, "response"))
        continue;

      info = multistatus_parse_response (doc, node, base);
    }

  if (info)
    {
        g_file_info_copy_into (info, job->file_info);
        g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,G_IO_ERROR_FAILED,
                        _("Response invalid"));
    }

  xmlFreeDoc (doc);
}

/* *** query_info () *** */
static gboolean
try_query_info (GVfsBackend           *backend,
                GVfsJobQueryInfo      *job,
                const char            *filename,
                GFileQueryInfoFlags    flags,
                GFileInfo             *info,
                GFileAttributeMatcher *attribute_matcher)
{
  SoupMessage *msg;
  gulong       len;
  char        *request;
  char        *redirect_header;

  len = 0;
  msg = message_new_from_filename (backend, "PROPFIND", filename);

  request = create_propfind_request (attribute_matcher, &len);

  if (request == NULL || msg == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,G_IO_ERROR_FAILED,
                        _("Could not create request"));
      

      if (msg)
          g_object_unref (msg);

      g_free (request);
      return TRUE;
    }

  soup_message_headers_append (msg->request_headers, "Depth", "0");

  /* RFC 4437 */
  if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
      redirect_header = "F";
  else
      redirect_header = "T";

  soup_message_headers_append (msg->request_headers,
                           "Apply-To-Redirect-Ref", redirect_header);

  soup_message_set_request (msg, "application/xml",
                            SOUP_MEMORY_TAKE,
                            request,
                            len);

  send_message (backend, msg, query_info_ready, job);
  return TRUE;
}

/* *** enumerate *** */



static void
enumerate_ready (SoupSession *session,
                 SoupMessage *msg,
                 gpointer     user_data)
{
  GVfsBackendDav    *backend;
  GVfsJobEnumerate  *job;
  SoupURI           *base;
  GFileInfo         *info;
  GError            *error;
  xmlDocPtr          doc;
  xmlNodePtr         root;
  xmlNodePtr         node;
 
  job     = G_VFS_JOB_ENUMERATE (user_data);
  backend = G_VFS_BACKEND_DAV (job->backend);
  base    = G_VFS_BACKEND_HTTP (backend)->mount_base;
  error   = NULL;
  info    = NULL;

  doc = multistatus_parse_xml (msg, &root, &error);

  if (doc == NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  for (node = root->children; node; node = node->next)
    {
      GFileInfo *info;
      const char *fn;

      if (node->type != XML_ELEMENT_NODE ||
          node->name == NULL ||
          strcmp ((char *) node->name, "response"))
        continue;

      info = multistatus_parse_response (doc, node, base);

      if (info == NULL)
        continue;
      
      fn = g_file_info_get_name (info);

      if (fn == NULL || g_str_equal (job->filename, fn))
        {
          g_object_unref (info);
          continue;
        }

        g_vfs_job_enumerate_add_info (job, info);
    }

  g_vfs_job_succeeded (G_VFS_JOB (job)); /* should that be called earlier? */
  g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
  xmlFreeDoc (doc);
}

static gboolean
try_enumerate (GVfsBackend           *backend,
               GVfsJobEnumerate      *job,
               const char            *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags    flags)
{
  SoupMessage *msg;
  gulong       len;
  char        *request;
  char        *redirect_header;

  len = 0;
  msg = message_new_from_filename (backend, "PROPFIND", filename);

  request = create_propfind_request (attribute_matcher, &len);

  if (request == NULL || msg == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,G_IO_ERROR_FAILED,
                        _("Could not create request"));
      

      if (msg)
          g_object_unref (msg);

      g_free (request);
      return TRUE;
    }

  soup_message_headers_append (msg->request_headers, "Depth", "1");

  /* RFC 4437 */
  if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
      redirect_header = "F";
  else
      redirect_header = "T";

  soup_message_headers_append (msg->request_headers,
                           "Apply-To-Redirect-Ref", redirect_header);

  soup_message_set_request (msg, "application/xml",
                            SOUP_MEMORY_TAKE,
                            request,
                            len);

  send_message (backend, msg, enumerate_ready, job);
  return TRUE;
}

/* ************************************************************************* */
/*  */

static void
g_vfs_backend_dav_class_init (GVfsBackendDavClass *klass)
{
  GObjectClass     *gobject_class;
  GVfsBackendClass *backend_class;
  
  gobject_class = G_OBJECT_CLASS (klass); 
  gobject_class->finalize  = g_vfs_backend_dav_finalize;

  backend_class = G_VFS_BACKEND_CLASS (klass); 
  backend_class->mount             = mount;
  backend_class->try_mount         = try_mount;
  backend_class->try_query_info    = try_query_info;
  backend_class->try_enumerate     = try_enumerate;
}
