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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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
#include "gvfsjobpush.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsdaemonutils.h"
#include "gvfsutils.h"

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

  /* Used for user-verified secure connections. */
  GTlsCertificate *certificate;
  GTlsCertificateFlags certificate_errors;

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

  g_clear_object (&dav_backend->certificate);

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

static gboolean
string_to_uint64 (const char *str, guint64 *value)
{
  char *endptr;

  *value = g_ascii_strtoull (str, &endptr, 10);

  return endptr != str;
}

static inline gboolean
sm_has_header (SoupMessage *msg, const char *header)
{
  return soup_message_headers_get_one (msg->response_headers, header) != NULL;
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
    return g_strdup ("/");

  parent = g_strrstr_len (path, len, "/");

  if (parent == NULL)
    return g_strdup ("/");

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
  gboolean diff;
  char *ua, *ub;

  ua = g_uri_unescape_string (a->path, "/");
  ub = g_uri_unescape_string (b->path, "/");

  diff = a->scheme != b->scheme || a->port != b->port  ||
    ! str_equal (a->host, b->host, TRUE)               ||
    ! path_equal (ua, ub, relax)                       ||
    ! str_equal (a->query, b->query, FALSE)            ||
    ! str_equal (a->fragment, b->fragment, FALSE);

  g_free (ua);
  g_free (ub);

  return !diff;
}

static char *
dav_uri_encode (const char *path_to_encode)
{
  char *path;
  static const char *allowed_reserved_chars = "/";

  path = g_uri_escape_string (path_to_encode,
                              allowed_reserved_chars,
                              FALSE);

  return path;
}

static gboolean
message_should_apply_redir_ref (SoupMessage *msg)
{
  const char *header;

  header = soup_message_headers_get_one (msg->request_headers,
                                         "Apply-To-Redirect-Ref");

  if (header == NULL || g_ascii_strcasecmp (header, "T"))
    return FALSE;

  return TRUE;
}


static SoupURI *
g_vfs_backend_dav_uri_for_path (GVfsBackend *backend,
                                const char  *path,
                                gboolean     is_dir)
{
  SoupURI *mount_base;
  SoupURI *uri;
  char    *fn_encoded;
  char    *new_path;

  mount_base = http_backend_get_mount_base (backend);
  uri = soup_uri_copy (mount_base);

  /* "/" means "whatever mount_base is" */
  if (!strcmp (path, "/"))
    return uri;

  /* The mount_base path is escaped already so we need to
     escape the new path as well */
  fn_encoded = dav_uri_encode (path);

  /* Otherwise, we append filename to mount_base (which is assumed to
   * be a directory in this case).
   *
   * Add a "/" in cases where it is likely that the url is going
   * to be a directory to avoid redirections
   */
  if (is_dir == FALSE || g_str_has_suffix (path, "/"))
    new_path = g_build_path ("/", uri->path, fn_encoded, NULL);
  else
    new_path = g_build_path ("/", uri->path, fn_encoded, "/", NULL);

  g_free (fn_encoded);
  g_free (uri->path);
  uri->path = new_path;

  return uri;
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
    new_loc = soup_message_headers_get_one (msg->response_headers, "Location");

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

       dest = soup_message_headers_get_one (msg->request_headers,
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

static void
g_vfs_backend_dav_setup_display_name (GVfsBackend *backend)
{
  GVfsBackendDav *dav_backend;
  SoupURI        *mount_base;
  char           *display_name;
  char            port[7] = {0, };

  dav_backend = G_VFS_BACKEND_DAV (backend);

#ifdef HAVE_AVAHI
  if (dav_backend->resolver != NULL)
    {
      const char *name;
      name = g_vfs_dns_sd_resolver_get_service_name (dav_backend->resolver);
      g_vfs_backend_set_display_name (backend, name);
      return;
    }
#endif

  mount_base = http_backend_get_mount_base (backend);

  if (! soup_uri_uses_default_port (mount_base))
    g_snprintf (port, sizeof (port), ":%u", mount_base->port);

  if (mount_base->user != NULL)
    /* Translators: This is the name of the WebDAV share constructed as
       "WebDAV as <username> on <hostname>:<port>"; the ":<port>" part is
       the second %s and only shown if it is not the default http(s) port. */
    display_name = g_strdup_printf (_("%s on %s%s"),
				    mount_base->user,
				    mount_base->host,
				    port);
  else
    display_name = g_strdup_printf ("%s%s",
				    mount_base->host,
				    port);

  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
}

static void
certificate_error_handler (SoupMessage *msg,
                           GParamSpec *pspec,
                           gpointer user_data)
{
  GVfsBackendDav *dav = G_VFS_BACKEND_DAV (user_data);
  GTlsCertificate *certificate;
  GTlsCertificateFlags errors;

  /* Fail the message if the certificate errors change or the certificate is
   * different. */
  if (soup_message_get_https_status (msg, &certificate, &errors))
    {
      if (errors != dav->certificate_errors ||
          !g_tls_certificate_is_same (certificate, dav->certificate))
        {
          soup_session_cancel_message (G_VFS_BACKEND_HTTP (dav)->session,
                                       msg,
                                       SOUP_STATUS_SSL_FAILED);
        }
    }
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

  if (G_VFS_BACKEND_DAV (backend)->certificate_errors)
    g_signal_connect (message, "notify::tls-errors",
                      G_CALLBACK (certificate_error_handler), backend);

  soup_message_add_header_handler (message, "got_body", "Location",
                                   G_CALLBACK (redirect_handler), session);

  return http_backend_send_message (backend, message);
}

static void
g_vfs_backend_dav_queue_message (GVfsBackend *backend,
                                 SoupMessage *msg,
                                 SoupSessionCallback callback,
                                 gpointer user_data)
{
  if (G_VFS_BACKEND_DAV (backend)->certificate_errors)
    g_signal_connect (msg, "notify::tls-errors",
                      G_CALLBACK (certificate_error_handler), backend);

  soup_session_queue_message (G_VFS_BACKEND_HTTP (backend)->session, msg,
                              callback, user_data);
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

static gboolean
node_is_empty (xmlNodePtr node)
{
  if (node == NULL)
    return TRUE;

  if (node->type == XML_TEXT_NODE)
    return node->content == NULL || node->content[0] == '\0';

  return node->children == NULL;
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
      g_set_error (error,
                   G_IO_ERROR,
                   http_error_code_from_status (msg->status_code),
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
  char    *path;

};

struct _MsResponse {

  Multistatus *multistatus;
  char        *path;
  gboolean     is_target;
  xmlNodePtr   first_propstat;

};

struct _MsPropstat {

  Multistatus *multistatus;

  xmlNodePtr   prop_node;
  guint        status_code;

};


static gboolean
multistatus_parse (SoupMessage *msg, Multistatus *multistatus, GError **error)
{
  xmlDocPtr   doc;
  xmlNodePtr  root;
  SoupURI    *uri;

  doc = parse_xml (msg, &root, "multistatus", error);

  if (doc == NULL)
    return FALSE;

  uri = soup_message_get_uri (msg);

  multistatus->doc = doc;
  multistatus->root = root;
  multistatus->target = uri;
  multistatus->path = g_uri_unescape_string (uri->path, "/");

  return TRUE;
}

static void
multistatus_free (Multistatus *multistatus)
{
  xmlFreeDoc (multistatus->doc);
  g_free (multistatus->path);
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
  SoupURI     *uri;
  const char  *text;
  char        *path;

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

  text = node_get_content (href);

  if (text == NULL)
    return FALSE;

  uri = soup_uri_new_with_base (multistatus->target, text);

  if (uri == NULL)
    return FALSE;

  path = g_uri_unescape_string (uri->path, "/");
  soup_uri_free (uri);

  response->path = path;
  response->is_target = path_equal (path, multistatus->path, TRUE);
  response->multistatus = multistatus;
  response->first_propstat = propstat;

  return resp_node != NULL;
}

static void
ms_response_clear (MsResponse *response)
{
  g_free (response->path);
}

static char *
ms_response_get_basename (MsResponse *response)
{
  return http_path_get_basename (response->path);
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
  GIcon      *symbolic_icon;
  gboolean    have_display_name;

  basename = ms_response_get_basename (response);
  g_file_info_set_name (info, basename);
  g_file_info_set_edit_name (info, basename);

  if (basename && basename[0] == '.')
    g_file_info_set_is_hidden (info, TRUE);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

  file_type = G_FILE_TYPE_REGULAR;
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
          if (! node_is_element (node) || node_is_empty (node))
            continue; /* TODO: check namespace, parse user data nodes*/

          text = node_get_content (node);

          if (node_has_name (node, "resourcetype"))
            {
              file_type = parse_resourcetype (node);
            }
          else if (node_has_name (node, "displayname") && text)
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
              char *ptr;

              mime_type = g_strdup (text);

              /* Ignore parameters of the content type */
              ptr = strchr (mime_type, ';');
              if (ptr)
                {
                  do
                    *ptr-- = '\0';
                  while (ptr >= mime_type && g_ascii_isspace (*ptr));
                }
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

  g_file_info_set_file_type (info, file_type);
  if (file_type == G_FILE_TYPE_DIRECTORY)
    {
      icon = g_themed_icon_new ("folder");
      symbolic_icon = g_themed_icon_new ("folder-symbolic");
      file_info_set_content_type (info, "inode/directory");
    }
  else
    {
      if (mime_type == NULL)
        mime_type = g_content_type_guess (basename, NULL, 0, NULL);

      icon = g_content_type_get_icon (mime_type);
      if (G_IS_THEMED_ICON (icon))
        {
          g_themed_icon_append_name (G_THEMED_ICON (icon), "text-x-generic");
        }

      symbolic_icon = g_content_type_get_symbolic_icon (mime_type);
      if (G_IS_THEMED_ICON (icon))
        {
          g_themed_icon_append_name (G_THEMED_ICON (symbolic_icon), "text-x-generic-symbolic");
        }

      file_info_set_content_type (info, mime_type);
    }

  if (have_display_name == FALSE)
    g_file_info_set_display_name (info, basename);

  g_file_info_set_icon (info, icon);
  g_file_info_set_symbolic_icon (info, symbolic_icon);
  g_object_unref (icon);
  g_object_unref (symbolic_icon);
  g_free (mime_type);
  g_free (basename);

}

static void
ms_response_to_fs_info (MsResponse *response,
                        GFileInfo  *info)
{
  xmlNodeIter iter;
  MsPropstat  propstat;
  xmlNodePtr  node;
  guint       status;
  const char *text;
  guint64     bytes_avail;
  guint64     bytes_used;
  gboolean    have_bytes_avail;
  gboolean    have_bytes_used;

  bytes_avail = bytes_used = 0;
  have_bytes_avail = have_bytes_used = FALSE;

  ms_response_get_propstat_iter (response, &iter);
  while (xml_node_iter_next (&iter))
    {
      status = ms_response_get_propstat (&iter, &propstat);

      if (! SOUP_STATUS_IS_SUCCESSFUL (status))
        continue;

      for (node = propstat.prop_node->children; node; node = node->next)
        {
          if (! node_is_element (node))
            continue;

          text = node_get_content (node);
          if (text == NULL)
            continue;

          if (node_has_name (node, "quota-available-bytes"))
            {
              if (! string_to_uint64 (text, &bytes_avail))
                continue;

              have_bytes_avail = TRUE;
            }
          else if (node_has_name (node, "quota-used-bytes"))
            {
              if (! string_to_uint64 (text, &bytes_used))
                continue;

              have_bytes_used = TRUE;
            }
        }
    }

  if (have_bytes_used)
    g_file_info_set_attribute_uint64 (info,
                                      G_FILE_ATTRIBUTE_FILESYSTEM_USED,
                                      bytes_used);

  if (have_bytes_avail)
    {
      g_file_info_set_attribute_uint64 (info,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                        bytes_avail);
      if (have_bytes_used && G_MAXUINT64 - bytes_avail >= bytes_used)
        g_file_info_set_attribute_uint64 (info,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
                                         bytes_avail + bytes_used);
    }
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

  uri = g_vfs_backend_dav_uri_for_path (backend, filename, depth > 0);
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
    "<D:getcontentlength/>\n"
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
                      gint64      *target_size,
                      guint       *num_children)
{
  Multistatus  ms;
  xmlNodeIter  iter;
  gboolean     res;
  GError      *error;
  guint        child_count;
  GFileInfo   *file_info;

  if (msg->status_code != 207)
    return FALSE;

  res = multistatus_parse (msg, &ms, &error);

  if (res == FALSE)
    return FALSE;

  res = FALSE;
  child_count = 0;
  file_info = g_file_info_new ();

  multistatus_get_response_iter (&ms, &iter);
  while (xml_node_iter_next (&iter))
    {
      MsResponse response;

      if (! multistatus_get_response (&iter, &response))
        continue;

      if (response.is_target)
        {
          ms_response_to_file_info (&response, file_info);
          res = TRUE;
        }
      else
        child_count++;

      ms_response_clear (&response);
    }

  if (res)
    {
      if (target_type)
        *target_type = g_file_info_get_file_type (file_info);

      if (target_size)
        *target_size = g_file_info_get_size (file_info);

      if (num_children)
        *num_children = child_count;
    }

  multistatus_free (&ms);
  g_object_unref (file_info);
  return res;
}

static gboolean
stat_location (GVfsBackend  *backend,
               SoupURI      *uri,
               GFileType    *target_type,
               gint64       *target_size,
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

      g_object_unref (msg);
      return FALSE;
    }

  res = stat_location_finish (msg, target_type, target_size, num_children);
  g_object_unref (msg);

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
  const char     *path;
  gint            port_num;

  host = g_mount_spec_get (spec, "host");
  user = g_mount_spec_get (spec, "user");
  port = g_mount_spec_get (spec, "port");
  ssl  = g_mount_spec_get (spec, "ssl");
  path = spec->mount_prefix;

  if (host == NULL || *host == 0)
    return NULL;

  uri = soup_uri_new (NULL);

  if (ssl != NULL && (strcmp (ssl, "true") == 0))
    soup_uri_set_scheme (uri, SOUP_URI_SCHEME_HTTPS);
  else
    soup_uri_set_scheme (uri, SOUP_URI_SCHEME_HTTP);

  soup_uri_set_user (uri, user);

  /* IPv6 host does not include brackets in SoupURI, but GMountSpec host does */
  if (gvfs_is_ipv6 (host))
    uri->host = g_strndup (host + 1, strlen (host) - 2);
  else
    soup_uri_set_host (uri, host);

  if (port && (port_num = atoi (port)))
    soup_uri_set_port (uri, port_num);

  g_free (uri->path);
  uri->path = dav_uri_encode (path);

  return uri;
}

static GMountSpec *
g_mount_spec_from_dav_uri (GVfsBackendDav *dav_backend,
                           SoupURI *uri)
{
  GMountSpec *spec;
  const char *ssl;
  char       *local_path;

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

  /* IPv6 host does not include brackets in SoupURI, but GMountSpec host does */
  if (strchr (uri->host, ':'))
    {
      char *host = g_strdup_printf ("[%s]", uri->host);
      g_mount_spec_set (spec, "host", host);
      g_free (host);
    }
  else
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

  /* There must not be any illegal characters in the
     URL at this point */
  local_path = g_uri_unescape_string (uri->path, "/");
  g_mount_spec_set_mount_prefix (spec, local_path);
  g_free (local_path);

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
  /* If anything has changed (e.g. address, port, txt-records or is-resolved),
   * it is safest to just unmount. */
  g_vfs_backend_force_unmount (G_VFS_BACKEND (dav_backend));
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
  gboolean        is_collection;
  gboolean        auth_interactive;
  gboolean        res;
  char           *last_good_path;
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

  /* Override the HTTP backend's default. */
  g_object_set (session, "ssl-strict", TRUE, NULL);

  data = &(G_VFS_BACKEND_DAV (backend)->auth_info); 
  data->mount_source = g_object_ref (mount_source);
  data->server_auth.username = g_strdup (mount_base->user);
  data->server_auth.pw_save = G_PASSWORD_SAVE_NEVER;
  data->proxy_auth.pw_save = G_PASSWORD_SAVE_NEVER;

  signal_id = g_signal_connect (session, "authenticate",
                                G_CALLBACK (soup_authenticate_interactive),
                                data);
  auth_interactive = TRUE;

  last_good_path = NULL;
  msg_opts = soup_message_new_from_uri (SOUP_METHOD_OPTIONS, mount_base);
  msg_stat = stat_location_begin (mount_base, FALSE);


  do {
    GFileType file_type;
    SoupURI *cur_uri;

    res = TRUE;
    status = g_vfs_backend_dav_send_message (backend, msg_opts);
    is_success = SOUP_STATUS_IS_SUCCESSFUL (status);
    is_webdav = sm_has_header (msg_opts, "DAV");

    /* If SSL is used and the certificate verifies OK, then ssl-strict remains
     * on for all further connections.
     * If SSL is used and the certificate does not verify OK, then the user
     * gets a chance to override it. If they do, ssl-strict is disabled but
     * the certificate is stored, and checked on each subsequent connection to
     * ensure that it hasn't changed. */
    if (status == SOUP_STATUS_SSL_FAILED &&
        !dav_backend->certificate_errors)
      {
        GTlsCertificate *certificate;
        GTlsCertificateFlags errors;

        soup_message_get_https_status (msg_opts, &certificate, &errors);

        if (gvfs_accept_certificate (mount_source, certificate, errors))
          {
            g_object_set (session, "ssl-strict", FALSE, NULL);
            dav_backend->certificate = g_object_ref (certificate);
            dav_backend->certificate_errors = errors;
            continue;
          }
        else
          {
            break;
          }
      }

    if (!is_success || !is_webdav)
      break;

    soup_message_headers_clear (msg_opts->response_headers);
    soup_message_body_truncate (msg_opts->response_body);

    cur_uri = soup_message_get_uri (msg_opts);
    soup_message_set_uri (msg_stat, cur_uri);

    g_vfs_backend_dav_send_message (backend, msg_stat);
    res = stat_location_finish (msg_stat, &file_type, NULL, NULL);
    is_collection = res && file_type == G_FILE_TYPE_DIRECTORY;

    g_debug (" [%s] webdav: %d, collection %d [res: %d]\n",
              mount_base->path, is_webdav, is_collection, res);

    if (is_collection == FALSE)
      break;
    
    /* we have found a new good root, try the parent ... */
    g_free (last_good_path);
    last_good_path = mount_base->path;
    mount_base->path = path_get_parent_dir (mount_base->path);
    soup_message_set_uri (msg_opts, mount_base);

    if (auth_interactive)
      {
         /* if we have found a root that is good then we assume
            that we also have obtained to correct credentials
            and we switch the auth handler. This will prevent us
            from asking for *different* credentials *again* if the
            server should response with 401 for some of the parent
            collections. See also bug #677753 */

         g_signal_handler_disconnect (session, signal_id);
         g_signal_connect (session, "authenticate",
                           G_CALLBACK (soup_authenticate_from_data),
                           data);
         auth_interactive = FALSE;
       }

    soup_message_headers_clear (msg_stat->response_headers);
    soup_message_body_truncate (msg_stat->response_body);

  } while (g_strcmp0 (last_good_path, "/") != 0);

  /* we either encountered an error or we have
     reached the end of paths we are allowed to
     chdir up to (or couldn't chdir up at all) */

  /* check if we at all have a good path */
  if (last_good_path == NULL)
    {
      if ((is_success && !is_webdav) ||
          msg_opts->status_code == SOUP_STATUS_METHOD_NOT_ALLOWED)
        {
          /* This means the either: a) OPTIONS request succeeded
             (which should be the case even for non-existent
             resources on a webdav enabled share) but we did not
             get the DAV header. Or b) the OPTIONS request was a
             METHOD_NOT_ALLOWED (405).
             Prioritize this error messages, because it seems most
             useful to the user. */
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR, G_IO_ERROR_FAILED,
                            _("Not a WebDAV enabled share"));
        }
      else if (!is_success || !res)
        {
          /* Either the OPTIONS request (is_success) or the PROPFIND
             request (res) failed. */
          SoupMessage *target = !is_success ? msg_opts : msg_stat;
          int error_code = http_error_code_from_status (target->status_code);

          if (error_code == G_IO_ERROR_CANCELLED)
            error_code = G_IO_ERROR_FAILED_HANDLED;

          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR, error_code,
                            _("HTTP Error: %s"), target->reason_phrase);
        }
      else
        {
          /* This means, we have a valid DAV header, PROPFIND worked,
             but it is not a collection!  */
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR, G_IO_ERROR_FAILED,
                            _("Could not find an enclosing directory"));
        }

      g_object_unref (msg_opts);
      g_object_unref (msg_stat);

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
  g_vfs_backend_set_symbolic_icon_name (backend, "folder-remote-symbolic");
  
  g_vfs_backend_dav_setup_display_name (backend);
  
  /* cleanup */
  g_mount_spec_unref (mount_spec);
  g_object_unref (msg_opts);
  g_object_unref (msg_stat);

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

      if (response.is_target)
        {
          ms_response_to_file_info (&response, job->file_info);
          res = TRUE;
        }

      ms_response_clear (&response);
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

static PropName fs_info_propnames[] = {
  {"quota-available-bytes", NULL},
  {"quota-used-bytes",      NULL},
  {NULL,                    NULL}
};

static void
do_query_fs_info (GVfsBackend           *backend,
                  GVfsJobQueryFsInfo    *job,
                  const char            *filename,
                  GFileInfo             *info,
                  GFileAttributeMatcher *attribute_matcher)
{
  SoupMessage *msg;
  Multistatus  ms;
  xmlNodeIter  iter;
  gboolean     res;
  GError      *error;

  g_file_info_set_attribute_string (info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
                                    "webdav");
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE,
                                     TRUE);

  if (! (g_file_attribute_matcher_matches (attribute_matcher,
                                           G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
         g_file_attribute_matcher_matches (attribute_matcher,
                                           G_FILE_ATTRIBUTE_FILESYSTEM_USED) ||
         g_file_attribute_matcher_matches (attribute_matcher,
                                           G_FILE_ATTRIBUTE_FILESYSTEM_FREE)))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
      return;
    }

  msg = propfind_request_new (backend, filename, 0, fs_info_propnames);

  if (msg == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Could not create request"));

      return;
    }

  g_vfs_backend_dav_send_message (backend, msg);

  error = NULL;
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

      if (response.is_target)
        {
          ms_response_to_fs_info (&response, info);
          res = TRUE;
        }

      ms_response_clear (&response);
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
  g_vfs_job_succeeded (G_VFS_JOB (job));

  multistatus_get_response_iter (&ms, &iter);

  while (xml_node_iter_next (&iter))
    {
      MsResponse  response;
      GFileInfo  *info;

      if (! multistatus_get_response (&iter, &response))
        continue;

      if (response.is_target == FALSE)
	{
	  info = g_file_info_new ();
	  ms_response_to_file_info (&response, info);
	  g_vfs_job_enumerate_add_info (job, info);
          g_object_unref (info);
	}

      ms_response_clear (&response);
    }

  multistatus_free (&ms);
  g_object_unref (msg);

  g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
}

/* ************************************************************************* */
/*  */

/* *** open () *** */
static void
try_open_stat_done (SoupSession *session,
		    SoupMessage *msg,
		    gpointer     user_data)
{
  GVfsJob         *job = G_VFS_JOB (user_data);
  GVfsBackend     *backend = job->backend_data;
  GFileType        target_type;
  SoupURI         *uri;
  gboolean         res;

  if (msg->status_code != 207)
    {
      http_job_failed (job, msg);
      return;
    }

  res = stat_location_finish (msg, &target_type, NULL, NULL);

  if (res == FALSE)
    {
      g_vfs_job_failed (job,
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Response invalid"));
      return;
    }

  if (target_type == G_FILE_TYPE_DIRECTORY)
    {
      g_vfs_job_failed (job,
			G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
			_("File is directory"));
      return;
    }

  uri = soup_message_get_uri (msg);

  http_backend_open_for_read (backend, job, uri);
}


static gboolean
try_open_for_read (GVfsBackend        *backend,
                   GVfsJobOpenForRead *job,
                   const char         *filename)
{
  SoupMessage     *msg;
  SoupURI         *uri;

  uri = g_vfs_backend_dav_uri_for_path (backend, filename, FALSE);
  msg = stat_location_begin (uri, FALSE);
  soup_uri_free (uri);

  if (msg == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Could not create request"));

      return FALSE;
    }

  g_vfs_job_set_backend_data (G_VFS_JOB (job), backend, NULL);
  g_vfs_backend_dav_queue_message (backend, msg, try_open_stat_done, job);

  return TRUE;
}




/* *** create () *** */
static void
try_create_tested_existence (SoupSession *session, SoupMessage *msg,
                             gpointer user_data)
{
  GVfsJob *job = G_VFS_JOB (user_data);
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
  stream = g_memory_output_stream_new (NULL, 0, g_try_realloc, g_free);
  g_object_set_data_full (G_OBJECT (stream), "-gvfs-stream-msg", put_msg, g_object_unref);

  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), stream);
  g_vfs_job_open_for_write_set_can_seek (G_VFS_JOB_OPEN_FOR_WRITE (job),
                                         g_seekable_can_seek (G_SEEKABLE (stream)));
  g_vfs_job_open_for_write_set_can_truncate (G_VFS_JOB_OPEN_FOR_WRITE (job),
                                             g_seekable_can_truncate (G_SEEKABLE (stream)));
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

  /* TODO: if we supported chunked requests, we could
   * use a PUT with "If-None-Match: *" and "Expect: 100-continue"
   */
  uri = g_vfs_backend_dav_uri_for_path (backend, filename, FALSE);
  msg = soup_message_new_from_uri (SOUP_METHOD_HEAD, uri);
  soup_uri_free (uri);

  g_vfs_job_set_backend_data (G_VFS_JOB (job), backend, NULL);

  g_vfs_backend_dav_queue_message (backend, msg, try_create_tested_existence, job);
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

  stream = g_memory_output_stream_new (NULL, 0, g_try_realloc, g_free);
  g_object_set_data_full (G_OBJECT (stream), "-gvfs-stream-msg", put_msg, g_object_unref);

  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), stream);
  g_vfs_job_open_for_write_set_can_seek (G_VFS_JOB_OPEN_FOR_WRITE (job),
                                         g_seekable_can_seek (G_SEEKABLE (stream)));
  g_vfs_job_open_for_write_set_can_truncate (G_VFS_JOB_OPEN_FOR_WRITE (job),
                                             g_seekable_can_truncate (G_SEEKABLE (stream)));
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
                              soup_message_headers_get_one (msg->request_headers, "If-Match"));
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



  uri = g_vfs_backend_dav_uri_for_path (backend, filename, FALSE);

  if (etag)
    {
      SoupMessage *msg;

      msg = soup_message_new_from_uri (SOUP_METHOD_HEAD, uri);
      soup_uri_free (uri);
      soup_message_headers_append (msg->request_headers, "If-Match", etag);

      g_vfs_job_set_backend_data (G_VFS_JOB (job), op_backend, NULL);
      g_vfs_backend_dav_queue_message (backend, msg,
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

static void
do_seek_on_write (GVfsBackend *backend,
                  GVfsJobSeekWrite *job,
                  GVfsBackendHandle handle,
                  goffset offset,
                  GSeekType type)
{
  GSeekable *stream = G_SEEKABLE (handle);
  GError *error = NULL;

  if (g_seekable_seek (stream, offset, type, G_VFS_JOB (job)->cancellable, &error))
    {
      g_vfs_job_seek_write_set_offset (job, g_seekable_tell (stream));
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static void
do_truncate (GVfsBackend *backend,
             GVfsJobTruncate *job,
             GVfsBackendHandle handle,
             goffset size)
{
  GSeekable *stream = G_SEEKABLE (handle);
  GError *error = NULL;

  if (g_seekable_truncate (stream, size, G_VFS_JOB (job)->cancellable, &error))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

/* *** close_write () *** */
static void
try_close_write_sent (SoupSession *session,
		      SoupMessage *msg,
		      gpointer     user_data)
{
  GVfsJob *job;

  job = G_VFS_JOB (user_data);
  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    http_job_failed (job, msg);
  else
    g_vfs_job_succeeded (job);
}

static gboolean
try_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle handle)
{
  GOutputStream *stream;
  SoupMessage *msg;
  gsize length;
  gchar *data;

  stream = G_OUTPUT_STREAM (handle);

  msg = g_object_get_data (G_OBJECT (stream), "-gvfs-stream-msg");
  g_object_ref (msg);
  g_object_set_data (G_OBJECT (stream), "-gvfs-stream-msg", NULL);

  g_output_stream_close (stream, NULL, NULL);
  length = g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (stream));
  data = g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (stream));
  g_object_unref (stream);

  soup_message_body_append (msg->request_body, SOUP_MEMORY_TAKE, data, length);
  g_vfs_backend_dav_queue_message (backend, msg,
                                   try_close_write_sent, job);

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

  uri = g_vfs_backend_dav_uri_for_path (backend, filename, TRUE);
  msg = soup_message_new_from_uri (SOUP_METHOD_MKCOL, uri);
  soup_uri_free (uri);

  status = g_vfs_backend_dav_send_message (backend, msg);

  if (! SOUP_STATUS_IS_SUCCESSFUL (status))
    if (status == SOUP_STATUS_METHOD_NOT_ALLOWED)
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
    else
      http_job_failed (G_VFS_JOB (job), msg);
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

  uri = g_vfs_backend_dav_uri_for_path (backend, filename, FALSE);
  res = stat_location (backend, uri, &file_type, NULL, &num_children, &error);

  if (res == FALSE)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      soup_uri_free (uri);
      return;
    }

  if (file_type == G_FILE_TYPE_DIRECTORY && num_children)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_EMPTY,
                        _("Directory not empty"));
      soup_uri_free (uri);
      return;
    }

  msg = soup_message_new_from_uri (SOUP_METHOD_DELETE, uri);

  status = g_vfs_backend_dav_send_message (backend, msg);

  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    http_job_failed (G_VFS_JOB (job), msg);
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

  source = g_vfs_backend_dav_uri_for_path (backend, filename, FALSE);
  msg = soup_message_new_from_uri (SOUP_METHOD_MOVE, source);

  dirname = g_path_get_dirname (filename);
  target_path = g_build_filename (dirname, display_name, NULL);
  target = g_vfs_backend_dav_uri_for_path (backend, target_path, FALSE);

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
    http_job_failed (G_VFS_JOB (job), msg);

  g_object_unref (msg);
  g_free (dirname);
  g_free (target_path);
  soup_uri_free (target);
  soup_uri_free (source);
}

static void
do_move (GVfsBackend *backend,
         GVfsJobMove *job,
         const char *source,
         const char *destination,
         GFileCopyFlags flags,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  SoupMessage *msg;
  SoupURI *source_uri;
  SoupURI *target_uri;
  guint status;
  GFileType source_ft, target_ft;
  GError *error = NULL;
  gboolean res, stat_res;
  gint64 file_size;

  if (flags & G_FILE_COPY_BACKUP)
    {
      if (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE)
        {
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_CANT_CREATE_BACKUP,
                                    _("Backups not supported"));
        }
      else
        {
          /* Return G_IO_ERROR_NOT_SUPPORTED instead of G_IO_ERROR_CANT_CREATE_BACKUP
           * to be proceeded with copy and delete fallback (see g_file_move). */
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_NOT_SUPPORTED,
                                    "Operation not supported");
        }

      return;
    }

  source_uri = g_vfs_backend_dav_uri_for_path (backend, source, FALSE);
  msg = soup_message_new_from_uri (SOUP_METHOD_MOVE, source_uri);
  target_uri = g_vfs_backend_dav_uri_for_path (backend, destination, FALSE);

  res = stat_location (backend, target_uri, &target_ft, NULL, NULL, &error);
  if (!res && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto error;
    }
  g_clear_error (&error);

  stat_res = stat_location (backend, source_uri, &source_ft, &file_size, NULL, &error);
  if (res)
    {
      if (flags & G_FILE_COPY_OVERWRITE)
        {
          if (stat_res)
            {
              if (target_ft == G_FILE_TYPE_DIRECTORY)
                {
                  if (source_ft == G_FILE_TYPE_DIRECTORY)
                    g_vfs_job_failed_literal (G_VFS_JOB(job),
                                              G_IO_ERROR,
                                              G_IO_ERROR_WOULD_MERGE,
                                              _("Can’t move directory over directory"));
                  else
                    g_vfs_job_failed_literal (G_VFS_JOB(job),
                                              G_IO_ERROR,
                                              G_IO_ERROR_IS_DIRECTORY,
                                              _("Can’t move over directory"));
                  goto error;
                }
              else if (source_ft == G_FILE_TYPE_DIRECTORY)
                {
                  /* Overwriting a file with a directory, first remove the
                   * file */
                  SoupMessage *msg;

                  msg = soup_message_new_from_uri (SOUP_METHOD_DELETE,
                                                   target_uri);
                  status = g_vfs_backend_dav_send_message (backend, msg);

                  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
                    {
                      http_job_failed (G_VFS_JOB (job), msg);
                      g_object_unref (msg);
                      goto error;
                    }
                  g_object_unref (msg);
                }
            }
          else
            {
              g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
              goto error;
            }
        }
      else
        {
          g_vfs_job_failed_literal (G_VFS_JOB(job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_EXISTS,
                                    _("Target file exists"));
          goto error;
        }
    }

  message_add_destination_header (msg, target_uri);
  message_add_overwrite_header (msg, flags & G_FILE_COPY_OVERWRITE);

  status = g_vfs_backend_dav_send_message (backend, msg);

  /* See do_set_display_name () for the explanation of the PRECONDITION_FAILED
   * and IS_REDIRECTION handling below. */

  if (SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      if (stat_res && progress_callback)
        progress_callback (file_size, file_size, progress_callback_data);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else if (status == SOUP_STATUS_PRECONDITION_FAILED ||
           SOUP_STATUS_IS_REDIRECTION (status))
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_EXISTS,
                      _("Target file already exists"));
  else
    http_job_failed (G_VFS_JOB (job), msg);

error:
  g_object_unref (msg);
  g_clear_error (&error);
  soup_uri_free (source_uri);
  soup_uri_free (target_uri);
}

static void
do_copy (GVfsBackend *backend,
         GVfsJobCopy *job,
         const char *source,
         const char *destination,
         GFileCopyFlags flags,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  SoupMessage *msg;
  SoupURI *source_uri;
  SoupURI *target_uri;
  guint status;
  GFileType source_ft, target_ft;
  GError *error = NULL;
  gboolean res;
  gint64 file_size;

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* Return G_IO_ERROR_NOT_SUPPORTED instead of
       * G_IO_ERROR_CANT_CREATE_BACKUP to proceed with the GIO fallback
       * copy. */
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_SUPPORTED,
                                "Operation not supported");
      return;
    }

  source_uri = g_vfs_backend_dav_uri_for_path (backend, source, FALSE);
  target_uri = g_vfs_backend_dav_uri_for_path (backend, destination, FALSE);

  res = stat_location (backend, source_uri, &source_ft, &file_size, NULL, &error);
  if (!res)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto error;
    }

  res = stat_location (backend, target_uri, &target_ft, NULL, NULL, &error);
  if (res)
    {
      if (flags & G_FILE_COPY_OVERWRITE)
        {
          if (target_ft == G_FILE_TYPE_DIRECTORY)
            {
              if (source_ft == G_FILE_TYPE_DIRECTORY)
                g_vfs_job_failed_literal (G_VFS_JOB(job),
                                          G_IO_ERROR,
                                          G_IO_ERROR_WOULD_MERGE,
                                          _("Can’t copy directory over directory"));
              else
                g_vfs_job_failed_literal (G_VFS_JOB(job),
                                          G_IO_ERROR,
                                          G_IO_ERROR_IS_DIRECTORY,
                                          _("File is directory"));
              goto error;
            }
        }
      else
        {
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_EXISTS,
                                    _("Target file already exists"));
          goto error;
        }
    }
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto error;
    }

  if (source_ft == G_FILE_TYPE_DIRECTORY)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR,
                                G_IO_ERROR_WOULD_RECURSE,
                                _("Can’t recursively copy directory"));
      goto error;
    }

  msg = soup_message_new_from_uri (SOUP_METHOD_COPY, source_uri);
  message_add_destination_header (msg, target_uri);
  message_add_overwrite_header (msg, flags & G_FILE_COPY_OVERWRITE);

  status = g_vfs_backend_dav_send_message (backend, msg);

  /* See do_set_display_name () for the explanation of the PRECONDITION_FAILED
   * and IS_REDIRECTION handling below. */

  if (SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      if (progress_callback)
        progress_callback (file_size, file_size, progress_callback_data);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else if (status == SOUP_STATUS_PRECONDITION_FAILED ||
           SOUP_STATUS_IS_REDIRECTION (status))
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_EXISTS,
                      _("Target file already exists"));
  else
    http_job_failed (G_VFS_JOB (job), msg);

  g_object_unref (msg);

error:
  g_clear_error (&error);
  soup_uri_free (source_uri);
  soup_uri_free (target_uri);
}

#define CHUNK_SIZE 65536

/* Used to keep track of the state of reads in flight when the restarted signal
 * is received. */
typedef enum {
  PUSH_READ_STATUS_NONE,
  PUSH_READ_STATUS_RESET,
  PUSH_READ_STATUS_DEFERRED,
} PushReadStatus;

typedef struct {
  /* Job details */
  GVfsBackend *backend;
  GVfsJob *job;
  GVfsJobPush *op_job;

  /* Local file */
  GInputStream *in;
  unsigned char *buf;
  goffset size;
  goffset n_read;
  PushReadStatus read_status;

  /* Remote file */
  SoupURI *uri;
  SoupMessage *msg;
  goffset n_written;
} PushHandle;

static void
push_write_next_chunk (SoupMessage *msg, gpointer user_data);

static void
push_handle_free (PushHandle *handle)
{
  if (handle->in)
    {
      g_input_stream_close_async (handle->in, 0, NULL, NULL, NULL);
      g_object_unref (handle->in);
    }
  g_object_unref (handle->backend);
  g_object_unref (handle->job);
  soup_uri_free (handle->uri);
  g_slice_free (PushHandle, handle);
}

static void
push_read_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PushHandle *handle = user_data;
  GError *error = NULL;
  gssize n;

  n = g_input_stream_read_finish (handle->in, res, &error);

  /* Ignore this read if we've subsequently been restarted. */
  if (handle->read_status != PUSH_READ_STATUS_NONE)
    {
      g_free (handle->buf);
      handle->buf = NULL;

      /* Queue another read if we've been subsequently restarted and
       * push_write_next_chunk () was called in the meantime. */
      if (handle->read_status == PUSH_READ_STATUS_DEFERRED)
        push_write_next_chunk (handle->msg, handle);

      return;
    }

  if (n > 0)
    {
      soup_message_body_append_take (handle->msg->request_body, handle->buf, n);
      handle->buf = NULL;
      handle->n_read += n;
      soup_session_unpause_message (G_VFS_BACKEND_HTTP (handle->backend)->session,
                                    handle->msg);
    }
  else if (n == 0)
    {
      g_free (handle->buf);
      handle->buf = NULL;

      if (handle->n_read != handle->size)
        {
          g_vfs_job_failed_literal (handle->job,
                                    G_IO_ERROR,
                                    G_IO_ERROR_FAILED,
                                    _("File length changed during transfer"));

          soup_session_cancel_message (G_VFS_BACKEND_HTTP (handle->backend)->session,
                                       handle->msg,
                                       SOUP_STATUS_CANCELLED);
        }
    }
  else
    {
      g_free (handle->buf);
      handle->buf = NULL;
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
      soup_session_cancel_message (G_VFS_BACKEND_HTTP (handle->backend)->session,
                                   handle->msg,
                                   SOUP_STATUS_CANCELLED);
    }
}

static void
push_write_next_chunk (SoupMessage *msg, gpointer user_data)
{
  PushHandle *handle = user_data;

  /* If we've been restarted, seek to the beginning of the file. */
  if (handle->read_status == PUSH_READ_STATUS_RESET)
    {
      GError *error = NULL;

      /* We've been restarted but there's still a read in flight, so defer. */
      if (handle->buf)
        {
          handle->read_status = PUSH_READ_STATUS_DEFERRED;
          return;
        }

      handle->n_read = 0;
      handle->n_written = 0;
      handle->read_status = PUSH_READ_STATUS_NONE;

      if (!g_seekable_seek (G_SEEKABLE (handle->in),
                            0, G_SEEK_SET,
                            handle->job->cancellable, &error))
        {
          g_vfs_job_failed_from_error (handle->job, error);
          g_error_free (error);
          soup_session_cancel_message (G_VFS_BACKEND_HTTP (handle->backend)->session,
                                       handle->msg,
                                       SOUP_STATUS_CANCELLED);
          return;
        }
    }

  handle->buf = g_malloc (CHUNK_SIZE);
  g_input_stream_read_async (handle->in,
                             handle->buf, CHUNK_SIZE,
                             0, handle->job->cancellable,
                             push_read_cb, handle);
}

static void
push_setup_message (PushHandle *handle)
{
  soup_message_set_flags (handle->msg, SOUP_MESSAGE_CAN_REBUILD);
  soup_message_body_set_accumulate (handle->msg->request_body, FALSE);
  message_add_overwrite_header (handle->msg,
                                handle->op_job->flags & G_FILE_COPY_OVERWRITE);
  soup_message_headers_set_encoding (handle->msg->request_headers,
                                     SOUP_ENCODING_CONTENT_LENGTH);
  soup_message_headers_set_content_length (handle->msg->request_headers,
                                           handle->size);
}

static void
push_restarted (SoupMessage *msg, gpointer user_data)
{
  PushHandle *handle = user_data;

  handle->read_status = PUSH_READ_STATUS_RESET;
	msg->method = SOUP_METHOD_PUT;
  push_setup_message (handle);
}

static void
push_wrote_body_data (SoupMessage *msg, SoupBuffer *chunk, gpointer user_data)
{
  PushHandle *handle = user_data;

  handle->n_written += chunk->length;
  g_vfs_job_progress_callback (handle->n_written, handle->size, handle->job);
}

static void
push_done (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
  PushHandle *handle = user_data;

  if (g_vfs_job_is_finished (handle->job))
    ; /* We got an error so we finished the job and cancelled msg. */
  else if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    http_job_failed (handle->job, msg);
  else
    {
      if (handle->op_job->remove_source)
        g_unlink (handle->op_job->local_path);

      g_vfs_job_succeeded (handle->job);
    }

  push_handle_free (handle);
}

static void
push_stat_dest_cb (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
  PushHandle *handle = user_data;
  GFileType type;

  if (stat_location_finish (msg, &type, NULL, NULL))
    {
      if (!(handle->op_job->flags & G_FILE_COPY_OVERWRITE))
        {
          g_vfs_job_failed (handle->job,
                            G_IO_ERROR,
                            G_IO_ERROR_EXISTS,
                            _("Target file already exists"));
          push_handle_free (handle);
          return;
        }
      if (type == G_FILE_TYPE_DIRECTORY)
        {
          g_vfs_job_failed (handle->job,
                            G_IO_ERROR,
                            G_IO_ERROR_IS_DIRECTORY,
                            _("File is directory"));
          push_handle_free (handle);
          return;
        }
    }

  handle->msg = soup_message_new_from_uri (SOUP_METHOD_PUT, handle->uri);
  push_setup_message (handle);

  g_signal_connect (handle->msg, "restarted",
                    G_CALLBACK (push_restarted), handle);
  g_signal_connect (handle->msg, "wrote_headers",
                    G_CALLBACK (push_write_next_chunk), handle);
  g_signal_connect (handle->msg, "wrote_chunk",
                    G_CALLBACK (push_write_next_chunk), handle);
  g_signal_connect (handle->msg, "wrote-body-data",
                    G_CALLBACK (push_wrote_body_data), handle);

  g_vfs_backend_dav_queue_message (handle->backend, handle->msg,
                                   push_done, handle);
}

static void
push_source_fstat_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFileInputStream *fin = G_FILE_INPUT_STREAM (source);
  PushHandle *handle = user_data;
  GError *error = NULL;
  GFileInfo *info;

  info = g_file_input_stream_query_info_finish (fin, res, &error);
  if (info)
    {
      SoupMessage *msg;

      handle->size = g_file_info_get_size (info);
      g_object_unref (info);

      msg = stat_location_begin (handle->uri, FALSE);
      g_vfs_backend_dav_queue_message (handle->backend, msg,
                                       push_stat_dest_cb, handle);
    }
  else
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
      push_handle_free (handle);
    }
}

static void
push_source_open_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *source_file = G_FILE (source);
  PushHandle *handle = user_data;
  GError *error = NULL;
  GFileInputStream *fin;

  fin = g_file_read_finish (source_file, res, &error);
  if (fin)
    {
      handle->in = G_INPUT_STREAM (fin);

      g_file_input_stream_query_info_async (fin,
                                            G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                            0, handle->job->cancellable,
                                            push_source_fstat_cb, handle);
    }
  else
    {
      if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_IS_DIRECTORY)
        {
          /* Fall back to default implementation to improve the error message */
          g_vfs_job_failed (handle->job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            _("Not supported"));
        }
      else
        g_vfs_job_failed_from_error (handle->job, error);

      g_error_free (error);
      push_handle_free (handle);
    }
}

static void
push_source_lstat_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *source_file = G_FILE (source);
  PushHandle *handle = user_data;
  GError *error = NULL;
  GFileInfo *info;

  info = g_file_query_info_finish (source_file, res, &error);
  if (!info)
    {
      g_vfs_job_failed_from_error (handle->job, error);
      g_error_free (error);
      push_handle_free (handle);
      return;
    }

  if ((handle->op_job->flags & G_FILE_COPY_NOFOLLOW_SYMLINKS) &&
      g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      /* Fall back to default implementation to copy symlink */
      g_vfs_job_failed (handle->job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        _("Not supported"));
      push_handle_free (handle);
      g_object_unref (info);
      return;
    }

  g_file_read_async (source_file,
                     0, handle->job->cancellable,
                     push_source_open_cb, handle);
  g_object_unref (info);
}

static gboolean
try_push (GVfsBackend *backend,
          GVfsJobPush *job,
          const char *destination,
          const char *local_path,
          GFileCopyFlags flags,
          gboolean remove_source,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GFile *source;
  PushHandle *handle;

  handle = g_slice_new0 (PushHandle);
  handle->backend = g_object_ref (backend);
  handle->job = g_object_ref (G_VFS_JOB (job));
  handle->op_job = job;
  handle->uri = g_vfs_backend_dav_uri_for_path (backend, destination, FALSE);

  source = g_file_new_for_path (local_path);
  g_file_query_info_async (source,
                           G_FILE_ATTRIBUTE_STANDARD_TYPE,
                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                           0, handle->job->cancellable,
                           push_source_lstat_cb, handle);
  g_object_unref (source);

  return TRUE;
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
  backend_class->try_query_fs_info = NULL;
  backend_class->query_fs_info     = do_query_fs_info;
  backend_class->enumerate         = do_enumerate;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_create        = try_create;
  backend_class->try_replace       = try_replace;
  backend_class->try_write         = try_write;
  backend_class->seek_on_write     = do_seek_on_write;
  backend_class->truncate          = do_truncate;
  backend_class->try_close_write   = try_close_write;
  backend_class->make_directory    = do_make_directory;
  backend_class->delete            = do_delete;
  backend_class->set_display_name  = do_set_display_name;
  backend_class->move              = do_move;
  backend_class->copy              = do_copy;
  backend_class->try_push          = try_push;
}
