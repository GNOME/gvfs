 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
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
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>

#ifdef HAVE_GCRYPT
#include <gcrypt.h>
#endif

#include "gvfskeyring.h"
#include "gvfsafpvolume.h"

#include "gvfsafpserver.h"

struct _GvfsAfpServerPrivate
{
  GNetworkAddress     *addr;
  GVfsAfpConnection   *conn;

  GVfsAfpServerInfo   info;
  gint32              time_diff;

  guint32             user_id;
  guint32             group_id;
  guint64             uuid;
};

G_DEFINE_TYPE_WITH_PRIVATE (GVfsAfpServer, g_vfs_afp_server, G_TYPE_OBJECT);

#define AFP_UAM_NO_USER   "No User Authent"
#define AFP_UAM_DHX       "DHCAST128"
#define AFP_UAM_DHX2      "DHX2"

GVfsAfpServer *
g_vfs_afp_server_new (GNetworkAddress *addr)
{
  GVfsAfpServer *server;

  server = g_object_new (G_VFS_TYPE_AFP_SERVER, NULL);

  server->priv->addr = addr;
  
  return server;
}

static void
g_vfs_afp_server_init (GVfsAfpServer *server)
{
  GVfsAfpServerPrivate *priv;

  server->priv = priv = g_vfs_afp_server_get_instance_private (server);

  priv->info.machine_type = NULL;
  priv->info.server_name = NULL;
  priv->info.utf8_server_name = NULL;
  priv->info.uams = NULL;
  priv->info.version = AFP_VERSION_INVALID;
}

static void
g_vfs_afp_server_finalize (GObject *object)
{
  GVfsAfpServer *server = G_VFS_AFP_SERVER (object);
  GVfsAfpServerPrivate *priv = server->priv;

  g_clear_object (&priv->addr);
  g_clear_object (&priv->conn);
  
  g_free (priv->info.machine_type);
  g_free (priv->info.server_name);
  g_free (priv->info.utf8_server_name);
  
  g_slist_free_full (priv->info.uams, g_free);

  G_OBJECT_CLASS (g_vfs_afp_server_parent_class)->finalize (object);
}

static void
g_vfs_afp_server_class_init (GVfsAfpServerClass *klass)
{
  GObjectClass* object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = g_vfs_afp_server_finalize;
}

static const char *
afp_version_to_string (AfpVersion afp_version)
{
  const char *version_strings[] = { "AFPX03", "AFP3.1", "AFP3.2", "AFP3.3" };

  return version_strings[afp_version - 1];
}

static AfpVersion
string_to_afp_version (const char *str)
{
  gint i;

  const char *version_strings[] = { "AFPX03", "AFP3.1", "AFP3.2", "AFP3.3" };

  for (i = 0; i < G_N_ELEMENTS (version_strings); i++)
  {
	if (g_str_equal (str, version_strings[i]))
	  return i + 1;
  }

  return AFP_VERSION_INVALID;
}

#ifdef HAVE_GCRYPT
static gboolean
dhx2_login (GVfsAfpServer *server,
            const char *username,
            const char *password,
            GCancellable *cancellable,
            GError **error)
{
  GVfsAfpServerPrivate *priv = server->priv;
  
  gboolean res;
  gcry_error_t gcry_err;
  GVfsAfpCommand *comm;
  GVfsAfpReply   *reply;
  AfpResultCode res_code;

  guint8 C2SIV[] = { 0x4c, 0x57, 0x61, 0x6c, 0x6c, 0x61, 0x63, 0x65  };
  guint8 S2CIV[] = { 0x43, 0x4a, 0x61, 0x6c, 0x62, 0x65, 0x72, 0x74  };

  /* reply 1 */
  guint16 id;
  guint16 len;
  guint32 bits;
  guint8 *tmp_buf, *buf;

  gcry_mpi_t g, p, Ma, Mb, Ra, key;
  gcry_cipher_hd_t cipher;
  
  gcry_mpi_t clientNonce;
  guint8 clientNonce_buf[16];
  guint8 key_md5_buf[16];

  /* reply 2 */
  guint8 reply2_buf[32];
  gcry_mpi_t clientNonce1, serverNonce;

  /* request 3 */
  guint8 answer_buf[272] = {0};
  size_t nonce_len;
  
  /* initialize for easy cleanup */
  g = NULL,
  p = NULL;
  Ma = NULL;
  Mb = NULL;
  Ra = NULL;
  key = NULL;
  clientNonce = NULL;
  clientNonce1 = NULL;
  serverNonce = NULL;
  buf = NULL;
  
  /* setup cipher */
  gcry_err = gcry_cipher_open (&cipher, GCRY_CIPHER_CAST5, GCRY_CIPHER_MODE_CBC,
                               0);
  g_assert (gcry_err == 0);

  
  if (strlen (password) > 256)
  {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                 /* Translators: %d is a constant, currently hardcoded to 256 */
                 ngettext ("The server doesn’t support passwords longer than %d character.",
                           "The server doesn’t support passwords longer than %d characters.",
                           256), 256);
    goto error;
  }

  /* Request 1 */
  comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN);
  g_vfs_afp_command_put_pascal (comm, afp_version_to_string (priv->info.version));
  g_vfs_afp_command_put_pascal (comm, AFP_UAM_DHX2);
  g_vfs_afp_command_put_pascal (comm, username);
  g_vfs_afp_command_pad_to_even (comm);

  reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm,
                                                  cancellable, error);
  g_object_unref (comm);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_AUTH_CONTINUE)
  {
    g_object_unref (reply);
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("An invalid username was provided."));
      goto error;
    }
    else
      goto generic_error;
  }

  /* Get data from reply */
  REPLY_READ_UINT16 (reply, &id);

  /* read g */
  REPLY_GET_DATA (reply, 4, &tmp_buf);
  gcry_err = gcry_mpi_scan (&g, GCRYMPI_FMT_USG, tmp_buf, 4, NULL);
  g_assert (gcry_err == 0);

  REPLY_READ_UINT16 (reply, &len);
  bits = len * 8;

  /* read p */
  REPLY_GET_DATA (reply, len, &tmp_buf);
  gcry_err = gcry_mpi_scan (&p, GCRYMPI_FMT_USG, tmp_buf, len, NULL);
  g_assert (gcry_err == 0);

  /* read Mb */
  REPLY_GET_DATA (reply, len, &tmp_buf);
  gcry_err = gcry_mpi_scan (&Mb, GCRYMPI_FMT_USG, tmp_buf, len, NULL);
  g_assert (gcry_err == 0);

  g_object_unref (reply);
  
  /* generate random number Ra != 0 */
  Ra = gcry_mpi_new (bits);
  while (gcry_mpi_cmp_ui (Ra, 0) == 0)
    gcry_mpi_randomize (Ra, bits, GCRY_STRONG_RANDOM);

  /* Secret key value must be less than half of prime */
  if (gcry_mpi_get_nbits (Ra) > bits - 1)
    gcry_mpi_clear_highbit (Ra, bits - 1);

  /* generate Ma */
  Ma = gcry_mpi_new (bits);
  gcry_mpi_powm (Ma, g, Ra, p);

  /* derive Key */
  key = gcry_mpi_new (bits);
  gcry_mpi_powm (key, Mb, Ra, p);

  buf = g_malloc0 (len);
  gcry_err = gcry_mpi_print (GCRYMPI_FMT_USG, buf, len, NULL,
                             key);
  g_assert (gcry_err == 0);
  gcry_md_hash_buffer (GCRY_MD_MD5, key_md5_buf, buf, len);

  /* generate random clientNonce != 0 */
  clientNonce = gcry_mpi_new (128);
  while (gcry_mpi_cmp_ui (clientNonce, 0) == 0)
    gcry_mpi_randomize (clientNonce, 128, GCRY_STRONG_RANDOM);

  gcry_err = gcry_mpi_print (GCRYMPI_FMT_USG, clientNonce_buf, 16, &nonce_len,
                             clientNonce);
  g_assert (gcry_err == 0);
  if (nonce_len < 16)
  {
    memmove(clientNonce_buf + 16 - nonce_len, clientNonce_buf, nonce_len);
    memset(clientNonce_buf, 0, 16 - nonce_len);
  }

  gcry_cipher_setiv (cipher, C2SIV, G_N_ELEMENTS (C2SIV));
  gcry_cipher_setkey (cipher, key_md5_buf, 16);

  gcry_err = gcry_cipher_encrypt (cipher, clientNonce_buf, 16,
                                  NULL, 0);
  g_assert (gcry_err == 0);


  /* Create Request 2 */
  comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN_CONT);
  
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
  /* Id */
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), id, NULL, NULL);
  /* Ma */
  memset (buf, 0, len);
  gcry_err = gcry_mpi_print (GCRYMPI_FMT_USG, buf, len, NULL,
                             Ma);
  g_assert (gcry_err == 0);
  g_output_stream_write_all (G_OUTPUT_STREAM (comm), buf, len, NULL, NULL, NULL);
  /* clientNonce */
  g_output_stream_write_all (G_OUTPUT_STREAM (comm), clientNonce_buf, 16, NULL, NULL, NULL);

  reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm,
                                                cancellable, error);
  g_object_unref (comm);
  if (!reply)
    goto error;
  
  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_AUTH_CONTINUE)
  {
    g_object_unref (reply);
    goto generic_error;
  }

  /* read data from reply 2 */
  REPLY_READ_UINT16 (reply, &id);

  REPLY_GET_DATA (reply, 32, &tmp_buf);
  memcpy (reply2_buf, tmp_buf, 32);

  g_object_unref (reply);

  /* decrypt */
  gcry_cipher_setiv (cipher, S2CIV, G_N_ELEMENTS (S2CIV));
  gcry_err = gcry_cipher_decrypt (cipher, reply2_buf, 32, NULL, 0);
  g_assert (gcry_err == 0);

  /* check clientNonce + 1 */
  gcry_err = gcry_mpi_scan (&clientNonce1, GCRYMPI_FMT_USG, reply2_buf, 16, NULL);
  g_assert (gcry_err == 0);
  gcry_mpi_add_ui (clientNonce, clientNonce, 1);
  if (gcry_mpi_cmp (clientNonce, clientNonce1) != 0)
    goto generic_error;

  gcry_err = gcry_mpi_scan (&serverNonce, GCRYMPI_FMT_USG, reply2_buf + 16, 16, NULL);
  g_assert (gcry_err == 0);
  gcry_mpi_add_ui (serverNonce, serverNonce, 1);

  /* create encrypted answer */
  gcry_err = gcry_mpi_print (GCRYMPI_FMT_USG, answer_buf, 16, &nonce_len, serverNonce);
  g_assert (gcry_err == 0);

  if (nonce_len < 16)
  {
    memmove(answer_buf + 16 - nonce_len, answer_buf, nonce_len);
    memset(answer_buf, 0, 16 - nonce_len);
  }

  memcpy (answer_buf + 16, password, strlen (password));

  gcry_cipher_setiv (cipher, C2SIV, G_N_ELEMENTS (C2SIV));
  gcry_err = gcry_cipher_encrypt (cipher, answer_buf, G_N_ELEMENTS (answer_buf),
                                  NULL, 0);
  g_assert (gcry_err == 0);
  
  /* Create request 3 */
  comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN_CONT);
  
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
  /* id */
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), id, NULL, NULL);
  g_output_stream_write_all (G_OUTPUT_STREAM (comm), answer_buf,
                             G_N_ELEMENTS (answer_buf), NULL, NULL, NULL);


  reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm,
                                                  cancellable, error);
  g_object_unref (comm);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("Unable to login to the server “%s” with the given password."),
                   priv->info.server_name);
      goto error;
    }
    else
      goto generic_error;
  }

  res = TRUE;
  
cleanup:
  gcry_mpi_release (g);
  gcry_mpi_release (p);
  gcry_mpi_release (Ma);
  gcry_mpi_release (Mb);
  gcry_mpi_release (Ra);
  gcry_mpi_release (key);
  gcry_mpi_release (clientNonce);
  gcry_mpi_release (clientNonce1);
  gcry_mpi_release (serverNonce);
  gcry_cipher_close (cipher);
  g_free (buf);

  return res;

error:
  res = FALSE;
  goto cleanup;
  
generic_error:
  g_propagate_error (error, afp_result_code_to_gerror (res_code));
  goto error;

invalid_reply:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Unable to connect to the server “%s”. A communication problem occurred."),
               priv->info.server_name);
  goto error;
}

static gboolean
dhx_login (GVfsAfpServer *server,
           const char *username,
           const char *password,
           GCancellable *cancellable,
           GError **error)
{
  GVfsAfpServerPrivate *priv = server->priv;
  
  gcry_error_t gcry_err;
  gcry_mpi_t prime, base;
  gcry_mpi_t ra;

  /* Ma */
  gcry_mpi_t ma;
  guint8 ma_buf[16];

  GVfsAfpCommand *comm;
  GVfsAfpReply *reply;
  AfpResultCode res_code;
  gboolean res;
  guint16 id;
  guint8 *tmp_buf;

  /* Mb */
  gcry_mpi_t mb;

  /* Nonce */
  guint8 nonce_buf[32];
  gcry_mpi_t nonce;

  /* Key */
  gcry_mpi_t key;
  guint8 key_buf[16];

  gcry_cipher_hd_t cipher;
  guint8 answer_buf[80] = { 0 };
  size_t len;

  static const guint8 C2SIV[] = { 0x4c, 0x57, 0x61, 0x6c, 0x6c, 0x61, 0x63, 0x65  };
  static const guint8 S2CIV[] = { 0x43, 0x4a, 0x61, 0x6c, 0x62, 0x65, 0x72, 0x74  };
  static const guint8 p[] = { 0xBA, 0x28, 0x73, 0xDF, 0xB0, 0x60, 0x57, 0xD4, 0x3F, 0x20, 0x24, 0x74,  0x4C, 0xEE, 0xE7, 0x5B };
  static const guint8 g[] = { 0x07 };

  if (strlen (password) > 64)
  {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                 ngettext ("The server doesn’t support passwords longer than %d character.",
                           "The server doesn’t support passwords longer than %d characters.",
                           64), 64);
    return FALSE;
  }
  
  /* create prime and base from vectors */
  gcry_err = gcry_mpi_scan (&prime, GCRYMPI_FMT_USG, p, G_N_ELEMENTS (p), NULL);
  g_assert (gcry_err == 0);

  gcry_err = gcry_mpi_scan (&base, GCRYMPI_FMT_USG, g, G_N_ELEMENTS (g), NULL);
  g_assert (gcry_err == 0);

  /* generate random number ra != 0 */
  ra = gcry_mpi_new (256);
  while (gcry_mpi_cmp_ui (ra, 0) == 0)
    gcry_mpi_randomize (ra, 256, GCRY_STRONG_RANDOM);

  /* Secret key value must be less than half of prime */
  if (gcry_mpi_get_nbits (ra) > 255)
		gcry_mpi_clear_highbit (ra, 255);
  
  /* generate ma */
  ma = gcry_mpi_new (128);
  gcry_mpi_powm (ma, base, ra, prime);
  gcry_mpi_release (base);
  gcry_err = gcry_mpi_print (GCRYMPI_FMT_USG, ma_buf, G_N_ELEMENTS (ma_buf), NULL,
                             ma);
  g_assert (gcry_err == 0);
  gcry_mpi_release (ma);

  /* Create login command */
  comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN);
  g_vfs_afp_command_put_pascal (comm, afp_version_to_string (priv->info.version));
  g_vfs_afp_command_put_pascal (comm, AFP_UAM_DHX);
  g_vfs_afp_command_put_pascal (comm, username);
  g_vfs_afp_command_pad_to_even (comm);
  g_output_stream_write_all (G_OUTPUT_STREAM(comm), ma_buf, G_N_ELEMENTS (ma_buf),
                             NULL, NULL, NULL);

  reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm,
                                                  cancellable, error);
  g_object_unref (comm);
  if (!reply)
    goto error;  

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_AUTH_CONTINUE)
  {
    g_object_unref (reply);
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("An invalid username was provided."));
      goto error;
    }
    else
      goto generic_error;
  }

  REPLY_READ_UINT16 (reply, &id);

  /* read Mb */
  REPLY_GET_DATA (reply, 16, &tmp_buf);
  gcry_err = gcry_mpi_scan (&mb, GCRYMPI_FMT_USG, tmp_buf, 16, NULL);
  g_assert (gcry_err == 0);

  /* read Nonce */
  REPLY_GET_DATA (reply, 32, &tmp_buf);
  memcpy (nonce_buf, tmp_buf, 32);

  g_object_unref (reply);

  /* derive key */
  key = gcry_mpi_new (128);
  gcry_mpi_powm (key, mb, ra, prime);
  gcry_mpi_release (mb);
  gcry_err = gcry_mpi_print (GCRYMPI_FMT_USG, key_buf, G_N_ELEMENTS (key_buf), NULL,
                             key);
  g_assert (gcry_err == 0);
  gcry_mpi_release (key);

  /* setup decrypt cipher */
  gcry_err = gcry_cipher_open (&cipher, GCRY_CIPHER_CAST5, GCRY_CIPHER_MODE_CBC,
                               0);
  g_assert (gcry_err == 0);

  gcry_cipher_setiv (cipher, S2CIV, G_N_ELEMENTS (S2CIV));
  gcry_cipher_setkey (cipher, key_buf, G_N_ELEMENTS (key_buf));

  /* decrypt Nonce */
  gcry_err = gcry_cipher_decrypt (cipher,  nonce_buf, G_N_ELEMENTS (nonce_buf),
                                  NULL, 0);
  g_assert (gcry_err == 0);

  gcry_err = gcry_mpi_scan (&nonce, GCRYMPI_FMT_USG, nonce_buf, 16, NULL);
  g_assert (gcry_err == 0);

  /* add one to nonce */
  gcry_mpi_add_ui (nonce, nonce, 1);

  /* set client->server initialization vector */
  gcry_cipher_setiv (cipher, C2SIV, G_N_ELEMENTS (C2SIV));
  
  /* create encrypted answer */
  gcry_err = gcry_mpi_print (GCRYMPI_FMT_USG, answer_buf, 16, &len, nonce);
  g_assert (gcry_err == 0);
  gcry_mpi_release (nonce);

  if (len < 16)
  {
    memmove(answer_buf + 16 - len, answer_buf, len);
    memset(answer_buf, 0, 16 - len);
  }
  
  memcpy (answer_buf + 16, password, strlen (password));

  gcry_err = gcry_cipher_encrypt (cipher, answer_buf, G_N_ELEMENTS (answer_buf),
                                  NULL, 0);
  g_assert (gcry_err == 0);
  gcry_cipher_close (cipher);

  /* Create Login Continue command */
  comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN_CONT);
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), id, NULL, NULL);
  g_output_stream_write_all (G_OUTPUT_STREAM (comm), answer_buf,
                             G_N_ELEMENTS (answer_buf), NULL, NULL, NULL);


  reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm,
                                                  cancellable, error);
  g_object_unref (comm);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("Unable to connect to the server “%s” with the given password."),
                   priv->info.server_name);
      goto error;
    }
    else
      goto generic_error;
  }
  
  res = TRUE;

done:
  gcry_mpi_release (prime);
  gcry_mpi_release (ra);

  return res;

error:
  res = FALSE;
  goto done;
  
generic_error:
  g_propagate_error (error, afp_result_code_to_gerror (res_code));
  res = FALSE;
  goto done;

invalid_reply:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Unable to connect to the server “%s”. A communication problem occurred."),
               priv->info.server_name);
  goto error;
}
#endif

static gboolean
do_login (GVfsAfpServer *server,
          const char *username,
          const char *password,
          gboolean anonymous,
          GCancellable *cancellable,
          GError **error)
{
  GVfsAfpServerPrivate *priv = server->priv;
  
  /* anonymous login */
  if (anonymous)
  {
    GVfsAfpCommand *comm;
    GVfsAfpReply *reply;
    AfpResultCode res_code;
    
    if (!g_slist_find_custom (priv->info.uams, AFP_UAM_NO_USER, (GCompareFunc)g_strcmp0))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("The server “%s” doesn’t support anonymous access."),
                   priv->info.server_name);
      return FALSE;
    }

    comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN);

    g_vfs_afp_command_put_pascal (comm, afp_version_to_string (priv->info.version));
    g_vfs_afp_command_put_pascal (comm, AFP_UAM_NO_USER);
    reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm,
                                                    cancellable, error);
    g_object_unref (comm);
    if (!reply)
      return FALSE;

    res_code = g_vfs_afp_reply_get_result_code (reply);
    g_object_unref (reply);
    
    if (res_code != AFP_RESULT_NO_ERROR)
    {
      switch (res_code)
      {
        case AFP_RESULT_USER_NOT_AUTH:
        case AFP_RESULT_BAD_UAM:
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       _("The server “%s” doesn’t support anonymous access."),
                       priv->info.server_name);
          break;
          
        default:
          g_propagate_error (error, afp_result_code_to_gerror (res_code));
          break;
      }
      
      return FALSE;
    }

    return TRUE;
  }

  else {

#ifdef HAVE_GCRYPT
    /* Diffie-Hellman 2 */
    if (g_slist_find_custom (priv->info.uams, AFP_UAM_DHX2, (GCompareFunc)g_strcmp0))
      return dhx2_login (server, username, password, cancellable, error);
    
    /* Diffie-Hellman */
    if (g_slist_find_custom (priv->info.uams, AFP_UAM_DHX, (GCompareFunc)g_strcmp0))
      return dhx_login (server, username, password, cancellable, error); 
#endif

    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 _("Unable to connect to the server “%s”. No suitable authentication mechanism was found."),
                 priv->info.server_name);
    return FALSE;
  }
}

static gboolean
get_server_info (GVfsAfpServer *server,
                 GCancellable *cancellable,
                 GError **error)
{
  GVfsAfpServerPrivate *priv = server->priv;
  
  GVfsAfpReply *reply;

  guint16 MachineType_offset, AFPVersionCount_offset, UAMCount_offset;
  
  guint8 count;
  guint i;

  reply = g_vfs_afp_query_server_info (G_SOCKET_CONNECTABLE (priv->addr),
                                       cancellable, error);
  if (!reply)
    return FALSE;

  REPLY_READ_UINT16 (reply, &MachineType_offset);
  REPLY_READ_UINT16 (reply, &AFPVersionCount_offset);
  REPLY_READ_UINT16 (reply, &UAMCount_offset);

  /* VolumeIconAndMask_offset */
  REPLY_READ_UINT16 (reply, NULL);

  REPLY_READ_UINT16 (reply, &priv->info.flags);

  REPLY_READ_PASCAL (reply, FALSE, &priv->info.server_name);

  /* Parse UTF-8 ServerName */
  if (priv->info.flags & (0x1 << 8)) {
    guint16 UTF8ServerName_offset;
    GVfsAfpName *utf8_server_name;

    REPLY_SKIP_TO_EVEN (reply);
    REPLY_SEEK (reply, 6, G_SEEK_CUR);
    REPLY_READ_UINT16 (reply, &UTF8ServerName_offset);

    REPLY_SEEK (reply, UTF8ServerName_offset, G_SEEK_SET);
    REPLY_READ_AFP_NAME (reply, FALSE, &utf8_server_name);
    priv->info.utf8_server_name = g_vfs_afp_name_get_string (utf8_server_name);
    g_vfs_afp_name_unref (utf8_server_name);
  }

  /* Parse MachineType */
  REPLY_SEEK (reply, MachineType_offset, G_SEEK_SET);
  REPLY_READ_PASCAL (reply, FALSE, &priv->info.machine_type);

  /* Parse Versions */
  REPLY_SEEK (reply, AFPVersionCount_offset, G_SEEK_SET);
  REPLY_READ_BYTE (reply, &count);
  for (i = 0; i < count; i++)
  {
    char *version;
    AfpVersion afp_version;

    REPLY_READ_PASCAL (reply, FALSE, &version);
    afp_version = string_to_afp_version (version);
    g_free (version);
    if (afp_version > priv->info.version)
      priv->info.version = afp_version;
  }

  if (priv->info.version == AFP_VERSION_INVALID)
  {
    g_object_unref (reply);
    g_set_error (error,
                 G_IO_ERROR, G_IO_ERROR_FAILED,
                 _("Unable to connect to the server “%s”. The server doesn’t support AFP version 3.0 or later."),
                 priv->info.server_name);
    return FALSE;
  }

  /* Parse UAMs */
  REPLY_SEEK (reply, UAMCount_offset, G_SEEK_SET);
  REPLY_READ_BYTE (reply, &count);
  for (i = 0; i < count; i++)
  {
    char *uam;

    REPLY_READ_PASCAL (reply, FALSE, &uam);
    priv->info.uams = g_slist_prepend (priv->info.uams, uam);
  }

  g_object_unref (reply);

  return TRUE;

invalid_reply:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Unable to connect to the server “%s”. A communication problem occurred."),
               priv->info.server_name);

  return FALSE;
}

static gboolean
get_server_parms (GVfsAfpServer *server,
                  GCancellable  *cancellable,
                  GError       **error)
{
  GVfsAfpServerPrivate *priv = server->priv;
  
  GVfsAfpCommand *comm;
  GVfsAfpReply   *reply;
  AfpResultCode   res_code;
  gint32          server_time;
  
  /* Get Server Parameters */
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_SRVR_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm, cancellable,
                                                  error);
  g_object_unref (comm);
  if (!reply)
    return FALSE;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    g_propagate_error (error, afp_result_code_to_gerror (res_code));
    return FALSE;
  }

  /* server time */
  REPLY_READ_INT32 (reply, &server_time);
  priv->time_diff = (g_get_real_time () / G_USEC_PER_SEC) - server_time;

  g_object_unref (reply);

  return TRUE;

invalid_reply:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Unable to connect to the server “%s”. A communication problem occurred."),
               priv->info.server_name);

  return FALSE;
}

static GVfsAfpReply *
command_get_user_info (GVfsAfpServer *server,
                       guint16        bitmap,
                       GCancellable  *cancellable,
                       GError       **error)
{
  GVfsAfpServerPrivate *priv = server->priv;
  
  GVfsAfpCommand *comm;
  GVfsAfpReply *reply;
  AfpResultCode res_code;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_USER_INFO);
  /* Flags, ThisUser = 1 */
  g_vfs_afp_command_put_byte (comm, 0x01);
  /* UserId */
  g_vfs_afp_command_put_int32 (comm, 0);
  /* Bitmap */
  g_vfs_afp_command_put_uint16 (comm, bitmap);

  reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm, cancellable,
                                                  error);
  g_object_unref (comm);
  if (!reply)
    return NULL;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("Permission denied."));
        break;
        break;
      case AFP_RESULT_CALL_NOT_SUPPORTED:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     _("The command is not supported by the server."));
        break;
      case AFP_RESULT_PWD_EXPIRED_ERR:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("Your password has expired."));
        break;
      case AFP_RESULT_PWD_NEEDS_CHANGE_ERR:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("Your password needs to be changed."));
        break;

      default:
        g_propagate_error (error, afp_result_code_to_gerror (res_code));
        break;
    }
    return NULL;
  }

  return reply;
}

static gboolean
get_userinfo (GVfsAfpServer *server,
              GCancellable  *cancellable,
              GError       **error)
{
  GVfsAfpServerPrivate *priv = server->priv;

  gboolean res = FALSE;
  GVfsAfpReply *reply = NULL;
  guint16 bitmap;

  bitmap = AFP_GET_USER_INFO_BITMAP_GET_UID_BIT | AFP_GET_USER_INFO_BITMAP_GET_UUID_BIT;
  reply = command_get_user_info (server, bitmap, cancellable, error);
  if (!reply)
    goto done;
  
  /* Bitmap */
  REPLY_READ_UINT16 (reply, &bitmap);
  if (bitmap != (AFP_GET_USER_INFO_BITMAP_GET_UID_BIT | AFP_GET_USER_INFO_BITMAP_GET_UUID_BIT))
    goto invalid_reply;

  REPLY_READ_UINT32 (reply, &priv->user_id);
  REPLY_READ_UINT64 (reply, &priv->uuid);

  g_clear_object (&reply);

  /* We try to get the group id separately since seems to give an invalid reply
   * on some OS X versions. */
  bitmap = AFP_GET_USER_INFO_BITMAP_GET_GID_BIT;
  reply = command_get_user_info (server, bitmap, cancellable, error);
  if (!reply)
    goto done;

  /* Bitmap */
  REPLY_READ_UINT16 (reply, &bitmap);
  if (bitmap != AFP_GET_USER_INFO_BITMAP_GET_GID_BIT)
    goto invalid_reply;

  /* Don't check for errors since it's known to fail on some servers. */
  g_vfs_afp_reply_read_uint32 (reply, &priv->group_id);

  res = TRUE;

done:
  g_clear_object (&reply);
  return res;

invalid_reply:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Unable to connect to the server “%s”. A communication problem occurred."),
               priv->info.server_name);
  goto done;
}

gboolean
g_vfs_afp_server_login (GVfsAfpServer *server,
                        const char     *initial_user,
                        GMountSource   *mount_source,
                        char           **logged_in_user,
                        GCancellable   *cancellable,
                        GError         **error)
{
  GVfsAfpServerPrivate *priv = server->priv;
  
  gboolean res;
  char *user, *olduser;
  char *password;
  char *server_name;
  gboolean anonymous;
  GPasswordSave password_save;
  char *prompt = NULL;
  GError *err = NULL;

  res = get_server_info (server, cancellable, error);
  if (!res)
    return FALSE;

  /* Use utf8_server_name if it exists */
  server_name = priv->info.utf8_server_name ? priv->info.utf8_server_name : priv->info.server_name;

  olduser = g_strdup (initial_user);

  if (initial_user)
  {
    if (g_str_equal (initial_user, "anonymous") &&
        g_slist_find_custom (priv->info.uams, AFP_UAM_NO_USER, (GCompareFunc)g_strcmp0))
    {
      user = NULL;
      password = NULL;
      anonymous = TRUE;
      goto try_login;
    }
  }

  if (g_vfs_keyring_lookup_password (initial_user,
                                     g_network_address_get_hostname (priv->addr),
                                     NULL,
                                     "afp",
                                     NULL,
                                     NULL,
                                     g_network_address_get_port (priv->addr),
                                     &user,
                                     NULL,
                                     &password) &&
      user != NULL &&
      password != NULL)
  {
    anonymous = FALSE;
    goto try_login;
  }

  while (TRUE)
  {
    GString *str;
    GAskPasswordFlags flags;
    gboolean aborted;

    g_free (prompt);
    g_clear_error (&err);

    str = g_string_new (NULL);

    /* create prompt */
    if (initial_user)
      /* Translators: the first %s is the username, the second the host name */
      g_string_append_printf (str, _("Authentication Required\nEnter password for “%s” on “%s”:"), initial_user, server_name);
    else
      /* Translators: %s here is the hostname */
      g_string_append_printf (str, _("Authentication Required\nEnter user and password for “%s”:"), server_name);

    prompt = g_string_free (str, FALSE);

    flags = G_ASK_PASSWORD_NEED_PASSWORD;

    if (!initial_user)
    {
      flags |= G_ASK_PASSWORD_NEED_USERNAME;
      
      if (g_slist_find_custom (priv->info.uams, AFP_UAM_NO_USER, (GCompareFunc)g_strcmp0))
        flags |= G_ASK_PASSWORD_ANONYMOUS_SUPPORTED;
    }

    if (g_vfs_keyring_is_available ())
      flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;

    if (!g_mount_source_ask_password (mount_source,
                                      prompt,
                                      olduser,
                                      NULL,
                                      flags,
                                      &aborted,
                                      &password,
                                      &user,
                                      NULL,
                                      &anonymous,
                                      &password_save) ||
        aborted)
    {
      g_set_error_literal (&err, G_IO_ERROR,
                           aborted ? G_IO_ERROR_FAILED_HANDLED : G_IO_ERROR_PERMISSION_DENIED,
                           _("The password prompt was cancelled."));
      res = FALSE;
      break;
    }
    
    if (!user)
      user = g_strdup (olduser);

try_login:

    /* Open connection */
    priv->conn = g_vfs_afp_connection_new (G_SOCKET_CONNECTABLE (priv->addr));
    res = g_vfs_afp_connection_open_sync (priv->conn, cancellable, &err);
    if (!res)
      break;

    res = do_login (server, user, password, anonymous,
                    cancellable, &err);
    if (!res)
    {
      g_vfs_afp_connection_close_sync (priv->conn, cancellable, NULL);
      g_clear_object (&priv->conn);

      if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        break;
    }
    else
      break;

    g_free (olduser);
    olduser = user;

    g_free (password);
  }

  g_free (olduser);

  if (!res)
    goto error;

    /* Get server parms */
  if (!get_server_parms (server, cancellable, &err))
    goto error;

  /* Get user info */
  if (!get_userinfo (server, cancellable, &err))
    goto error;
  
  if (prompt && !anonymous)
  {
    /* a prompt was created, so we have to save the password */
    g_vfs_keyring_save_password (user,
                                 g_network_address_get_hostname (priv->addr),
                                 NULL,
                                 "afp",
                                 NULL,
                                 NULL,
                                 g_network_address_get_port (priv->addr),
                                 password,
                                 password_save);
    g_free (prompt);
  }
  
  if (logged_in_user)
  {
    if (anonymous)
      *logged_in_user = g_strdup ("anonymous");
    else
      *logged_in_user = user;
  }
  else
    g_free (user);
  
  g_free (password);

  return TRUE;

error:
  g_free (user);
  g_free (password);
  g_propagate_error (error, err);
  return FALSE;
}

/*
 * g_vfs_afp_server_logout_sync:
 * 
 * Terminates and closes the connection to the server
 */
gboolean
g_vfs_afp_server_logout_sync (GVfsAfpServer *server,
                              GCancellable  *cancellable,
                              GError       **error)
{
  GVfsAfpServerPrivate *priv;
  GVfsAfpCommand *comm;
  GVfsAfpReply *reply;
  gint32 res_code;
  gboolean res = FALSE;

  g_return_val_if_fail (G_VFS_IS_AFP_SERVER (server), FALSE);

  priv = server->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_LOGOUT);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  reply = g_vfs_afp_connection_send_command_sync (priv->conn, comm, cancellable, error);
  g_object_unref (comm);
  if (!reply) {
    g_vfs_afp_connection_close_sync (priv->conn, cancellable, NULL);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Unable to disconnect from the server."));
    g_vfs_afp_connection_close_sync (priv->conn, cancellable, NULL);
    goto done;
  }

  res = g_vfs_afp_connection_close_sync (priv->conn, cancellable, error);

done:
  g_clear_object (&priv->conn);
  return res; 
}

/**
 * g_vfs_afp_server_get_info:
 * 
 * @server: a #GVfsAfpServer
 * 
 * Returns: a #GVfsAfpServerInfo or %NULL if the server is not logged in.
 */
const GVfsAfpServerInfo *
g_vfs_afp_server_get_info (GVfsAfpServer *server)
{
  GVfsAfpServerPrivate *priv;
  
  g_return_val_if_fail (G_VFS_IS_AFP_SERVER (server), FALSE);

  priv = server->priv;

  if (!priv->conn)
    return NULL;

  return &priv->info;
}

/**
 * g_vfs_afp_server_get_max_request_size:
 * @server: a #GVfsAfpServer
 * 
 * Get the maximum request size the server supports.
 * 
 * Returns: The maximum request size the server supports.
 */
guint32
g_vfs_afp_server_get_max_request_size (GVfsAfpServer *server)
{
  g_return_val_if_fail (G_VFS_IS_AFP_SERVER (server), 0);
  
  return g_vfs_afp_connection_get_max_request_size (server->priv->conn);
}

/**
 * g_vfs_afp_server_time_to_local_time:
 * 
 * @server: a #GVfsAfpServer
 * @server_time: a time value in server time
 * 
 * Returns: the time converted to local time
 */
gint64
g_vfs_afp_server_time_to_local_time (GVfsAfpServer *server,
                                     gint32         server_time)
{  
  g_return_val_if_fail (G_VFS_IS_AFP_SERVER (server), 0);

  return server_time + server->priv->time_diff;
}


static void
volume_data_free (GVfsAfpVolumeData *vol_data)
{
  g_free (vol_data->name);
  g_slice_free (GVfsAfpVolumeData, vol_data);
}


static void
get_volumes_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GTask *task = G_TASK (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  
  guint8 num_volumes, i;
  GPtrArray *volumes;
  
  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_task_return_error (task, err);
    g_object_unref (task);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    g_task_return_error (task, afp_result_code_to_gerror (res_code));
    g_object_unref (task);
    return;
  }

  /* server time */
  REPLY_READ_INT32 (reply, NULL);

  /* NumVolStructures */
  REPLY_READ_BYTE (reply, &num_volumes);

  volumes = g_ptr_array_sized_new (num_volumes);
  g_ptr_array_set_free_func (volumes, (GDestroyNotify)volume_data_free);
  for (i = 0; i < num_volumes; i++)
  {
    guint8 flags;
    char *vol_name;

    GVfsAfpVolumeData *volume_data;

    REPLY_READ_BYTE (reply, &flags);
    REPLY_READ_PASCAL (reply, TRUE, &vol_name);
    if (!vol_name)
      continue;

    volume_data = g_slice_new (GVfsAfpVolumeData);
    volume_data->flags = flags;
    volume_data->name = vol_name;

    g_ptr_array_add (volumes, volume_data);
  }
  g_object_unref (reply);

  g_task_return_pointer (task, volumes, (GDestroyNotify)g_ptr_array_unref);
  g_object_unref (task);
  return;

invalid_reply:
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Unable to connect to the server. A communication problem occurred."));
  g_object_unref (task);
}

/**
 * g_vfs_afp_server_get_volumes:
 * 
 * @server: a #GVfsAfpServer
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously retrieves the volumes available on @server.
 */
void
g_vfs_afp_server_get_volumes (GVfsAfpServer       *server,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GVfsAfpCommand *comm;
  GTask *task;
  
  /* Get Server Parameters */
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_SRVR_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  task = g_task_new (server, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_vfs_afp_server_get_volumes);
  g_vfs_afp_connection_send_command (server->priv->conn, comm, NULL, get_volumes_cb,
                                     cancellable, task);
}

/**
 * g_vfs_afp_server_get_volumes_finish:
 * 
 * @server: a #GVfsAfpServer.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_server_get_volumes.
 * 
 * Returns: A #GPtrArray containing the volumes #GVfsAfpVolumeData structures or
 * %NULL on error.
 *                      
 */
GPtrArray *
g_vfs_afp_server_get_volumes_finish (GVfsAfpServer  *server,
                                     GAsyncResult   *result,
                                     GError         **error)
{
  g_return_val_if_fail (g_task_is_valid (result, server), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_vfs_afp_server_get_volumes), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

GVfsAfpVolume *
g_vfs_afp_server_mount_volume_sync (GVfsAfpServer *server,
                                    const char    *volume_name,
                                    GCancellable  *cancellable,
                                    GError **error)
{
  GVfsAfpVolume *volume;

  volume = g_vfs_afp_volume_new (server, server->priv->conn);
  if (!g_vfs_afp_volume_mount_sync (volume, volume_name, cancellable, error))
  {
    g_object_unref (volume);
    return NULL;
  }

  return volume;
}

static void
set_access_attributes_trusted (GFileInfo *info,
                               guint32 perm)
{
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
				     perm & 0x4);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
				     perm & 0x2);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				     perm & 0x1);
}

/* For files we don't own we can't trust a negative response to this check, as
   something else could allow us to do the operation, for instance an ACL
   or some sticky bit thing */
static void
set_access_attributes (GFileInfo *info,
                       guint32 perm)
{
  if (perm & 0x4)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
				       TRUE);
  if (perm & 0x2)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
				       TRUE);
  if (perm & 0x1)
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				       TRUE);
}

gboolean
g_vfs_afp_server_fill_info (GVfsAfpServer *server,
                            GFileInfo     *info,
                            GVfsAfpReply  *reply,
                            gboolean       directory,
                            guint16        bitmap,
                            GError         **error)
{
  goffset start_pos;

  if (directory)
  {
    const char *content_type = "inode/directory";
    GIcon *icon;
    
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, content_type);

    icon = g_content_type_get_icon (content_type);
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
    icon = g_content_type_get_symbolic_icon (content_type);
    g_file_info_set_symbolic_icon (info, icon);
    g_object_unref (icon);
  }
  else
    g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  
  start_pos = g_vfs_afp_reply_get_pos (reply);

  if (bitmap & AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT)
  {
    guint16 attributes;

    REPLY_READ_UINT16 (reply, &attributes);
    
    if (attributes & AFP_FILEDIR_ATTRIBUTES_BITMAP_INVISIBLE_BIT)
      g_file_info_set_is_hidden (info, TRUE);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_PARENT_DIR_ID_BIT)
  {
    guint32 parent_dir_id;

    REPLY_READ_UINT32 (reply, &parent_dir_id);
    g_file_info_set_attribute_uint32 (info, "afp::parent-dir-id", parent_dir_id);
  }
  
  if (bitmap & AFP_FILEDIR_BITMAP_CREATE_DATE_BIT)
  {
    gint32 create_date;
    gint64 create_date_local;

    REPLY_READ_INT32 (reply, &create_date);
    
    create_date_local = g_vfs_afp_server_time_to_local_time (server, create_date);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED,
                                      create_date_local);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_MOD_DATE_BIT)
  {
    gint32 mod_date;
    guint64 mod_date_unix;
    char *etag;

    REPLY_READ_INT32 (reply, &mod_date);
    mod_date_unix = g_vfs_afp_server_time_to_local_time (server, mod_date);
    
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                      mod_date_unix);

    etag = g_strdup_printf ("%"G_GUINT64_FORMAT, mod_date_unix);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
    g_free (etag);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_NODE_ID_BIT)
  {
    guint32 node_id;

    REPLY_READ_UINT32 (reply, &node_id);
    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_NODE_ID, node_id);
  }
  
  /* Directory specific attributes */
  if (directory)
  {
    if (bitmap & AFP_DIR_BITMAP_OFFSPRING_COUNT_BIT)
    {
      guint16 offspring_count;

      REPLY_READ_UINT16 (reply, &offspring_count);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_CHILDREN_COUNT,
                                        offspring_count);
    }
  }
  
  /* File specific attributes */
  else
  {
    if (bitmap & AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT)
    {
      guint64 fork_len;

      REPLY_READ_UINT64 (reply, &fork_len);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        fork_len);
    }
  }
  
  if (bitmap & AFP_FILEDIR_BITMAP_UTF8_NAME_BIT)
  {
    guint16 UTF8Name_offset;
    goffset old_pos;
    GVfsAfpName *afp_name;
    char *utf8_name;

    REPLY_READ_UINT16 (reply, &UTF8Name_offset);
    /* Pad */
    REPLY_READ_UINT32 (reply, NULL);

    old_pos = g_vfs_afp_reply_get_pos (reply);
    REPLY_SEEK (reply, start_pos + UTF8Name_offset, G_SEEK_SET);

    REPLY_READ_AFP_NAME (reply, TRUE, &afp_name);
    utf8_name = g_vfs_afp_name_get_string (afp_name);    
    g_vfs_afp_name_unref (afp_name);

    g_file_info_set_name (info, utf8_name);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                      utf8_name);

    /* Set file as hidden if it begins with a dot */
    if (utf8_name[0] == '.')
      g_file_info_set_is_hidden (info, TRUE);

    if (!directory)
    {
      char *content_type;
      gboolean uncertain_content_type;
      GIcon *icon;

      content_type = g_content_type_guess (utf8_name, NULL, 0, &uncertain_content_type);
      if (!uncertain_content_type)
        g_file_info_set_content_type (info, content_type);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
                                        content_type);

      icon = g_content_type_get_icon (content_type);
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      icon = g_content_type_get_symbolic_icon (content_type);
      g_file_info_set_symbolic_icon (info, icon);
      g_object_unref (icon);

      g_free (content_type);
    }
    
    g_free (utf8_name);

    REPLY_SEEK (reply, old_pos, G_SEEK_SET);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT)
  {
    guint32 uid, gid, permissions, ua_permissions;

    REPLY_READ_UINT32 (reply, &uid);
    REPLY_READ_UINT32 (reply, &gid);
    REPLY_READ_UINT32 (reply, &permissions);
    /* ua_permissions */
    REPLY_READ_UINT32 (reply, &ua_permissions);

    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, permissions);
    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, uid);
    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, gid);

    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_UA_PERMISSIONS,
                                      ua_permissions);
    
    if (uid == server->priv->user_id)
      set_access_attributes_trusted (info, (permissions >> 6) & 0x7);
    else if (gid == server->priv->group_id)
      set_access_attributes (info, (permissions >> 3) & 0x7);
    else
      set_access_attributes (info, (permissions >> 0) & 0x7);
  }

  return TRUE;

invalid_reply:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Unable to connect to the server “%s”. A communication problem occurred."),
               server->priv->info.server_name);

  return FALSE;
}

static void
map_id_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *conn = G_VFS_AFP_CONNECTION (source_object);
  GTask *task = G_TASK (user_data);
  GVfsAfpMapIDFunction function = GPOINTER_TO_INT (g_task_get_task_data (task));

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  gchar *name;

  reply = g_vfs_afp_connection_send_command_finish (conn, res, &err);
  if (!reply)
  {
    g_task_return_error (task, err);
    g_object_unref (task);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ITEM_NOT_FOUND:
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 _("Identification not found."));
        break;
      default:
        g_task_return_error (task, afp_result_code_to_gerror (res_code));
        break;
    }

    g_object_unref (task);
    return;
  }

  if (function == GVFS_AFP_MAP_ID_FUNCTION_USER_UUID_TO_UTF8_NAME ||
      function == GVFS_AFP_MAP_ID_FUNCTION_GROUP_UUID_TO_UTF8_NAME)
  {
    /* objType */
	REPLY_READ_UINT32 (reply, NULL);	
    /* id */
	REPLY_READ_UINT32 (reply, NULL);
  }

  if (function == GVFS_AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME ||
      function == GVFS_AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME)
  {
    REPLY_READ_PASCAL (reply, FALSE, &name);
  }
  else
  {
    GVfsAfpName *afp_name;

	
    REPLY_READ_AFP_NAME (reply, FALSE, &afp_name);
    name = g_vfs_afp_name_get_string (afp_name);
    g_vfs_afp_name_unref (afp_name);
  }

  g_task_return_pointer (task, name, g_free);
  g_object_unref (task);
  return;

invalid_reply:
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Unable to connect to the server. A communication problem occurred."));
  g_object_unref (task);
}

/**
 * g_vfs_afp_server_map_id:
 * 
 * @server: a #GVfsAfpServer.
 * @map_function: a #GVfsAfpMapIDFunction.
 * @id: the id to be mapped to a name.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously maps a user id, group id or uuid to a name.
 */
void
g_vfs_afp_server_map_id (GVfsAfpServer       *server,
                         GVfsAfpMapIDFunction map_function,
                         gint64               id,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GVfsAfpServerPrivate *priv;
  GVfsAfpCommand *comm;
  GTask *task;

  g_return_if_fail (G_VFS_IS_AFP_SERVER (server));

  priv = server->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_MAP_ID);

  /* SubFunction*/
  g_vfs_afp_command_put_byte (comm, map_function);

  /* ID */
  if (map_function == GVFS_AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME ||
      map_function == GVFS_AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME)
    g_vfs_afp_command_put_int32 (comm, id);
  else
    g_vfs_afp_command_put_int64 (comm, id);

  task = g_task_new (server, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_vfs_afp_server_map_id);
  g_task_set_task_data (task, GINT_TO_POINTER (map_function), NULL);

  g_vfs_afp_connection_send_command (priv->conn, comm, NULL,
                                     map_id_cb, cancellable, task);
  g_object_unref (comm);
}

/**
 * g_vfs_afp_server_map_id_finish:
 * 
 * @server: a #GVfsAfpServer.
 * @result: a #GAsyncResult.
 * @map_function: (out) optional out parameter to get the #GVfsAfpMapIDFunction
 * which was used, %NULL to ignore.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_server_map_id.
 * 
 * Returns: (transfer full): A string with the name of the id or %NULL
 * on error.
 */
char *
g_vfs_afp_server_map_id_finish (GVfsAfpServer        *server,
                                GAsyncResult         *res,
                                GVfsAfpMapIDFunction *map_function,
                                GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (res, server), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (res, g_vfs_afp_server_map_id), NULL);

  if (map_function)
    *map_function = GPOINTER_TO_INT (g_task_get_task_data (G_TASK (res)));

  return g_task_propagate_pointer (G_TASK (res), error);
}
