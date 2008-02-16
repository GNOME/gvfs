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
#include "soup-output-stream.h"

struct _GVfsBackendDav
{
  GVfsBackendHttp parent_instance;

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

/* ************************************************************************* */
/* generic xml parsing functions */

static inline gboolean
node_has_name (xmlNodePtr node, const char *name)
{
  g_return_val_if_fail (node != NULL, FALSE);

  return ! strcmp ((char *) node->name, name);
}

static inline gboolean
node_has_name_ns (xmlNodePtr node, const char *name, const char *ns_href)
{
  gboolean has_name;
  gboolean has_ns;

  g_return_val_if_fail (node != NULL, FALSE);

  has_name = has_ns = TRUE;

  if (name)
    has_name = node->name && ! strcmp ((char *) node->name, name);

  if (ns_href)
    has_ns = node->ns && node->ns->href &&
      ! g_ascii_strcasecmp ((char *) node->ns->href, ns_href);

  return has_name && has_ns;
}

static inline gboolean
node_is_element (xmlNodePtr node)
{
  return node->type == XML_ELEMENT_NODE && node->name != NULL;
}


static inline gboolean
node_is_element_with_name (xmlNodePtr node, const char *name)
{
  return node->type == XML_ELEMENT_NODE &&
    node->name != NULL && 
    ! strcmp ((char *) node->name, name);
}

static const char *
node_get_content (xmlNodePtr node)
{
    if (node == NULL)
      return NULL;

    switch (node->type)
      {
        case XML_ELEMENT_NODE:
          return node_get_content (node->children);
          break;
        case XML_TEXT_NODE:
          return (const char *) node->content;
          break;
        default:
          return NULL;
      }
    return NULL;
}

typedef struct _xmlNodeIter {

  xmlNodePtr cur_node;
  xmlNodePtr next_node;

  const char *name;
  const char *ns_href;

  void       *user_data;

} xmlNodeIter;

static xmlNodePtr
xml_node_iter_next (xmlNodeIter *iter)
{
  xmlNodePtr node;

  while ((node = iter->next_node))
    {
      iter->next_node = node->next;

      if (node->type == XML_ELEMENT_NODE) {
        if (node_has_name_ns (node, iter->name, iter->ns_href))
          break;
      }
    }

  iter->cur_node = node;
  return node;
}

static void *
xml_node_iter_get_user_data (xmlNodeIter *iter)
{
  return iter->user_data;
}

static xmlNodePtr
xml_node_iter_get_current (xmlNodeIter *iter)
{
  return iter->cur_node;
}

static xmlDocPtr
parse_xml (SoupMessage  *msg,
           xmlNodePtr   *root,
           const char   *name,
           GError      **error)
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
                       XML_PARSE_NOWARNING |
                       XML_PARSE_NOBLANKS |
                       XML_PARSE_NSCLEAN |
                       XML_PARSE_NOCDATA |
                       XML_PARSE_COMPACT);
  if (doc == NULL)
    { 
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", _("Could not parse response"));
      return NULL;
    }

  *root = xmlDocGetRootElement (doc);

  if (*root == NULL || (*root)->children == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", _("Empty response"));
      return NULL;
    }

  if (strcmp ((char *) (*root)->name, name))
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "%s", _("Unexpected reply from server"));
      return NULL;
    }

  return doc;
}

/* ************************************************************************* */
/* Multistatus parsing code */

typedef struct _Multistatus Multistatus;
typedef struct _MsResponse MsResponse;
typedef struct _MsPropstat MsPropstat;

struct _Multistatus {

  xmlDocPtr  doc;
  xmlNodePtr root;

  const SoupURI *target;

};

struct _MsResponse {

  Multistatus *multistatus;

  xmlNodePtr  href;
  xmlNodePtr  first_propstat;
};

struct _MsPropstat {

  Multistatus *multistatus;

  xmlNodePtr   prop_node;
  guint        status_code;

};


static gboolean
multistatus_parse (SoupMessage *msg, Multistatus *multistatus, GError **error)
{
  xmlDocPtr  doc;
  xmlNodePtr root;

  doc = parse_xml (msg, &root, "multistatus", error);

  if (doc == NULL)
    return FALSE;

  multistatus->doc = doc;
  multistatus->root = root;
  multistatus->target = soup_message_get_uri (msg);

  return TRUE;
}

static void
multistatus_free (Multistatus *multistatus)
{
  xmlFreeDoc (multistatus->doc);
}

static void
multistatus_get_response_iter (Multistatus *multistatus, xmlNodeIter *iter)
{
  iter->cur_node = multistatus->root->children;
  iter->next_node = multistatus->root->children;
  iter->name = "response";
  iter->ns_href = "DAV:";
  iter->user_data = multistatus;
}

static gboolean
multistatus_get_response (xmlNodeIter *resp_iter, MsResponse *response)
{
  Multistatus *multistatus;
  xmlNodePtr   resp_node;
  xmlNodePtr   iter;
  xmlNodePtr   href;
  xmlNodePtr   propstat;

  multistatus = xml_node_iter_get_user_data (resp_iter);
  resp_node = xml_node_iter_get_current (resp_iter);

  if (resp_node == NULL)
    return FALSE;

  propstat = NULL;
  href = NULL;

  for (iter = resp_node->children; iter; iter = iter->next)
    {
      if (! node_is_element (iter))
        {
          continue;
        }
      else if (node_has_name_ns (iter, "href", "DAV:"))
        {
          href = iter;
        }
      else if (node_has_name_ns (iter, "propstat", "DAV:"))
        {
          if (propstat == NULL)
            propstat = iter;
        }

      if (href && propstat)
        break;
    }

  if (href == NULL)
    return FALSE;

  response->href = href;
  response->multistatus = multistatus;
  response->first_propstat = propstat;

  return resp_node != NULL;
}

static char *
ms_response_get_basename (MsResponse *response)
{
  const char *text;
  text = node_get_content (response->href);

  return uri_get_basename (text);

}

static gboolean
ms_response_is_target (MsResponse *response)
{
  const char    *text;
  const char    *path;
  const SoupURI *target;
  SoupURI       *uri;
  gboolean       res;

  res = FALSE;
  uri = NULL;
  path = NULL;
  target = response->multistatus->target;
  text = node_get_content (response->href);

  if (text == NULL)
    return FALSE;

  if (*text == '/')
    {
      path = text;
    }
  else if (!g_ascii_strncasecmp (text, "http", 4))
    {
      uri = soup_uri_new (text);
      path = uri->path;
    }

  if (path)
    res = g_str_equal (path, target->path);

  if (uri)
    soup_uri_free (uri);
  
  return res;
}

static void
ms_response_get_propstat_iter (MsResponse *response, xmlNodeIter *iter)
{
  iter->cur_node = response->first_propstat;
  iter->next_node = response->first_propstat;
  iter->name = "propstat";
  iter->ns_href = "DAV:"; 
  iter->user_data = response;
}

static guint
ms_response_get_propstat (xmlNodeIter *cur_node, MsPropstat *propstat)
{
  MsResponse *response;
  xmlNodePtr  pstat_node;
  xmlNodePtr  iter;
  xmlNodePtr  prop;
  xmlNodePtr  status;
  const char *status_text;
  gboolean    res;
  guint       code;

  response = xml_node_iter_get_user_data (cur_node);
  pstat_node = xml_node_iter_get_current (cur_node);

  if (pstat_node == NULL)
    return 0;

  status = NULL;
  prop = NULL;

  for (iter = pstat_node->children; iter; iter = iter->next)
    {
      if (!node_is_element (iter))
        {
          continue;
        }
      else if (node_has_name_ns (iter, "status", "DAV:"))
        {
          status = iter;
        }
      else if (node_has_name_ns (iter, "prop", "DAV:"))
        {
          prop = iter;
        }

      if (status && prop)
        break;
    }

  status_text = node_get_content (status);

  if (status_text == NULL || prop == NULL)
    return 0;

  res = soup_headers_parse_status_line ((char *) status_text,
                                        NULL,
                                        &code,
                                        NULL);

  if (res == FALSE)
    return 0;

  propstat->prop_node = prop;
  propstat->status_code = code;
  propstat->multistatus = response->multistatus;

  return code;
}

static GFileType
parse_resourcetype (xmlNodePtr rt)
{
  xmlNodePtr node;
  GFileType  type;

  for (node = rt->children; node; node = node->next)
    { 
      if (node_is_element (node))
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

static void
ms_response_to_file_info (MsResponse *response,
                          GFileInfo  *info)
{
  xmlNodeIter iter;
  MsPropstat  propstat;
  xmlNodePtr  node;
  guint       status;
  char       *basename;

  basename = ms_response_get_basename (response);
  g_file_info_set_name (info, basename);
  g_file_info_set_edit_name (info, basename);
  g_free (basename);

  ms_response_get_propstat_iter (response, &iter);

  while (xml_node_iter_next (&iter))
    {
      status = ms_response_get_propstat (&iter, &propstat);

      if (! SOUP_STATUS_IS_SUCCESSFUL (status))
        continue;

      for (node = propstat.prop_node->children; node; node = node->next)
        {
          const char *text;
          GTimeVal    tv;

          if (! node_is_element (node))
            continue; /* FIXME: check namespace, parse user data nodes*/

          text = node_get_content (node);

          if (node_has_name (node, "resourcetype"))
            {
              GFileType type = parse_resourcetype (node);
              g_file_info_set_file_type (info, type);
            }
          else if (node_has_name (node, "displayname"))
            {
              g_file_info_set_display_name (info, text);
            }
          else if (node_has_name (node, "getetag"))
            {
              g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE,
                                                text);
            }
          else if (node_has_name (node, "creationdate"))
            {
              if (! g_time_val_from_iso8601 (text, &tv))
                continue;

              g_file_info_set_attribute_uint64 (info,
                                                G_FILE_ATTRIBUTE_TIME_CREATED,
                                                tv.tv_sec);

              g_file_info_set_attribute_uint64 (info,
                                                G_FILE_ATTRIBUTE_TIME_CREATED_USEC,
                                                tv.tv_usec);
            }
          else if (node_has_name (node, "getcontenttype"))
            {
              g_file_info_set_content_type (info, text);
            }
          else if (node_has_name (node, "getcontentlength"))
            {
              gint64 size;
              size = g_ascii_strtoll (text, NULL, 10);
              g_file_info_set_size (info, size);
            }
          else if (node_has_name (node, "getlastmodified"))
            {
              if (g_time_val_from_iso8601 (text, &tv))
                g_file_info_set_modification_time (info, &tv);
            }
        }
    }
}

static GFileType
ms_response_to_file_type (MsResponse *response)
{
  xmlNodeIter prop_iter;
  MsPropstat  propstat;
  GFileType   file_type;
  guint       status;

  file_type = G_FILE_TYPE_UNKNOWN;

  ms_response_get_propstat_iter (response, &prop_iter);
  while (xml_node_iter_next (&prop_iter))
    {
      xmlNodePtr iter;

      status = ms_response_get_propstat (&prop_iter, &propstat);

      if (! SOUP_STATUS_IS_SUCCESSFUL (status))
        continue;

      for (iter = propstat.prop_node->children; iter; iter = iter->next)
        {
          if (node_is_element (iter) &&
              node_has_name_ns (iter, "resourcetype", "DAV:"))
            break;
        }

      if (iter)
        {
          file_type = parse_resourcetype (iter);
          break;
        }
    }

  return file_type;
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

typedef struct _MountOpData {

  gulong        signal_id;

  SoupSession  *session;
  GMountSource *mount_source;

  char         *last_good_path;

     /* for server authentication */
  char *username;
  char *password;
  char *last_realm;

     /* for proxy authentication */
  char *proxy_user;
  char *proxy_password;

} MountOpData;

static void
mount_op_data_free (gpointer _data)
{
  MountOpData *data;

  data = (MountOpData *) _data;
  
  g_free (data->last_good_path);
  
  if (data->mount_source)
    g_object_unref (data->mount_source);
  
  g_free (data->username);
  g_free (data->password);
  g_free (data->last_realm);
  g_free (data->proxy_user);
  g_free (data->proxy_password);

  g_free (data);
}

static void
soup_authenticate (SoupSession *session,
                   SoupMessage *msg,
                   SoupAuth    *auth,
                   gboolean     retrying,
                   gpointer     user_data)
{
  MountOpData *data;
  const char  *username;
  const char  *password;
  gboolean     res;
  gboolean     aborted;
  char        *new_username;
  char        *new_password;
  char        *prompt;

  g_print ("+ soup_authenticate \n");

  data = (MountOpData *) user_data;

  new_username = NULL;
  new_password = NULL;

  if (soup_auth_is_for_proxy (auth))
    {
      username = data->proxy_user;
      password = retrying ? NULL : data->proxy_password;
    }
  else
    {
      username = data->username;
      password = retrying ? NULL : data->password;
    }

  if (username && password)
    {
      soup_auth_authenticate (auth, username, password);
      return;
    }

  if (soup_auth_is_for_proxy (auth))
    {
      prompt = g_strdup (_("Please enter proxy password"));
    }
  else
    {
      const char *auth_realm;

      auth_realm = soup_auth_get_realm (auth);

      if (auth_realm == NULL)
        auth_realm = _("WebDAV share");

      prompt = g_strdup_printf (_("Enter password for %s"), auth_realm);
    }

  soup_session_pause_message (data->session, msg);

  res = g_mount_source_ask_password (data->mount_source,
                                     prompt,
                                     username,
                                     NULL,
                                     G_ASK_PASSWORD_NEED_PASSWORD |
                                     G_ASK_PASSWORD_NEED_USERNAME,
                                     &aborted,
                                     &new_password,
                                     &new_username, 
                                     NULL);
  if (res && !aborted)
    soup_auth_authenticate (auth, new_username, new_password);

  g_free (new_password);
  g_free (new_username);


  g_print ("- soup_authenticate \n");
  g_free (prompt);
}

static void
discover_mount_root_ready (SoupSession *session,
                           SoupMessage *msg,
                           gpointer     user_data)
{
  GVfsBackendDav *backend;
  GVfsJobMount   *job;
  MountOpData    *data;
  GMountSpec     *mount_spec;
  SoupURI        *mount_base;
  gboolean        is_success;
  gboolean        is_dav;

  job = G_VFS_JOB_MOUNT (user_data);
  backend = G_VFS_BACKEND_DAV (job->backend);
  mount_base = G_VFS_BACKEND_HTTP (backend)->mount_base;
  data = (MountOpData *) G_VFS_JOB (job)->backend_data;

  is_success = SOUP_STATUS_IS_SUCCESSFUL (msg->status_code);
  is_dav = sm_has_header (msg, "DAV");
  
  g_print ("+ discover_mount_root_ready \n");

  if (is_success && is_dav)
    {

      data->last_good_path = mount_base->path;
      mount_base->path = path_get_parent_dir (mount_base->path);

      if (mount_base->path)
        {
          SoupMessage *msg;
          msg = message_new_from_uri (SOUP_METHOD_OPTIONS, mount_base);
          soup_session_queue_message (session, msg, 
                                      discover_mount_root_ready, job);
          return;
        } 
    }

  /* we have reached the end of paths we are allowed to
   * chdir up to (or couldn't chdir up at all) */
  
  /* check if we at all have a good path */
  if (data->last_good_path == NULL)
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

  g_free (mount_base->path);
  mount_base->path = data->last_good_path;
  data->last_good_path = NULL;
  mount_spec = g_mount_spec_new ("dav"); 
  
  g_mount_spec_set (mount_spec, "host", mount_base->host);

  if (mount_base->user)
    g_mount_spec_set (mount_spec, "user", mount_base->user);

  if (mount_base->scheme == SOUP_URI_SCHEME_HTTP)
    g_mount_spec_set (mount_spec, "ssl", "false");
  else if (mount_base->scheme == SOUP_URI_SCHEME_HTTPS)
    g_mount_spec_set (mount_spec, "ssl", "true");

  g_mount_spec_set_mount_prefix (mount_spec, mount_base->path);
  g_vfs_backend_set_icon_name (G_VFS_BACKEND (backend), "folder-remote");

  g_vfs_backend_set_mount_spec (G_VFS_BACKEND (backend), mount_spec);
  g_mount_spec_unref (mount_spec);

  g_signal_handler_disconnect (session, data->signal_id);

  g_print ("- discover_mount_root_ready success: %s \n", mount_base->path);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount (GVfsBackend  *backend,
           GVfsJobMount *job,
           GMountSpec   *mount_spec,
           GMountSource *mount_source,
           gboolean      is_automount)
{
  GVfsBackendDav *op_backend;
  MountOpData    *data;
  SoupSession    *session;
  SoupMessage    *msg;
  SoupURI        *uri;
  const char     *host;
  const char     *user;
  const char     *port;
  const char     *ssl;
  guint           port_num;

  g_print ("+ mount\n");

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

  session = G_VFS_BACKEND_HTTP (backend)->session;
  G_VFS_BACKEND_HTTP (backend)->mount_base = uri; 

  msg = message_new_from_uri (SOUP_METHOD_OPTIONS, uri);
  soup_session_queue_message (session, msg, discover_mount_root_ready, job);

  data = g_new0 (MountOpData, 1);
  data->session = g_object_ref (session);
  data->mount_source = g_object_ref (mount_source);
  data->username = g_strdup (user);

  data->signal_id = g_signal_connect (session, "authenticate",
                                      G_CALLBACK (soup_authenticate), data);

  g_vfs_job_set_backend_data (G_VFS_JOB (job), data, mount_op_data_free);

  g_print ("- mount\n");
  return TRUE;
}

static void
query_info_ready (SoupSession *session,
                  SoupMessage *msg,
                  gpointer     user_data)
{
  GVfsBackendDav    *backend;
  GVfsJobQueryInfo  *job;
  Multistatus        ms;
  xmlNodeIter        iter;
  SoupURI           *base;
  gboolean           res;
  GError            *error;
 
  job     = G_VFS_JOB_QUERY_INFO (user_data);
  backend = G_VFS_BACKEND_DAV (job->backend);
  base    = G_VFS_BACKEND_HTTP (backend)->mount_base;
  error   = NULL;

  res = multistatus_parse (msg, &ms, &error);

  if (res == FALSE)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  res = FALSE;
  multistatus_get_response_iter (&ms, &iter);

  while (xml_node_iter_next (&iter))
    {
      MsResponse response;

      if (! multistatus_get_response (&iter, &response))
        continue;

      if (ms_response_is_target (&response))
        {
          ms_response_to_file_info (&response, job->file_info);
          res = TRUE;
        }
    }

  if (res)
        g_vfs_job_succeeded (G_VFS_JOB (job));
  else
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,G_IO_ERROR_FAILED,
                        _("Response invalid"));

  multistatus_free (&ms);
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
  Multistatus        ms;
  xmlNodeIter        iter;
  gboolean           res;
  SoupURI           *base;
  GError            *error;

 
  job     = G_VFS_JOB_ENUMERATE (user_data);
  backend = G_VFS_BACKEND_DAV (job->backend);
  base    = G_VFS_BACKEND_HTTP (backend)->mount_base;
  error   = NULL;
  
  res = multistatus_parse (msg, &ms, &error);

  if (res == FALSE)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  multistatus_get_response_iter (&ms, &iter);

  while (xml_node_iter_next (&iter))
    {
      MsResponse  response;
      const char *basename;
      GFileInfo  *info;

      if (! multistatus_get_response (&iter, &response))
        continue;

      basename = ms_response_get_basename (&response);

      if (ms_response_is_target (&response))
        continue;

      info = g_file_info_new ();
      ms_response_to_file_info (&response, info);
      g_vfs_job_enumerate_add_info (job, info);
    }

  g_vfs_job_succeeded (G_VFS_JOB (job)); /* should that be called earlier? */
  g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
  multistatus_free (&ms);
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

/* *** create () *** */
static void
try_create_tested_existence (SoupSession *session, SoupMessage *msg,
                             gpointer user_data)
{
  GVfsJob *job = G_VFS_JOB (user_data);
  GVfsBackendHttp *op_backend = job->backend_data;
  GOutputStream   *stream;
  SoupMessage     *put_msg;

  if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      g_vfs_job_failed (job,
                        G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
      return;
    }
  /* FIXME: other errors */

  put_msg = message_new_from_uri ("PUT", soup_message_get_uri (msg));

  soup_message_headers_append (put_msg->request_headers, "If-None-Match", "*");
  stream = soup_output_stream_new (op_backend->session, put_msg, -1);
  g_object_unref (put_msg);

  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), stream);
  g_vfs_job_succeeded (job);
}  

static gboolean
try_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  GVfsBackendHttp *op_backend;
  SoupMessage     *msg;

  /* FIXME: if SoupOutputStream supported chunked requests, we could
   * use a PUT with "If-None-Match: *" and "Expect: 100-continue"
   */

  op_backend = G_VFS_BACKEND_HTTP (backend);

  msg = message_new_from_filename (backend, "HEAD", filename);

  g_vfs_job_set_backend_data (G_VFS_JOB (job), op_backend, NULL);
  soup_session_queue_message (op_backend->session, msg,
                              try_create_tested_existence, job);
  return TRUE;
}

/* *** replace () *** */
static void
open_for_replace_succeeded (GVfsBackendHttp *op_backend, GVfsJob *job,
                            SoupURI *uri, const char *etag)
{
  SoupMessage     *put_msg;
  GOutputStream   *stream;

  put_msg = message_new_from_uri (SOUP_METHOD_PUT, uri);

  if (etag)
    soup_message_headers_append (put_msg->request_headers, "If-Match", etag);

  stream = soup_output_stream_new (op_backend->session, put_msg, -1);
  g_object_unref (put_msg);

  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), stream);
  g_vfs_job_succeeded (job);
}

static void
try_replace_checked_etag (SoupSession *session, SoupMessage *msg,
                          gpointer user_data)
{
  GVfsJob *job = G_VFS_JOB (user_data);
  GVfsBackendHttp *op_backend = job->backend_data;

  if (msg->status_code == SOUP_STATUS_PRECONDITION_FAILED)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_WRONG_ETAG,
                        _("The file was externally modified"));
      return;
    }
  /* FIXME: other errors */

  open_for_replace_succeeded (op_backend, job, soup_message_get_uri (msg),
                              soup_message_headers_get (msg->request_headers, "If-Match"));
}  

static gboolean
try_replace (GVfsBackend *backend,
             GVfsJobOpenForWrite *job,
             const char *filename,
             const char *etag,
             gboolean make_backup,
             GFileCreateFlags flags)
{
  GVfsBackendHttp *op_backend;
  SoupURI         *uri;

  /* FIXME: if SoupOutputStream supported chunked requests, we could
   * use a PUT with "If-Match: ..." and "Expect: 100-continue"
   */

  op_backend = G_VFS_BACKEND_HTTP (backend);

  if (make_backup)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Backup file creation failed"));
      return TRUE;
    }



  uri = g_vfs_backend_uri_for_filename (backend, filename);

  if (etag)
    {
      SoupMessage *msg;

      msg = soup_message_new_from_uri (SOUP_METHOD_HEAD, uri);
      soup_uri_free (uri);
      soup_message_headers_append (msg->request_headers, "User-Agent", "gvfs/" VERSION);
      soup_message_headers_append (msg->request_headers, "If-Match", etag);

      g_vfs_job_set_backend_data (G_VFS_JOB (job), op_backend, NULL);
      soup_session_queue_message (op_backend->session, msg,
                                  try_replace_checked_etag, job);
      return TRUE;
    }

  open_for_replace_succeeded (op_backend, G_VFS_JOB (job), uri, NULL);
  soup_uri_free (uri);
  return TRUE;
}

/* *** write () *** */
static void
write_ready (GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
  GOutputStream *stream;
  GVfsJob       *job;
  GError        *error;
  gssize         nwrote;

  stream = G_OUTPUT_STREAM (source_object); 
  error  = NULL;
  job    = G_VFS_JOB (user_data);

  nwrote = g_output_stream_write_finish (stream, result, &error);

  if (nwrote < 0)
   {
     g_vfs_job_failed (G_VFS_JOB (job),
                       error->domain,
                       error->code,
                       error->message);

     g_error_free (error);
     return;
   }

  g_vfs_job_write_set_written_size (G_VFS_JOB_WRITE (job), nwrote);
  g_vfs_job_succeeded (job);
}

static gboolean
try_write (GVfsBackend *backend,
           GVfsJobWrite *job,
           GVfsBackendHandle handle,
           char *buffer,
           gsize buffer_size)
{
  GVfsBackendHttp *op_backend;
  GOutputStream   *stream;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  stream = G_OUTPUT_STREAM (handle);

  g_output_stream_write_async (stream,
                               buffer,
                               buffer_size,
                               G_PRIORITY_DEFAULT,
                               G_VFS_JOB (job)->cancellable,
                               write_ready,
                               job);
  return TRUE;
}

/* *** close_write () *** */
static void
close_write_ready (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  GOutputStream *stream;
  GVfsJob       *job;
  GError        *error;
  gboolean       res;

  job = G_VFS_JOB (user_data);
  stream = G_OUTPUT_STREAM (source_object);
  res = g_output_stream_close_finish (stream,
                                      result,
                                      &error);
  if (res == FALSE)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        error->domain,
                        error->code,
                        error->message);

      g_error_free (error);
    }
  else
    g_vfs_job_succeeded (job);

  g_object_unref (stream);
}

static gboolean
try_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle handle)
{
  GVfsBackendHttp *op_backend;
  GOutputStream   *stream;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  stream = G_OUTPUT_STREAM (handle);

  g_output_stream_close_async (stream,
                               G_PRIORITY_DEFAULT,
                               G_VFS_JOB (job)->cancellable,
                               close_write_ready,
                               job);

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

  backend_class->try_mount         = try_mount;
  backend_class->try_query_info    = try_query_info;
  backend_class->try_enumerate     = try_enumerate;
  backend_class->try_create        = try_create;
  backend_class->try_replace       = try_replace;
  backend_class->try_write         = try_write;
  backend_class->try_close_write   = try_close_write;

}
