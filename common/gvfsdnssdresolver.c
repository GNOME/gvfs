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
 * Author: David Zeuthen <davidz@redhat.com>
 */

/*
 * TODO: - locking
 *       - cancellation
 *       - get rid of g_main_loop (bug 555436#c32)
 */

#include <config.h>
#include <string.h>
#include <glib/gi18n-lib.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>
#include <avahi-glib/glib-watch.h>
#include <avahi-glib/glib-malloc.h>

#include "gvfsdnssdutils.h"
#include "gvfsdnssdresolver.h"

enum
{
  PROP_0,
  PROP_ENCODED_TRIPLE,
  PROP_REQUIRED_TXT_KEYS,
  PROP_SERVICE_NAME,
  PROP_SERVICE_TYPE,
  PROP_DOMAIN,
  PROP_TIMEOUT_MSEC,

  PROP_IS_RESOLVED,
  PROP_ADDRESS,
  PROP_PORT,
  PROP_TXT_RECORDS,
};

enum {
    CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _GVfsDnsSdResolver
{
  GObject parent_instance;

  char *encoded_triple;
  char *service_name;
  char *service_type;
  char *domain;
  char *required_txt_keys;
  char **required_txt_keys_broken_out;
  guint timeout_msec;

  gboolean is_resolved;
  char *address;
  guint port;
  char **txt_records;

  AvahiServiceResolver *avahi_resolver;
};


struct _GVfsDnsSdResolverClass
{
  GObjectClass parent_class;

  /* signals */
  void (*changed) (GVfsDnsSdResolver *resolver);
};

G_DEFINE_TYPE (GVfsDnsSdResolver, g_vfs_dns_sd_resolver, G_TYPE_OBJECT);

static gboolean resolver_supports_mdns = FALSE;
static AvahiClient *global_client = NULL;
static gboolean avahi_initialized = FALSE;
static void free_global_avahi_client (void);
static AvahiClient *get_global_avahi_client (GError **error);

static gboolean ensure_avahi_resolver (GVfsDnsSdResolver  *resolver,
                                       GError            **error);

static void service_resolver_cb (AvahiServiceResolver   *resolver,
                                 AvahiIfIndex            interface,
                                 AvahiProtocol           protocol,
                                 AvahiResolverEvent      event,
                                 const char             *name,
                                 const char             *type,
                                 const char             *domain,
                                 const char             *host_name,
                                 const AvahiAddress     *a,
                                 uint16_t                port,
                                 AvahiStringList        *txt,
                                 AvahiLookupResultFlags  flags,
                                 void                   *user_data);



static GList *resolvers = NULL;

static void
clear_avahi_data (GVfsDnsSdResolver *resolver);

static void
remove_client_from_resolver (GVfsDnsSdResolver *resolver)
{
  if (resolver->avahi_resolver != NULL)
    {
      avahi_service_resolver_free (resolver->avahi_resolver);
      resolver->avahi_resolver = NULL;
    }

  clear_avahi_data (resolver);
}

static void
add_client_to_resolver (GVfsDnsSdResolver *resolver)
{
  ensure_avahi_resolver (resolver, NULL);
}

/* Callback for state changes on the Client */
static void
avahi_client_callback (AvahiClient *client, AvahiClientState state, void *userdata)
{
  if (global_client == NULL)
    global_client = client;

  if (state == AVAHI_CLIENT_FAILURE)
    {
      if (avahi_client_errno (client) == AVAHI_ERR_DISCONNECTED)
        {
          free_global_avahi_client ();

          /* Attempt to reconnect */
          get_global_avahi_client (NULL);
        }
    }
  else if (state == AVAHI_CLIENT_S_RUNNING)
    {
      /* Start resolving again */
      g_list_foreach (resolvers, (GFunc) add_client_to_resolver, NULL);
    }
}

static void
free_global_avahi_client (void)
{
  /* Remove current resolvers */
  g_list_foreach (resolvers, (GFunc) remove_client_from_resolver, NULL);

  /* Destroy client */
  avahi_client_free (global_client);
  global_client = NULL;
  avahi_initialized = FALSE;
}

static AvahiClient *
get_global_avahi_client (GError **error)
{
  static AvahiGLibPoll *glib_poll = NULL;
  int avahi_error;

  if (!avahi_initialized)
    {
      avahi_initialized = TRUE;

      if (glib_poll == NULL)
        {
          avahi_set_allocator (avahi_glib_allocator ());
          glib_poll = avahi_glib_poll_new (NULL, G_PRIORITY_DEFAULT);
        }

      /* Create a new AvahiClient instance */
      global_client = avahi_client_new (avahi_glib_poll_get (glib_poll),
                                        AVAHI_CLIENT_NO_FAIL,
                                        avahi_client_callback,
                                        glib_poll,
                                        &avahi_error);

      if (global_client == NULL)
        {
          g_set_error (error,
                       G_IO_ERROR,
                       G_IO_ERROR_FAILED,
                       _("Error initializing Avahi: %s"),
                       avahi_strerror (avahi_error));
          goto out;
        }
    }

 out:

  return global_client;
}


static gboolean
ensure_avahi_resolver (GVfsDnsSdResolver  *resolver,
                       GError            **error)
{
  AvahiClient *avahi_client;
  gboolean ret;

  ret = FALSE;

  if (resolver->avahi_resolver != NULL)
    {
      ret = TRUE;
      goto out;
    }

  avahi_client = get_global_avahi_client (error);
  if (avahi_client == NULL)
    goto out;

  resolver->avahi_resolver = avahi_service_resolver_new (avahi_client,
                                                         AVAHI_IF_UNSPEC,
                                                         AVAHI_PROTO_UNSPEC,
                                                         resolver->service_name,
                                                         resolver->service_type,
                                                         resolver->domain,
                                                         AVAHI_PROTO_UNSPEC,
                                                         0, /* AvahiLookupFlags */
                                                         service_resolver_cb,
                                                         resolver);
  if (resolver->avahi_resolver == NULL)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   _("Error creating Avahi resolver: %s"),
                   avahi_strerror (avahi_client_errno (avahi_client)));
      goto out;
    }

  ret = TRUE;

out:
  return ret;
}

static void
g_vfs_dns_sd_resolver_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  GVfsDnsSdResolver *resolver = G_VFS_DNS_SD_RESOLVER (object);

  switch (prop_id)
    {
    case PROP_ENCODED_TRIPLE:
      g_value_set_string (value, resolver->encoded_triple);
      break;

    case PROP_REQUIRED_TXT_KEYS:
      g_value_set_string (value, resolver->required_txt_keys);
      break;

    case PROP_SERVICE_NAME:
      g_value_set_string (value, resolver->service_name);
      break;

    case PROP_SERVICE_TYPE:
      g_value_set_string (value, resolver->service_type);
      break;

    case PROP_DOMAIN:
      g_value_set_string (value, resolver->domain);
      break;

    case PROP_TIMEOUT_MSEC:
      g_value_set_uint (value, resolver->timeout_msec);
      break;

    case PROP_IS_RESOLVED:
      g_value_set_boolean (value, resolver->is_resolved);
      break;

    case PROP_ADDRESS:
      g_value_set_string (value, resolver->address);
      break;

    case PROP_PORT:
      g_value_set_uint (value, resolver->port);
      break;

    case PROP_TXT_RECORDS:
      g_value_set_boxed (value, resolver->txt_records);
      break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
g_vfs_dns_sd_resolver_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  GVfsDnsSdResolver *resolver = G_VFS_DNS_SD_RESOLVER (object);

  switch (prop_id)
    {
    case PROP_ENCODED_TRIPLE:
      resolver->encoded_triple = g_strdup (g_value_get_string (value));
      break;

    case PROP_REQUIRED_TXT_KEYS:
      resolver->required_txt_keys = g_strdup (g_value_get_string (value));
      if (resolver->required_txt_keys != NULL)
        {
          /* TODO: maybe support escaping ',' */
          resolver->required_txt_keys_broken_out = g_strsplit (resolver->required_txt_keys, ",", 0);
        }
      break;

    case PROP_SERVICE_NAME:
      resolver->service_name = g_strdup (g_value_get_string (value));
      break;

    case PROP_SERVICE_TYPE:
      resolver->service_type = g_strdup (g_value_get_string (value));
      break;

    case PROP_DOMAIN:
      resolver->domain = g_strdup (g_value_get_string (value));
      break;

    case PROP_TIMEOUT_MSEC:
      resolver->timeout_msec = g_value_get_uint (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
g_vfs_dns_sd_resolver_finalize (GObject *object)
{
  GVfsDnsSdResolver *resolver;

  resolver = G_VFS_DNS_SD_RESOLVER (object);

  g_free (resolver->encoded_triple);
  g_free (resolver->service_name);
  g_free (resolver->service_type);
  g_free (resolver->domain);
  g_free (resolver->required_txt_keys);
  g_strfreev (resolver->required_txt_keys_broken_out);

  g_free (resolver->address);
  g_strfreev (resolver->txt_records);

  if (resolver->avahi_resolver != NULL)
    avahi_service_resolver_free (resolver->avahi_resolver);


  resolvers = g_list_remove (resolvers, resolver);

  /* free the global avahi client for the last resolver */
  if (resolvers == NULL)
    {
      free_global_avahi_client ();
    }

  G_OBJECT_CLASS (g_vfs_dns_sd_resolver_parent_class)->finalize (object);
}

static void
g_vfs_dns_sd_resolver_constructed (GObject *object)
{
  GVfsDnsSdResolver *resolver;

  resolver = G_VFS_DNS_SD_RESOLVER (object);

  if (resolver->encoded_triple != NULL)
    {
      GError *error;

      if (resolver->service_name != NULL)
        {
          g_warning ("Ignoring service-name since encoded-triple is already set");
          g_free (resolver->service_name);
          resolver->service_name = NULL;
        }

      if (resolver->service_type != NULL)
        {
          g_warning ("Ignoring service-type since encoded-triple is already set");
          g_free (resolver->service_type);
          resolver->service_type = NULL;
        }

      if (resolver->domain != NULL)
        {
          g_warning ("Ignoring domain since encoded-triple is already set");
          g_free (resolver->domain);
          resolver->domain = NULL;
        }


      error = NULL;
      if (!g_vfs_decode_dns_sd_triple (resolver->encoded_triple,
                                       &(resolver->service_name),
                                       &(resolver->service_type),
                                       &(resolver->domain),
                                       &error))
        {
          /* GObject construction can't fail. So whine if the triple isn't valid. */
          g_warning ("Malformed construction data passed: %s", error->message);
          g_error_free (error);

          g_free (resolver->encoded_triple);
          g_free (resolver->service_name);
          g_free (resolver->service_type);
          g_free (resolver->domain);
          resolver->encoded_triple = NULL;
          resolver->service_name = NULL;
          resolver->service_type = NULL;
          resolver->domain = NULL;
          goto out;
        }
    }

  /* Always set encoded triple to what we encode; this is because we can decode
   * an encoded triple that isn't 100% properly URI encoded, e.g.
   *
   *  "davidz's public files on quad.fubar.dk._webdav._tcp.local"
   *
   * will be properly decoded. But we want to return a properly URI encoded triple
   *
   *  "davidz%27s%20public%20files%20on%20quad%2efubar%2edk._webdav._tcp.local"
   *
   * for e.g. setting the GMountSpec. This is useful because the use can
   * put the former into the pathbar in a file manager and then it will
   * be properly rewritten on mount.
   */
  g_free (resolver->encoded_triple);
  resolver->encoded_triple = g_vfs_encode_dns_sd_triple (resolver->service_name,
                                                         resolver->service_type,
                                                         resolver->domain);

  /* start resolving immediately */
  ensure_avahi_resolver (resolver, NULL);

  resolvers = g_list_prepend (resolvers, resolver);

 out:

  if (G_OBJECT_CLASS (g_vfs_dns_sd_resolver_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (g_vfs_dns_sd_resolver_parent_class)->constructed (object);
}

static void
g_vfs_dns_sd_resolver_class_init (GVfsDnsSdResolverClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  resolver_supports_mdns = (avahi_nss_support () > 0);

  gobject_class->get_property = g_vfs_dns_sd_resolver_get_property;
  gobject_class->set_property = g_vfs_dns_sd_resolver_set_property;
  gobject_class->finalize = g_vfs_dns_sd_resolver_finalize;
  gobject_class->constructed = g_vfs_dns_sd_resolver_constructed;

  /**
   * GVfsDnsSdResolver::changed:
   * @resolver: The resolver emitting the signal.
   *
   * Emitted when resolved data changes.
   */
  signals[CHANGED] = g_signal_new ("changed",
                                   G_VFS_TYPE_DNS_SD_RESOLVER,
                                   G_SIGNAL_RUN_LAST,
                                   G_STRUCT_OFFSET (GVfsDnsSdResolverClass, changed),
                                   NULL,
                                   NULL,
                                   g_cclosure_marshal_VOID__VOID,
                                   G_TYPE_NONE,
                                   0);


  /**
   * GVfsDnsSdResolver:encoded-triple:
   *
   * The encoded DNS-SD triple for the resolver.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_ENCODED_TRIPLE,
                                   g_param_spec_string ("encoded-triple",
                                                        "Encoded triple",
                                                        "Encoded triple",
                                                        NULL,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:required-txt-keys:
   *
   * A comma separated list of keys that must appear in the TXT
   * records in order to consider the service being resolved.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_REQUIRED_TXT_KEYS,
                                   g_param_spec_string ("required-txt-keys",
                                                        "Required TXT keys",
                                                        "Required TXT keys",
                                                        NULL,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:service-name:
   *
   * The name of the service for the resolver.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_SERVICE_NAME,
                                   g_param_spec_string ("service-name",
                                                        "Service Name",
                                                        "Service Name",
                                                        NULL,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:service-type:
   *
   * The type of the service for the resolver.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_SERVICE_TYPE,
                                   g_param_spec_string ("service-type",
                                                        "Service Type",
                                                        "Service Type",
                                                        NULL,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:domain:
   *
   * The domain for the resolver.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DOMAIN,
                                   g_param_spec_string ("domain",
                                                        "Domain",
                                                        "Domain",
                                                        NULL,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:timeout-msec:
   *
   * Timeout in milliseconds to use when resolving.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_TIMEOUT_MSEC,
                                   g_param_spec_uint ("timeout-msec",
                                                      "Timeout in milliseconds",
                                                      "Timeout in milliseconds",
                                                      0,
                                                      G_MAXUINT,
                                                      5000,
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_NAME |
                                                      G_PARAM_STATIC_BLURB |
                                                      G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:is-resolved:
   *
   * Whether the service is resolved.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_IS_RESOLVED,
                                   g_param_spec_boolean ("is-resolved",
                                                         "Is resolved",
                                                         "Is resolved",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_STATIC_NAME |
                                                         G_PARAM_STATIC_BLURB |
                                                         G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:address:
   *
   * The resolved address.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DOMAIN,
                                   g_param_spec_string ("address",
                                                        "Address",
                                                        "Address",
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:port:
   *
   * The resolved port.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_PORT,
                                   g_param_spec_uint ("port",
                                                      "Port",
                                                      "Port",
                                                      0,
                                                      65536,
                                                      0,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_NAME |
                                                      G_PARAM_STATIC_BLURB |
                                                      G_PARAM_STATIC_NICK));

  /**
   * GVfsDnsSdResolver:txt-records:
   *
   * The resolved TXT records.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_TXT_RECORDS,
                                   g_param_spec_boxed ("txt-records",
                                                       "TXT Records",
                                                       "TXT Records",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READABLE |
                                                       G_PARAM_STATIC_NAME |
                                                       G_PARAM_STATIC_BLURB |
                                                       G_PARAM_STATIC_NICK));
}

static void
g_vfs_dns_sd_resolver_init (GVfsDnsSdResolver *resolver)
{
}

gboolean
g_vfs_dns_sd_resolver_is_resolved (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), FALSE);
  return resolver->is_resolved;
}

const gchar *
g_vfs_dns_sd_resolver_get_encoded_triple (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), NULL);
  return resolver->encoded_triple;
}

const gchar *
g_vfs_dns_sd_resolver_get_required_txt_keys (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), NULL);
  return resolver->required_txt_keys;
}

const gchar *
g_vfs_dns_sd_resolver_get_service_name (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), NULL);
  return resolver->service_name;
}

const gchar *
g_vfs_dns_sd_resolver_get_service_type (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), NULL);
  return resolver->service_type;
}

const gchar *
g_vfs_dns_sd_resolver_get_domain (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), NULL);
  return resolver->domain;
}

gchar *
g_vfs_dns_sd_resolver_get_address (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), NULL);
  return g_strdup (resolver->address);
}

guint
g_vfs_dns_sd_resolver_get_port (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), (guint) -1);
  return resolver->port;
}

gchar **
g_vfs_dns_sd_resolver_get_txt_records (GVfsDnsSdResolver *resolver)
{
  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), NULL);
  return g_strdupv (resolver->txt_records);
}

gchar *
g_vfs_dns_sd_resolver_lookup_txt_record (GVfsDnsSdResolver *resolver,
                                         const gchar       *key)
{
  gint n;
  gchar *result;
  gsize key_len;

  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), NULL);
  g_return_val_if_fail (key != NULL, NULL);

  result = NULL;


  if (resolver->txt_records == NULL)
    goto out;

  key_len = strlen (key);

  for (n = 0; resolver->txt_records[n] != NULL; n++)
    {
      const gchar *s = resolver->txt_records[n];
      const gchar *p;

      p = strchr (s, '=');
      if (p != NULL && (p - s) == key_len)
        {
          if (g_ascii_strncasecmp (s,
                                   key,
                                   p - s) == 0)
            {
              result = g_strdup (p + 1);
              goto out;
            }
        }
    }

 out:
  return result;
}

GVfsDnsSdResolver *
g_vfs_dns_sd_resolver_new_for_encoded_triple (const gchar *encoded_triple,
                                              const gchar *required_txt_keys)

{
  g_return_val_if_fail (encoded_triple != NULL, NULL);

  return G_VFS_DNS_SD_RESOLVER (g_object_new (G_VFS_TYPE_DNS_SD_RESOLVER,
                                              "encoded-triple", encoded_triple,
                                              "required-txt-keys", required_txt_keys,
                                              NULL));
}

GVfsDnsSdResolver *
g_vfs_dns_sd_resolver_new_for_service (const gchar *service_name,
                                       const gchar *service_type,
                                       const gchar *domain,
                                       const gchar *required_txt_keys)
{
  g_return_val_if_fail (service_name != NULL, NULL);
  g_return_val_if_fail (service_type != NULL, NULL);
  g_return_val_if_fail (domain != NULL, NULL);

  return G_VFS_DNS_SD_RESOLVER (g_object_new (G_VFS_TYPE_DNS_SD_RESOLVER,
                                              "service-name", service_name,
                                              "service-type", service_type,
                                              "domain", domain,
                                              "required-txt-keys", required_txt_keys,
                                              NULL));
}

static int
safe_strcmp (const char *a, const char *b)
{
  if (a == NULL)
    a = "";
  if (b == NULL)
    b = "";
  return strcmp (a, b);
}

static gboolean
strv_equal (char **a, char **b)
{
  static char *dummy[1] = {NULL};
  int n;
  gboolean ret;

  if (a == NULL)
    a = dummy;
  if (b == NULL)
    b = dummy;

  ret = FALSE;

  if (g_strv_length (a) != g_strv_length (b))
    goto out;

  for (n = 0; a[n] != NULL && b[n] != NULL; n++)
    {
      if (strcmp (a[n], b[n]) != 0)
        goto out;
    }

  ret = TRUE;

 out:
  return ret;

}

static gboolean
has_required_txt_keys (GVfsDnsSdResolver *resolver)
{
  gboolean ret;
  int n;
  char *value;

  ret = FALSE;

  if (resolver->required_txt_keys_broken_out != NULL)
    {
      for (n = 0; resolver->required_txt_keys_broken_out[n] != NULL; n++)
        {
          value = g_vfs_dns_sd_resolver_lookup_txt_record (resolver,
                                                           resolver->required_txt_keys_broken_out[n]);
          if (value == NULL)
            {
              /* key is missing */
              goto out;
            }
          g_free (value);
        }
    }

  ret = TRUE;

 out:
  return ret;
}

static void
set_avahi_data (GVfsDnsSdResolver    *resolver,
                const char           *host_name,
                AvahiProtocol         protocol,
                const AvahiAddress   *a,
                uint16_t              port,
                AvahiStringList      *txt)
{
  char *address;
  gboolean changed;
  AvahiStringList *l;
  GPtrArray *p;
  char **txt_records;
  gboolean is_resolved;

  changed = FALSE;

  if (resolver_supports_mdns)
    {
      address = g_strdup (host_name);
    }
  else
    {
      char aa[128];

      avahi_address_snprint (aa, sizeof(aa), a);
      if (protocol == AVAHI_PROTO_INET6)
        {
          /* an ipv6 address, follow RFC 2732 */
          address = g_strdup_printf ("[%s]", aa);
        }
      else
        {
          address = g_strdup (aa);
        }
    }

  if (safe_strcmp (resolver->address, address) != 0)
    {
      g_free (resolver->address);
      resolver->address = g_strdup (address);
      g_object_notify (G_OBJECT (resolver), "address");
      changed = TRUE;
    }

  g_free (address);

  if (resolver->port != port)
    {
      resolver->port = port;
      g_object_notify (G_OBJECT (resolver), "port");
      changed = TRUE;
    }

  p = g_ptr_array_new ();
  for (l = txt; l != NULL; l = avahi_string_list_get_next (l))
    {
      g_ptr_array_add (p, g_strdup ((const char *) l->text));
    }
  g_ptr_array_add (p, NULL);
  txt_records = (char **) g_ptr_array_free (p, FALSE);

  if (strv_equal (resolver->txt_records, txt_records))
    {
      g_strfreev (txt_records);
    }
  else
    {
      g_strfreev (resolver->txt_records);
      resolver->txt_records = txt_records;
      g_object_notify (G_OBJECT (resolver), "txt-records");
      changed = TRUE;
    }

  is_resolved = has_required_txt_keys (resolver);

  if (is_resolved != resolver->is_resolved)
    {
      resolver->is_resolved = is_resolved;
      g_object_notify (G_OBJECT (resolver), "is-resolved");
      changed = TRUE;
    }

  if (changed)
    g_signal_emit (resolver, signals[CHANGED], 0);
}

static void
clear_avahi_data (GVfsDnsSdResolver *resolver)
{
  gboolean changed;

  changed = FALSE;

  if (resolver->is_resolved)
    {
      resolver->is_resolved = FALSE;
      g_object_notify (G_OBJECT (resolver), "is-resolved");
      changed = TRUE;
    }

  if (resolver->address != NULL)
    {
      g_free (resolver->address);
      resolver->address = NULL;
      g_object_notify (G_OBJECT (resolver), "address");
      changed = TRUE;
    }

  if (resolver->port != 0)
    {
      resolver->port = 0;
      g_object_notify (G_OBJECT (resolver), "port");
      changed = TRUE;
    }

  if (resolver->txt_records != NULL)
    {
      resolver->txt_records = NULL;
      g_object_notify (G_OBJECT (resolver), "txt-records");
      changed = TRUE;
    }

  if (changed)
    g_signal_emit (resolver, signals[CHANGED], 0);
}

static void
service_resolver_cb (AvahiServiceResolver   *avahi_resolver,
                     AvahiIfIndex            interface,
                     AvahiProtocol           protocol,
                     AvahiResolverEvent      event,
                     const char             *name,
                     const char             *type,
                     const char             *domain,
                     const char             *host_name,
                     const AvahiAddress     *a,
                     uint16_t                port,
                     AvahiStringList        *txt,
                     AvahiLookupResultFlags  flags,
                     void                   *user_data)
{
  GVfsDnsSdResolver *resolver = G_VFS_DNS_SD_RESOLVER (user_data);

  switch (event)
    {
    case AVAHI_RESOLVER_FOUND:
      set_avahi_data (resolver,
                      host_name,
                      protocol,
                      a,
                      port,
                      txt);
      break;

    case AVAHI_RESOLVER_FAILURE:
      clear_avahi_data (resolver);
      break;
    }
}


typedef struct
{
  GVfsDnsSdResolver *resolver;
  GSimpleAsyncResult *simple;
  guint timeout_id;
} ResolveData;

static void service_resolver_changed (GVfsDnsSdResolver *resolver, ResolveData *data);

static void
resolve_data_free (ResolveData *data)
{
  if (data->timeout_id > 0)
    g_source_remove (data->timeout_id);
  g_signal_handlers_disconnect_by_func (data->resolver, service_resolver_changed, data);
  g_object_unref (data->simple);
  g_free (data);
}

static void
service_resolver_changed (GVfsDnsSdResolver *resolver,
                          ResolveData       *data)
{
  if (resolver->is_resolved)
    {
      g_simple_async_result_set_op_res_gboolean (data->simple, TRUE);
      g_simple_async_result_complete (data->simple);
      resolve_data_free (data);
    }
  else
    {
      if (data->resolver->address != NULL)
        {
          /* keep going until timeout if we're missing TXT records */
        }
      else
        {
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
          /* Translators:
           * - the first %s refers to the service type
           * - the second %s refers to the service name
           * - the third %s refers to the domain
           */
                                           _("Error resolving \"%s\" service \"%s\" on domain \"%s\""),
                                           data->resolver->service_type,
                                           data->resolver->service_name,
                                           data->resolver->domain);
          g_simple_async_result_complete (data->simple);
          resolve_data_free (data);
        }
    }
}

static gboolean
service_resolver_timed_out (ResolveData *data)
{

  if (data->resolver->address != NULL)
    {
      /* special case if one of the required TXT records are missing */
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
      /* Translators:
       * - the first %s refers to the service type
       * - the second %s refers to the service name
       * - the third %s refers to the domain
       * - the fourth %s refers to the required TXT keys
       */
                                       _("Error resolving \"%s\" service \"%s\" on domain \"%s\". "
                                         "One or more TXT records are missing. Keys required: \"%s\"."),
                                       data->resolver->service_type,
                                       data->resolver->service_name,
                                       data->resolver->domain,
                                       data->resolver->required_txt_keys);
    }
  else
    {
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_TIMED_OUT,
      /* Translators:
       * - the first %s refers to the service type
       * - the second %s refers to the service name
       * - the third %s refers to the domain
       */
                                       _("Timed out resolving \"%s\" service \"%s\" on domain \"%s\""),
                                       data->resolver->service_type,
                                       data->resolver->service_name,
                                       data->resolver->domain);
    }

  g_simple_async_result_complete (data->simple);
  data->timeout_id = 0;
  resolve_data_free (data);
  return FALSE;
}

gboolean
g_vfs_dns_sd_resolver_resolve_finish (GVfsDnsSdResolver  *resolver,
                                      GAsyncResult       *res,
                                      GError            **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);

  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), FALSE);

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == g_vfs_dns_sd_resolver_resolve);
  g_simple_async_result_propagate_error (simple, error);

  return g_simple_async_result_get_op_res_gboolean (simple);
}

void
g_vfs_dns_sd_resolver_resolve (GVfsDnsSdResolver  *resolver,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
  ResolveData *data;
  GSimpleAsyncResult *simple;
  GError *error;

  g_return_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver));

  simple = g_simple_async_result_new (G_OBJECT (resolver),
                                      callback,
                                      user_data,
                                      g_vfs_dns_sd_resolver_resolve);


  if (resolver->is_resolved)
    {
      g_simple_async_result_set_op_res_gboolean (simple, TRUE);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      goto out;
    }

  error = NULL;
  if (!ensure_avahi_resolver (resolver, &error))
    {
      g_simple_async_result_set_from_error (simple, error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      g_error_free (error);
      goto out;
    }

  data = g_new0 (ResolveData, 1);
  data->resolver = resolver;
  data->simple = simple;
  data->timeout_id = g_timeout_add (resolver->timeout_msec,
                                    (GSourceFunc) service_resolver_timed_out,
                                    data);

  g_signal_connect (resolver,
                    "changed",
                    (GCallback) service_resolver_changed,
                    data);

 out:
  ;
}


typedef struct
{
  GMainLoop *loop;
  GError *error;
  gboolean ret;
} ResolveDataSync;

static void
resolve_sync_cb (GVfsDnsSdResolver *resolver,
                 GAsyncResult      *res,
                 ResolveDataSync   *data)
{
  data->ret = g_vfs_dns_sd_resolver_resolve_finish (resolver,
                                                    res,
                                                    &(data->error));
  g_main_loop_quit (data->loop);
}


gboolean
g_vfs_dns_sd_resolver_resolve_sync (GVfsDnsSdResolver  *resolver,
                                    GCancellable       *cancellable,
                                    GError            **error)
{
  ResolveDataSync *data;
  gboolean ret;

  g_return_val_if_fail (G_VFS_IS_DNS_SD_RESOLVER (resolver), FALSE);

  /* TODO: get rid of this nested mainloop, port to avahi mainloop instead  */
  /*       see http://bugzilla.gnome.org/show_bug.cgi?id=555436#c32 */

  data = g_new0 (ResolveDataSync, 1);
  /* mark the main loop as running to have an indication
     whether g_main_loop_quit() was called before g_main_loop_run() */
  data->loop = g_main_loop_new (NULL, TRUE);

  g_vfs_dns_sd_resolver_resolve (resolver,
                                 cancellable,
                                 (GAsyncReadyCallback) resolve_sync_cb,
                                 data);

  /* start main loop only if wasn't quit before
     (i.e. in case when pulling record from cache) */
  if (g_main_loop_is_running (data->loop))
    g_main_loop_run (data->loop);

  ret = data->ret;
  if (data->error != NULL)
    g_propagate_error (error, data->error);

  g_main_loop_unref (data->loop);
  g_free (data);

  return ret;
}
