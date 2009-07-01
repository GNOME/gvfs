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
#include "gvfskeyring.h"

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

#ifdef HAVE_AVAHI
#include "gvfsdnssdutils.h"
#include "gvfsdnssdresolver.h"
#endif

typedef struct _MountAuthData MountAuthData;

static void mount_auth_info_free (MountAuthData *info);


#ifdef HAVE_AVAHI
static void dns_sd_resolver_changed  (GVfsDnsSdResolver *resolver, GVfsBackendDav *dav_backend);
#endif

typedef struct _AuthInfo {

   /* for server authentication */
    char          *username;
    char          *password;
    char          *realm;

    GPasswordSave  pw_save;

} AuthInfo;

struct _MountAuthData {

  SoupSession  *session;
  GMountSource *mount_source;

  AuthInfo server_auth;
  AuthInfo proxy_auth;

};

struct _GVfsBackendDav
{
  GVfsBackendHttp parent_instance;

  MountAuthData auth_info;

#ifdef HAVE_AVAHI
  /* only set if we're handling a [dav|davs]+sd:// mounts */
  GVfsDnsSdResolver *resolver;
#endif
};

G_DEFINE_TYPE (GVfsBackendDav, g_vfs_backend_dav, G_VFS_TYPE_BACKEND_HTTP);

static void
g_vfs_backend_dav_finalize (GObject *object)
{
  GVfsBackendDav *dav_backend;

  dav_backend = G_VFS_BACKEND_DAV (object);

#ifdef HAVE_AVAHI
  if (dav_backend->resolver != NULL)
    {
      g_signal_handlers_disconnect_by_func (dav_backend->resolver, dns_sd_resolver_changed, dav_backend);
      g_object_unref (dav_backend->resolver);
    }
#endif

  mount_auth_info_free (&(dav_backend->auth_info));
  
  if (G_OBJECT_CLASS (g_vfs_backend_dav_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_dav_parent_class)->finalize) (object);
}

static void
g_vfs_backend_dav_init (GVfsBackendDav *backend)
{
  g_vfs_backend_set_user_visible (G_VFS_BACKEND (backend), TRUE);
}

/* ************************************************************************* */
/* Small utility functions */

static inline gboolean
sm_has_header (SoupMessage *msg, const char *header)
{
  return soup_message_headers_get (msg->response_headers, header) != NULL;
}

static char *
path_get_parent_dir (const char *path)
{
  char   *parent;
  size_t  len;

  len = strlen (path);

  while (len > 0 && path[len - 1] == '/')
    len--;

  if (len == 0)
    return NULL;

  parent = g_strrstr_len (path, len, "/");

  if (parent == NULL)
    return NULL;

  return g_strndup (path, (parent - path) + 1);
}

/* message utility functions */

static void
message_add_destination_header (SoupMessage *msg,
                                SoupURI     *uri)
{
  char *string;

  string = soup_uri_to_string (uri, FALSE);
  soup_message_headers_append (msg->request_headers,
                               "Destination",
                               string);
  g_free (string);
}
static void
message_add_overwrite_header (SoupMessage *msg,
                              gboolean     overwrite)
{
  soup_message_headers_append (msg->request_headers,
                               "Overwrite",
                               overwrite ? "T" : "F");
}

static void
message_add_redirect_header (SoupMessage         *msg,
                             GFileQueryInfoFlags  flags)
{
  const char  *header_redirect;

  /* RFC 4437 */
  if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
      header_redirect = "F";
  else
      header_redirect = "T";

  soup_message_headers_append (msg->request_headers,
                               "Apply-To-Redirect-Ref",
                               header_redirect);
}

static inline gboolean
str_equal (const char *a, const char *b, gboolean insensitive)
{
   if (a == NULL || b == NULL)
      return a == b;

   return insensitive ? !g_ascii_strcasecmp (a, b) : !strcmp (a, b);
}

static gboolean
path_equal (const char *a, const char *b, gboolean relax)
{
  gboolean res;
  size_t a_len, b_len;

  if (relax == FALSE)
    return str_equal (a, b, FALSE);

  if (a == NULL || b == NULL)
      return a == b;

  a_len = strlen (a);
  b_len = strlen (b);

  while (a_len > 0 && a[a_len - 1] == '/')
    a_len--;

  while (b_len > 0 && b[b_len - 1] == '/')
    b_len--;

  if (a_len == b_len)
    res = ! strncmp (a, b, a_len);
  else
    res = FALSE;

  return res;
}

/* Like soup_uri_equal */
static gboolean
dav_uri_match (SoupURI *a, SoupURI *b, gboolean relax)
{
  if (a->scheme != b->scheme ||
      a->port != b->port     ||
      ! str_equal (a->user, b->user, FALSE)         ||
      ! str_equal (a->password, b->password, FALSE) ||
      ! str_equal (a->host, b->host, TRUE)          ||
      ! path_equal (a->path, b->path, relax)        ||
      ! str_equal (a->query, b->query, FALSE)       ||
      ! str_equal (a->fragment, b->fragment, FALSE))
    return FALSE;
  return TRUE;
}

static gboolean
message_should_apply_redir_ref (SoupMessage *msg)
{
  const char *header;

  header = soup_message_headers_get (msg->request_headers,
                                     "Apply-To-Redirect-Ref");

  if (header == NULL || g_ascii_strcasecmp (header, "T"))
    return FALSE;

  return TRUE;
}

/* redirection */
static void
redirect_handler (SoupMessage *msg, gpointer user_data)
{
    SoupSession *session = user_data;
    const char  *new_loc;
    SoupURI     *new_uri;
    SoupURI     *old_uri;
    guint        status;
    gboolean     redirect;

    status = msg->status_code;
    new_loc = soup_message_headers_get (msg->response_headers, "Location");

    /* If we don't have a location to redirect to, just fail */
    if (new_loc == NULL)
      return;

    if (!SOUP_STATUS_IS_REDIRECTION(status))
      return;

   new_uri = soup_uri_new_with_base (soup_message_get_uri (msg), new_loc);
   if (new_uri == NULL)
     {
       soup_message_set_status_full (msg,
                                     SOUP_STATUS_MALFORMED,
                                     "Invalid Redirect URL");
       return;
     }

   old_uri = soup_message_get_uri (msg);

   /* copy over username and password to new_uri */
   soup_uri_set_user(new_uri, old_uri->user);
   soup_uri_set_password(new_uri, old_uri->password);

   /* Check if this is a trailing slash redirect (i.e. /a/b to /a/b/),
    * redirect it right away
    */
   redirect = dav_uri_match (new_uri, old_uri, TRUE);

   if (redirect == TRUE)
     {
       const char *dest;

       dest = soup_message_headers_get (msg->request_headers,
                                        "Destination");

       if (dest && g_str_has_suffix (dest, "/") == FALSE)
         {
           char *new_dest = g_strconcat (dest, "/", NULL);
           soup_message_headers_replace (msg->request_headers,
                                         "Destination",
                                         new_dest);
           g_free (new_dest);
         }
     }
   else if (message_should_apply_redir_ref (msg))
     {


       if (status == SOUP_STATUS_MOVED_PERMANENTLY ||
           status == SOUP_STATUS_TEMPORARY_REDIRECT)
         {

           /* Only corss-site redirect safe methods */
           if (msg->method == SOUP_METHOD_GET &&
               msg->method == SOUP_METHOD_HEAD &&
               msg->method == SOUP_METHOD_OPTIONS &&
               msg->method == SOUP_METHOD_PROPFIND)
             redirect = TRUE;
         }

#if 0
       else if (msg->status_code == SOUP_STATUS_SEE_OTHER ||
                msg->status_code == SOUP_STATUS_FOUND)
         {
           /* Redirect using a GET */
           g_object_set (msg,
                         SOUP_MESSAGE_METHOD, SOUP_METHOD_GET,
                         NULL);
           soup_message_set_request (msg, NULL,
                                     SOUP_MEMORY_STATIC, NULL, 0);
           soup_message_headers_set_encoding (msg->request_headers,
                                              SOUP_ENCODING_NONE);
         }
#endif
         /* ELSE:
          *
          * Two possibilities:
          *
          *   1) It's a non-redirecty 3xx response (300, 304,
          *      305, 306)
          *   2) It's some newly-defined 3xx response (308+)
          *
          * We ignore both of these cases. In the first,
          * redirecting would be explicitly wrong, and in the
          * last case, we have no clue if the 3xx response is
          * supposed to be redirecty or non-redirecty. Plus,
          * 2616 says unrecognized status codes should be
          * treated as the equivalent to the x00 code, and we
          * don't redirect on 300, so therefore we shouldn't
          * redirect on 308+ either.
          */
     }

    if (redirect)
      {
        soup_message_set_uri (msg, new_uri);
        soup_session_requeue_message (session, msg);
      }

    soup_uri_free (new_uri);
}

static guint
g_vfs_backend_dav_send_message (GVfsBackend *backend, SoupMessage *message)
{
  GVfsBackendHttp *http_backend;
  SoupSession     *session;

  http_backend = G_VFS_BACKEND_HTTP (backend);
  session = http_backend->session;

  /* We have our own custom redirect handler */
  soup_message_set_flags (message, SOUP_MESSAGE_NO_REDIRECT);

  soup_message_add_header_handler (message, "got_body", "Location",
                                   G_CALLBACK (redirect_handler), session);

  return http_backend_send_message (backend, message);
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

static gint
http_to_gio_error(guint status_code)
{
  switch (status_code)
    {
      case SOUP_STATUS_NOT_FOUND:
        return G_IO_ERROR_NOT_FOUND;
        break;
      case SOUP_STATUS_UNAUTHORIZED:
      case SOUP_STATUS_PAYMENT_REQUIRED:
      case SOUP_STATUS_FORBIDDEN:
        return G_IO_ERROR_PERMISSION_DENIED;
        break;
      case SOUP_STATUS_REQUEST_TIMEOUT:
        return G_IO_ERROR_TIMED_OUT;
        break;
      case SOUP_STATUS_CANT_RESOLVE:
        return G_IO_ERROR_HOST_NOT_FOUND;
        break;
      case SOUP_STATUS_NOT_IMPLEMENTED:
        return G_IO_ERROR_NOT_SUPPORTED;
        break;
      case SOUP_STATUS_INSUFFICIENT_STORAGE:
        return G_IO_ERROR_NO_SPACE;
        break;
    }
  
  return G_IO_ERROR_FAILED;
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
      g_set_error (error, G_IO_ERROR, http_to_gio_error (msg->status_code),
                   _("HTTP Error: %s"), msg->reason_phrase);
      return NULL;
    }

  doc = xmlReadMemory (msg->response_body->data,
                       msg->response_body->length,
                       "response.xml",
                       NULL,
                       XML_PARSE_NONET |
                       XML_PARSE_NOWARNING |
                       XML_PARSE_NOBLANKS |
                       XML_PARSE_NSCLEAN |
                       XML_PARSE_NOCDATA |
                       XML_PARSE_COMPACT);
  if (doc == NULL)
    { 
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
	                   _("Could not parse response"));
      return NULL;
    }

  *root = xmlDocGetRootElement (doc);

  if (*root == NULL || (*root)->children == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
	                   _("Empty response"));
      xmlFreeDoc (doc);
      return NULL;
    }

  if (strcmp ((char *) (*root)->name, name))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Unexpected reply from server"));
      xmlFreeDoc (doc);
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

  SoupURI *target;

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

  return http_uri_get_basename (text);

}

static gboolean
ms_response_is_target (MsResponse *response)
{
  const char *text;
  const char *path;
  SoupURI    *target;
  SoupURI    *uri;
  gboolean    res;

  uri    = NULL;
  path   = NULL;
  target = response->multistatus->target;
  text   = node_get_content (response->href);

  if (text == NULL)
    return FALSE;

  uri = soup_uri_new_with_base (target, text);

  if (uri == NULL)
    return FALSE;

  res = dav_uri_match (uri, target, TRUE);

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

static inline void
file_info_set_content_type (GFileInfo *info, const char *type)
{
  g_file_info_set_content_type (info, type);
  g_file_info_set_attribute_string (info,
                                    G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
                                    type);

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
  const char *text;
  GTimeVal    tv;
  GFileType   file_type;
  char       *mime_type;
  GIcon      *icon;
  gboolean    have_display_name;

  basename = ms_response_get_basename (response);
  g_file_info_set_name (info, basename);
  g_file_info_set_edit_name (info, basename);

  file_type = G_FILE_TYPE_UNKNOWN;
  mime_type = NULL;

  have_display_name = FALSE;
  ms_response_get_propstat_iter (response, &iter);
  while (xml_node_iter_next (&iter))
    {
      status = ms_response_get_propstat (&iter, &propstat);

      if (! SOUP_STATUS_IS_SUCCESSFUL (status))
        continue;

      for (node = propstat.prop_node->children; node; node = node->next)
        {
          if (! node_is_element (node))
            continue; /* TODO: check namespace, parse user data nodes*/

          text = node_get_content (node);

          if (node_has_name (node, "resourcetype"))
            {
              file_type = parse_resourcetype (node);
              g_file_info_set_file_type (info, file_type);
            }
          else if (node_has_name (node, "displayname"))
            {
              g_file_info_set_display_name (info, text);
              have_display_name = TRUE;
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
            }
          else if (node_has_name (node, "getcontenttype"))
            {
              mime_type = g_strdup (text);
            }
          else if (node_has_name (node, "getcontentlength"))
            {
              gint64 size;
              size = g_ascii_strtoll (text, NULL, 10);
              g_file_info_set_size (info, size);
            }
          else if (node_has_name (node, "getlastmodified"))
            {
              SoupDate *sd;
              GTimeVal tv;
	      sd = soup_date_new_from_string(text);
	      if (sd)
	        {
		  soup_date_to_timeval (sd, &tv);
		  g_file_info_set_modification_time (info, &tv);
		  soup_date_free (sd);
		}
	    }
        }
    }

  if (file_type == G_FILE_TYPE_DIRECTORY)
    {
      icon = g_themed_icon_new ("folder");
      file_info_set_content_type (info, "inode/directory");
    }
  else
    {
      if (mime_type == NULL)
        mime_type = g_content_type_guess (basename, NULL, 0, NULL);

      icon = g_content_type_get_icon (mime_type);

      if (G_IS_THEMED_ICON (icon))
        g_themed_icon_append_name (G_THEMED_ICON (icon), "text-x-generic");

      file_info_set_content_type (info, mime_type);
    }

  if (have_display_name == FALSE)
    g_file_info_set_display_name (info, basename);

  g_file_info_set_icon (info, icon);
  g_object_unref (icon);
  g_free (mime_type);
  g_free (basename);

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

#define PROPSTAT_XML_BEGIN                        \
  "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n" \
  " <D:propfind xmlns:D=\"DAV:\">\n"

#define PROPSTAT_XML_ALLPROP "  <D:allprop/>\n"
#define PROPSTAT_XML_PROP_BEGIN "  <D:prop>\n"
#define PROPSTAT_XML_PROP_END   "  </D:prop>\n"

#define PROPSTAT_XML_END                          \
  " </D:propfind>"

typedef struct _PropName {
  
  const char *name;
  const char *namespace;

} PropName;


static SoupMessage *
propfind_request_new (GVfsBackend     *backend,
                      const char      *filename,
                      guint            depth,
                      const PropName  *properties)
{
  SoupMessage *msg;
  SoupURI     *uri;
  const char  *header_depth;
  GString     *body;

  uri = http_backend_uri_for_filename (backend, filename, depth > 0);
  msg = soup_message_new_from_uri (SOUP_METHOD_PROPFIND, uri);
  soup_uri_free (uri);

  if (msg == NULL)
    return NULL;

  if (depth == 0)
    header_depth = "0";
  else if (depth == 1)
    header_depth = "1";
  else
    header_depth = "infinity";

  soup_message_headers_append (msg->request_headers, "Depth", header_depth);

  body = g_string_new (PROPSTAT_XML_BEGIN);

  if (properties != NULL)
    {
      const PropName *prop;
      g_string_append (body, PROPSTAT_XML_PROP_BEGIN);

      for (prop = properties; prop->name; prop++)
        {
          if (prop->namespace != NULL)
            g_string_append_printf (body, "<%s xmlns=\"%s\"/>\n",
                                    prop->name,
                                    prop->namespace);
          else
            g_string_append_printf (body, "<D:%s/>\n", prop->name);
        }
      g_string_append (body, PROPSTAT_XML_PROP_END);
    }
  else
    g_string_append (body, PROPSTAT_XML_ALLPROP);
    

  g_string_append (body, PROPSTAT_XML_END);

  soup_message_set_request (msg, "application/xml",
                            SOUP_MEMORY_TAKE,
                            body->str,
                            body->len);

  g_string_free (body, FALSE);

  return msg;
}

static SoupMessage *
stat_location_begin (SoupURI  *uri,
                     gboolean  count_children)
{
  SoupMessage       *msg;
  const char        *depth;
  static const char *stat_profind_body =
    PROPSTAT_XML_BEGIN
    PROPSTAT_XML_PROP_BEGIN
    "<D:resourcetype/>\n"
    PROPSTAT_XML_PROP_END
    PROPSTAT_XML_END;

  msg = soup_message_new_from_uri (SOUP_METHOD_PROPFIND, uri);

  if (count_children)
    depth = "1";
  else
    depth = "0";

  soup_message_headers_append (msg->request_headers, "Depth", depth);

  soup_message_set_request (msg, "application/xml",
                            SOUP_MEMORY_STATIC,
                            stat_profind_body,
                            strlen (stat_profind_body));
  return msg;
}

static gboolean
stat_location_finish (SoupMessage *msg,
                      GFileType   *target_type,
                      guint       *num_children)
{
  Multistatus  ms;
  xmlNodeIter  iter;
  gboolean     res;
  GError      *error;
  guint        child_count;
  GFileType    file_type;

  if (msg->status_code != 207)
    return FALSE;

  res = multistatus_parse (msg, &ms, &error);

  if (res == FALSE)
    return FALSE;

  res = FALSE;
  child_count = 0;
  file_type = G_FILE_TYPE_UNKNOWN;

  multistatus_get_response_iter (&ms, &iter);
  while (xml_node_iter_next (&iter))
    {
      MsResponse response;

      if (! multistatus_get_response (&iter, &response))
        continue;

      if (ms_response_is_target (&response))
        {
          file_type = ms_response_to_file_type (&response);
          res = TRUE;
        }
      else
        child_count++;
    }

  if (res)
    {
      if (target_type)
        *target_type = file_type;

      if (num_children)
        *num_children = child_count;
    }

  multistatus_free (&ms);
  return res;
}

static gboolean
stat_location (GVfsBackend  *backend,
               SoupURI      *uri,
               GFileType    *target_type,
               guint        *num_children,
               GError      **error)
{
  SoupMessage *msg;
  guint        status;
  gboolean     count_children;
  gboolean     res;

  count_children = num_children != NULL;
  msg = stat_location_begin (uri, count_children);

  if (msg == NULL)
    return FALSE;

  status = g_vfs_backend_dav_send_message (backend, msg);

  if (status != 207)
    {
      g_set_error_literal (error,
	                   G_IO_ERROR,
        	           http_error_code_from_status (status),
                	   msg->reason_phrase);

      return FALSE;
    }

  res = stat_location_finish (msg, target_type, num_children);

  if (res == FALSE)
    g_set_error_literal (error, 
	                 G_IO_ERROR, G_IO_ERROR_FAILED,
        	         _("Response invalid"));

  return res;
}


/* ************************************************************************* */
/* Authentication */

static void
mount_auth_info_free (MountAuthData *data)
{
  if (data->mount_source)
    g_object_unref (data->mount_source);
  
  g_free (data->server_auth.username);
  g_free (data->server_auth.password);
  g_free (data->server_auth.realm);
  
  g_free (data->proxy_auth.username);
  g_free (data->proxy_auth.password);

}

static void
soup_authenticate_from_data (SoupSession *session,
                             SoupMessage *msg,
                             SoupAuth    *auth,
                             gboolean     retrying,
                             gpointer     user_data)
{
  MountAuthData *data;
  AuthInfo      *info;

  g_debug ("+ soup_authenticate_from_data (%s) \n",
           retrying ? "retrying" : "first auth");

  if (retrying)
    return;

  data = (MountAuthData *) user_data;

  if (soup_auth_is_for_proxy (auth))
    info = &data->proxy_auth;
  else
    info = &data->server_auth;

  soup_auth_authenticate (auth, info->username, info->password);
}

static void
soup_authenticate_interactive (SoupSession *session,
                               SoupMessage *msg,
                               SoupAuth    *auth,
                               gboolean     retrying,
                               gpointer     user_data)
{
  MountAuthData     *data;
  AuthInfo          *info;
  GAskPasswordFlags  pw_ask_flags;
  GPasswordSave      pw_save;
  const char        *realm;
  gboolean           res;
  gboolean           aborted;
  gboolean           is_proxy;
  gboolean           have_auth;
  char              *new_username;
  char              *new_password;
  char              *prompt;

  g_debug ("+ soup_authenticate_interactive (%s) \n",
           retrying ? "retrying" : "first auth");

  data = (MountAuthData *) user_data;

  new_username = NULL;
  new_password = NULL;
  realm        = NULL;
  pw_ask_flags = G_ASK_PASSWORD_NEED_PASSWORD;

  is_proxy = soup_auth_is_for_proxy (auth);
  realm    = soup_auth_get_realm (auth);

  if (is_proxy)
    info = &(data->proxy_auth);
  else
    info = &(data->server_auth);

  if (realm && info->realm == NULL)
    info->realm = g_strdup (realm);
  else if (realm && info->realm && !g_str_equal (realm, info->realm))
    return;

  have_auth = info->username && info->password;

  if (have_auth == FALSE && g_vfs_keyring_is_available ())
    {
      SoupURI *uri; 
      SoupURI *uri_free = NULL;

      pw_ask_flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;

      if (is_proxy)
        {
          g_object_get (session, SOUP_SESSION_PROXY_URI, &uri_free, NULL);
          uri = uri_free;
        }
      else
        uri = soup_message_get_uri (msg);

      res = g_vfs_keyring_lookup_password (info->username,
                                           uri->host,
                                           NULL,
                                           "http",
                                           realm,
                                           is_proxy ? "proxy" : "basic",
                                           uri->port,
                                           &new_username,
                                           NULL,
                                           &new_password);

      if (res == TRUE)
        {
          have_auth = TRUE;
          g_free (info->username);
          g_free (info->password);
          info->username = new_username;
          info->password = new_password;
        }

      if (uri_free)
        soup_uri_free (uri_free);
    }

  if (retrying == FALSE && have_auth)
    {
      soup_auth_authenticate (auth, info->username, info->password);
      return;
    }

  if (is_proxy == FALSE)
    {
      if (realm == NULL)
        realm = _("WebDAV share");

      prompt = g_strdup_printf (_("Enter password for %s"), realm);
    }
  else
    prompt = g_strdup (_("Please enter proxy password"));

  if (info->username == NULL)
    pw_ask_flags |= G_ASK_PASSWORD_NEED_USERNAME;

  res = g_mount_source_ask_password (data->mount_source,
                                     prompt,
                                     info->username,
                                     NULL,
                                     pw_ask_flags, 
                                     &aborted,
                                     &new_password,
                                     &new_username,
                                     NULL,
				     NULL,
                                     &pw_save);

  if (res && !aborted)
    {
      /* it's not safe to assume that we get the username filed in,
         in the case that we provied a default username */
      if (new_username == NULL)
        new_username = g_strdup (info->username);

      soup_auth_authenticate (auth, new_username, new_password);

      g_free (info->username);
      g_free (info->password);
      info->username = new_username;
      info->password = new_password;
      info->pw_save  = pw_save;
    }
  else
    soup_session_cancel_message (session, msg, SOUP_STATUS_CANCELLED);

  g_debug ("- soup_authenticate \n");
  g_free (prompt);
}

static void
keyring_save_authinfo (AuthInfo *info,
                       SoupURI  *uri,
                       gboolean  is_proxy)
{
  const char *type = is_proxy ? "proxy" : "basic";

  g_vfs_keyring_save_password (info->username,
                               uri->host,
                               NULL,
                               "http",
                               info->realm,
                               type,
                               uri->port,
                               info->password,
                               info->pw_save);
}

/* ************************************************************************* */

static SoupURI *
g_mount_spec_to_dav_uri (GMountSpec *spec)
{
  SoupURI        *uri;
  const char     *host;
  const char     *user;
  const char     *port;
  const char     *ssl;
  gint            port_num;

  host = g_mount_spec_get (spec, "host");
  user = g_mount_spec_get (spec, "user");
  port = g_mount_spec_get (spec, "port");
  ssl  = g_mount_spec_get (spec, "ssl");

  if (host == NULL || *host == 0)
    return NULL;

  uri = soup_uri_new (NULL);

  if (ssl != NULL && (strcmp (ssl, "true") == 0))
    soup_uri_set_scheme (uri, SOUP_URI_SCHEME_HTTPS);
  else
    soup_uri_set_scheme (uri, SOUP_URI_SCHEME_HTTP);

  soup_uri_set_user (uri, user);

  if (port && (port_num = atoi (port)))
    soup_uri_set_port (uri, port_num);

  soup_uri_set_host (uri, host);
  soup_uri_set_path (uri, spec->mount_prefix);

  return uri;
}

static GMountSpec *
g_mount_spec_from_dav_uri (GVfsBackendDav *dav_backend,
                           SoupURI *uri)
{
  GMountSpec *spec;
  const char *ssl;

#ifdef HAVE_AVAHI
  if (dav_backend->resolver != NULL)
    {
      const char *type;
      const char *service_type;

      service_type = g_vfs_dns_sd_resolver_get_service_type (dav_backend->resolver);
      if (strcmp (service_type, "_webdavs._tcp") == 0)
        type = "davs+sd";
      else
        type = "dav+sd";

      spec = g_mount_spec_new (type);
      g_mount_spec_set (spec,
                        "host",
                        g_vfs_dns_sd_resolver_get_encoded_triple (dav_backend->resolver));
      return spec;
    }
#endif

  spec = g_mount_spec_new ("dav");

  g_mount_spec_set (spec, "host", uri->host);

  if (uri->scheme == SOUP_URI_SCHEME_HTTPS)
    ssl = "true";
  else
    ssl = "false";

  g_mount_spec_set (spec, "ssl", ssl);

  if (uri->user)
    g_mount_spec_set (spec, "user", uri->user);

  if (! soup_uri_uses_default_port (uri))
    {
      char *port = g_strdup_printf ("%u", uri->port);
      g_mount_spec_set (spec, "port", port);
      g_free (port);
    }

  g_mount_spec_set_mount_prefix (spec, uri->path);

  return spec;
}

#ifdef HAVE_AVAHI
static SoupURI *
dav_uri_from_dns_sd_resolver (GVfsBackendDav *dav_backend)
{
  SoupURI    *uri;
  char       *user;
  char       *path;
  char       *address;
  const char *service_type;
  guint       port;

  service_type = g_vfs_dns_sd_resolver_get_service_type (dav_backend->resolver);
  address = g_vfs_dns_sd_resolver_get_address (dav_backend->resolver);
  port = g_vfs_dns_sd_resolver_get_port (dav_backend->resolver);
  user = g_vfs_dns_sd_resolver_lookup_txt_record (dav_backend->resolver, "u"); /* mandatory */
  path = g_vfs_dns_sd_resolver_lookup_txt_record (dav_backend->resolver, "path"); /* optional */

  /* TODO: According to http://www.dns-sd.org/ServiceTypes.html
   * there's also a TXT record "p" for password. Handle this.
   */

  uri = soup_uri_new (NULL);

  if (strcmp (service_type, "_webdavs._tcp") == 0)
    soup_uri_set_scheme (uri, SOUP_URI_SCHEME_HTTPS);
  else
    soup_uri_set_scheme (uri, SOUP_URI_SCHEME_HTTP);

  soup_uri_set_user (uri, user);

  soup_uri_set_port (uri, port);

  soup_uri_set_host (uri, address);

  if (path != NULL)
    soup_uri_set_path (uri, path);
  else
    soup_uri_set_path (uri, "/");


  g_free (address);
  g_free (user);
  g_free (path);

  return uri;
}
#endif

#ifdef HAVE_AVAHI
static void
dns_sd_resolver_changed (GVfsDnsSdResolver *resolver,
                         GVfsBackendDav    *dav_backend)
{
  /* TODO: handle when DNS-SD data changes */
}
#endif

/* ************************************************************************* */
/* Backend Functions */
static void
do_mount (GVfsBackend  *backend,
          GVfsJobMount *job,
          GMountSpec   *mount_spec,
          GMountSource *mount_source,
          gboolean      is_automount)
{
  GVfsBackendDav *dav_backend = G_VFS_BACKEND_DAV (backend);
  MountAuthData  *data;
  SoupSession    *session;
  SoupMessage    *msg_opts;
  SoupMessage    *msg_stat;
  SoupURI        *mount_base;
  gulong          signal_id;
  guint           status;
  gboolean        is_success;
  gboolean        is_webdav;
  gboolean        res;
  char           *last_good_path;
  char           *display_name;
  const char     *host;
  const char     *type;

  g_debug ("+ mount\n");

  host = g_mount_spec_get (mount_spec, "host");
  type = g_mount_spec_get (mount_spec, "type");

#ifdef HAVE_AVAHI
  /* resolve DNS-SD style URIs */
  if ((strcmp (type, "dav+sd") == 0 || strcmp (type, "davs+sd") == 0) && host != NULL)
    {
      GError *error;

      dav_backend->resolver = g_vfs_dns_sd_resolver_new_for_encoded_triple (host, "u");

      error = NULL;
      if (!g_vfs_dns_sd_resolver_resolve_sync (dav_backend->resolver,
                                               NULL,
                                               &error))
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          return;
        }
      g_signal_connect (dav_backend->resolver,
                        "changed",
                        (GCallback) dns_sd_resolver_changed,
                        dav_backend);

      mount_base = dav_uri_from_dns_sd_resolver (dav_backend);
    }
  else
#endif
    {
      mount_base = g_mount_spec_to_dav_uri (mount_spec);
    }

  if (mount_base == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        _("Invalid mount spec"));
      return;
    }

  session = G_VFS_BACKEND_HTTP (backend)->session;
  G_VFS_BACKEND_HTTP (backend)->mount_base = mount_base; 

  data = &(G_VFS_BACKEND_DAV (backend)->auth_info); 
  data->mount_source = g_object_ref (mount_source);
  data->server_auth.username = g_strdup (mount_base->user);
  data->server_auth.pw_save = G_PASSWORD_SAVE_NEVER;
  data->proxy_auth.pw_save = G_PASSWORD_SAVE_NEVER;

  signal_id = g_signal_connect (session, "authenticate",
                                G_CALLBACK (soup_authenticate_interactive),
                                data);

  last_good_path = NULL;
  msg_opts = soup_message_new_from_uri (SOUP_METHOD_OPTIONS, mount_base);
  msg_stat = stat_location_begin (mount_base, FALSE);

  do {
    status = g_vfs_backend_dav_send_message (backend, msg_opts);

    is_success = SOUP_STATUS_IS_SUCCESSFUL (status);
    is_webdav = is_success && sm_has_header (msg_opts, "DAV");

    soup_message_headers_clear (msg_opts->response_headers);
    soup_message_body_truncate (msg_opts->response_body);

    if (is_webdav)
      {
        GFileType file_type;
        SoupURI *cur_uri;
        
        cur_uri = soup_message_get_uri (msg_opts);
        soup_message_set_uri (msg_stat, cur_uri);

        g_vfs_backend_dav_send_message (backend, msg_stat);
        res = stat_location_finish (msg_stat, &file_type, NULL);

        if (res && file_type == G_FILE_TYPE_DIRECTORY)
          {
            g_free (last_good_path);
            last_good_path = mount_base->path;
          }

        mount_base->path = path_get_parent_dir (mount_base->path);
        soup_message_set_uri (msg_opts, mount_base);

        soup_message_headers_clear (msg_stat->response_headers);
        soup_message_body_truncate (msg_stat->response_body);
      }

  } while (is_webdav && mount_base->path != NULL);

  /* we have reached the end of paths we are allowed to
   * chdir up to (or couldn't chdir up at all) */

  /* check if we at all have a good path */
  if (last_good_path == NULL) 
    {

      /* TODO: set correct error in case of cancellation */
      if (!is_success) 
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("HTTP Error: %s"), msg_opts->reason_phrase);
      else if (!is_webdav)
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Not a WebDAV enabled share"));
      else
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Not a WebDAV enabled share"));

      /* TODO: We leak a bunch of stuff here :-( */
      /* TODO: STRING CHANGE: change to: Could not find an enclosing directory */
      return;
    }

  /* Success! We are mounted */	
  /* Save the auth info in the keyring */

  keyring_save_authinfo (&(data->server_auth), mount_base, FALSE);
  /* TODO: save proxy auth */

  /* Set the working path in mount path */
  g_free (mount_base->path);
  mount_base->path = last_good_path;

  /* dup the mountspec, but only copy known fields */
  mount_spec = g_mount_spec_from_dav_uri (dav_backend, mount_base);

  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_vfs_backend_set_icon_name (backend, "folder-remote");
  
#ifdef HAVE_AVAHI
  if (dav_backend->resolver != NULL)
    display_name = g_strdup (g_vfs_dns_sd_resolver_get_service_name (dav_backend->resolver));
  else
#endif
    display_name = g_strdup_printf (_("WebDAV on %s"), mount_base->host);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  
  /* cleanup */
  g_mount_spec_unref (mount_spec);
  g_object_unref (msg_opts);
  g_object_unref (msg_stat);

  /* switch the signal handler */
  g_signal_handler_disconnect (session, signal_id);
  g_signal_connect (session, "authenticate",
                    G_CALLBACK (soup_authenticate_from_data),
                    data);

  /* also auth the workaround async session we need for SoupInputStream */
  g_signal_connect (G_VFS_BACKEND_HTTP (backend)->session_async, "authenticate",
                    G_CALLBACK (soup_authenticate_from_data),
                    data);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_debug ("- mount\n");
}

static PropName ls_propnames[] = {
    {"creationdate",     NULL},
    {"displayname",      NULL},
    {"getcontentlength", NULL},
    {"getcontenttype",   NULL},
    {"getetag",          NULL},
    {"getlastmodified",  NULL},
    {"resourcetype",     NULL},
    {NULL,               NULL}
};

/* *** query_info () *** */
static void
do_query_info (GVfsBackend           *backend,
               GVfsJobQueryInfo      *job,
               const char            *filename,
               GFileQueryInfoFlags    flags,
               GFileInfo             *info,
               GFileAttributeMatcher *matcher)
{
  SoupMessage *msg;
  Multistatus  ms;
  xmlNodeIter  iter;
  gboolean     res;
  GError      *error;

  error   = NULL;

  g_debug ("Query info %s\n", filename);

  msg = propfind_request_new (backend, filename, 0, ls_propnames);

  if (msg == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Could not create request"));
      
      return;
    }

  message_add_redirect_header (msg, flags);

  g_vfs_backend_dav_send_message (backend, msg);

  res = multistatus_parse (msg, &ms, &error);

  if (res == FALSE)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_object_unref (msg);
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

  multistatus_free (&ms);
  g_object_unref (msg);

  if (res)
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Response invalid"));

}


/* *** enumerate *** */
static void
do_enumerate (GVfsBackend           *backend,
              GVfsJobEnumerate      *job,
              const char            *filename,
              GFileAttributeMatcher *matcher,
              GFileQueryInfoFlags    flags)
{
  SoupMessage *msg;
  Multistatus  ms;
  xmlNodeIter  iter;
  gboolean     res;
  GError      *error;
 
  error = NULL;

  g_debug ("+ do_enumerate: %s\n", filename);

  msg = propfind_request_new (backend, filename, 1, ls_propnames);

  if (msg == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Could not create request"));
      
      return;
    }

  message_add_redirect_header (msg, flags);

  g_vfs_backend_dav_send_message (backend, msg);

  res = multistatus_parse (msg, &ms, &error);

  if (res == FALSE)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_object_unref (msg);
      return;
    }

  multistatus_get_response_iter (&ms, &iter);

  while (xml_node_iter_next (&iter))
    {
      MsResponse  response;
      GFileInfo  *info;

      if (! multistatus_get_response (&iter, &response))
        continue;

      if (ms_response_is_target (&response))
        continue;

      info = g_file_info_new ();
      ms_response_to_file_info (&response, info);
      g_vfs_job_enumerate_add_info (job, info);
    }

  multistatus_free (&ms);
  g_object_unref (msg);

  g_vfs_job_succeeded (G_VFS_JOB (job)); /* should that be called earlier? */
  g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
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
  SoupURI         *uri;

  if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      g_vfs_job_failed (job,
                        G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
      return;
    }
  /* TODO: other errors */

  uri = soup_message_get_uri (msg);
  put_msg = soup_message_new_from_uri (SOUP_METHOD_PUT, uri);

  /* 
   * Doesn't work with apache > 2.2.9
   * soup_message_headers_append (put_msg->request_headers, "If-None-Match", "*");
   */
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
  SoupMessage *msg;
  SoupURI     *uri;

  /* TODO: if SoupOutputStream supported chunked requests, we could
   * use a PUT with "If-None-Match: *" and "Expect: 100-continue"
   */
  uri = http_backend_uri_for_filename (backend, filename, FALSE);
  msg = soup_message_new_from_uri (SOUP_METHOD_HEAD, uri);
  soup_uri_free (uri);

  g_vfs_job_set_backend_data (G_VFS_JOB (job), backend, NULL);

  http_backend_queue_message (backend, msg, try_create_tested_existence, job);
  return TRUE;
}

/* *** replace () *** */
static void
open_for_replace_succeeded (GVfsBackendHttp *op_backend, GVfsJob *job,
                            SoupURI *uri, const char *etag)
{
  SoupMessage     *put_msg;
  GOutputStream   *stream;

  put_msg = soup_message_new_from_uri (SOUP_METHOD_PUT, uri);

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
  /* TODO: other errors */

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

  /* TODO: if SoupOutputStream supported chunked requests, we could
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



  uri = http_backend_uri_for_filename (backend, filename, FALSE);

  if (etag)
    {
      SoupMessage *msg;

      msg = soup_message_new_from_uri (SOUP_METHOD_HEAD, uri);
      soup_uri_free (uri);
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
     g_vfs_job_failed_literal (G_VFS_JOB (job),
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
  GOutputStream   *stream;

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

  error = NULL;
  job = G_VFS_JOB (user_data);
  stream = G_OUTPUT_STREAM (source_object);
  res = g_output_stream_close_finish (stream,
                                      result,
                                      &error);
  if (res == FALSE)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
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
  GOutputStream   *stream;

  stream = G_OUTPUT_STREAM (handle);

  g_output_stream_close_async (stream,
                               G_PRIORITY_DEFAULT,
                               G_VFS_JOB (job)->cancellable,
                               close_write_ready,
                               job);

  return TRUE;
}

static void
do_make_directory (GVfsBackend          *backend,
                   GVfsJobMakeDirectory *job,
                   const char           *filename)
{
  SoupMessage *msg;
  SoupURI     *uri;
  guint        status;

  uri = http_backend_uri_for_filename (backend, filename, TRUE);
  msg = soup_message_new_from_uri (SOUP_METHOD_MKCOL, uri);
  soup_uri_free (uri);

  status = g_vfs_backend_dav_send_message (backend, msg);

  if (! SOUP_STATUS_IS_SUCCESSFUL (status))
    if (status == SOUP_STATUS_METHOD_NOT_ALLOWED)
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
    else
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                                http_error_code_from_status (status),
                                msg->reason_phrase);
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));

  g_object_unref (msg);
}

static void
do_delete (GVfsBackend   *backend,
           GVfsJobDelete *job,
           const char    *filename)
{
  SoupMessage *msg;
  SoupURI     *uri;
  GFileType    file_type;
  gboolean     res;
  guint        num_children;
  guint        status;
  GError      *error;

  error = NULL;

  uri = http_backend_uri_for_filename (backend, filename, FALSE);
  res = stat_location (backend, uri, &file_type, &num_children, &error);

  if (res == FALSE)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  if (file_type == G_FILE_TYPE_DIRECTORY && num_children)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_EMPTY,
                        _("Directory not empty"));
      return;
    }

  msg = soup_message_new_from_uri (SOUP_METHOD_DELETE, uri);

  status = g_vfs_backend_dav_send_message (backend, msg);

  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR,
                              http_error_code_from_status (status),
                              msg->reason_phrase);
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));

  soup_uri_free (uri);
  g_object_unref (msg);
}

static void
do_set_display_name (GVfsBackend           *backend,
                     GVfsJobSetDisplayName *job,
                     const char            *filename,
                     const char            *display_name)
{
  SoupMessage *msg;
  SoupURI     *source;
  SoupURI     *target;
  char        *target_path;
  char        *dirname;
  guint        status;

  source = http_backend_uri_for_filename (backend, filename, FALSE);
  msg = soup_message_new_from_uri (SOUP_METHOD_MOVE, source);

  dirname = g_path_get_dirname (filename);
  target_path = g_build_filename (dirname, display_name, NULL);
  target = http_backend_uri_for_filename (backend, target_path, FALSE);

  message_add_destination_header (msg, target);
  message_add_overwrite_header (msg, FALSE);

  status = g_vfs_backend_dav_send_message (backend, msg);

  /*
   * The precondition of SOUP_STATUS_PRECONDITION_FAILED (412) in
   * this case was triggered by the "Overwrite: F" header which
   * means that the target already exists.
   * Also if we get a REDIRECTION it means that there was no
   * "Location" header, since otherwise that would have triggered
   * our redirection handler. This probably means we are dealing
   * with an web dav implementation (like mod_dav) that also sends
   * redirects for the destionaion (i.e. "Destination: /foo" header)
   * which very likely means that the target also exists (and is a
   * directory). That or the webdav server is broken.
   * We could find out by doing another stat and but I think this is
   * such a corner case that we are totally fine with returning
   * G_IO_ERROR_EXISTS.
   * */

  if (SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      g_debug ("new target_path: %s\n", target_path);
      g_vfs_job_set_display_name_set_new_path (job, target_path);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else if (status == SOUP_STATUS_PRECONDITION_FAILED ||
           SOUP_STATUS_IS_REDIRECTION (status))
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_EXISTS,
                      _("Target file already exists"));
  else
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      http_error_code_from_status (status),
                      "%s", msg->reason_phrase);

  g_object_unref (msg);
  g_free (dirname);
  g_free (target_path);
  soup_uri_free (target);
  soup_uri_free (source);
}

static gboolean
try_unmount (GVfsBackend    *backend,
             GVfsJobUnmount *job,
             GMountUnmountFlags flags,
             GMountSource *mount_source)
{
  _exit (0);
}

/* ************************************************************************* */
/*  */
static void
g_vfs_backend_dav_class_init (GVfsBackendDavClass *klass)
{
  GObjectClass         *gobject_class;
  GVfsBackendClass     *backend_class;
  
  gobject_class = G_OBJECT_CLASS (klass); 
  gobject_class->finalize  = g_vfs_backend_dav_finalize;

  backend_class = G_VFS_BACKEND_CLASS (klass); 

  backend_class->try_mount         = NULL;
  backend_class->mount             = do_mount;
  backend_class->try_query_info    = NULL;
  backend_class->query_info        = do_query_info;
  backend_class->enumerate         = do_enumerate;
  backend_class->try_create        = try_create;
  backend_class->try_replace       = try_replace;
  backend_class->try_write         = try_write;
  backend_class->try_close_write   = try_close_write;
  backend_class->make_directory    = do_make_directory;
  backend_class->delete            = do_delete;
  backend_class->set_display_name  = do_set_display_name;
  backend_class->try_unmount       = try_unmount;
}
