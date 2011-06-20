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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
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

#include "gvfsafpserver.h"

G_DEFINE_TYPE (GVfsAfpServer, g_vfs_afp_server, G_TYPE_OBJECT);

#define AFP_UAM_NO_USER   "No User Authent"
#define AFP_UAM_DHX       "DHCAST128"
#define AFP_UAM_DHX2      "DHX2"

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
dhx2_login (GVfsAfpServer *afp_serv,
            const char *username,
            const char *password,
            GCancellable *cancellable,
            GError **error)
{
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
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         _("Server doesn't support passwords longer than 256 characters"));
    goto error;
  }

  /* Request 1 */
  comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN);
  g_vfs_afp_command_put_pascal (comm, afp_version_to_string (afp_serv->version));
  g_vfs_afp_command_put_pascal (comm, AFP_UAM_DHX2);
  g_vfs_afp_command_put_pascal (comm, username);
  g_vfs_afp_command_pad_to_even (comm);

  res = g_vfs_afp_connection_send_command_sync (afp_serv->conn, comm,
                                                cancellable, error);
  g_object_unref (comm);
  if (!res)
    goto error;

  reply = g_vfs_afp_connection_read_reply_sync (afp_serv->conn, cancellable, error);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_AUTH_CONTINUE)
  {
    g_object_unref (reply);
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("An invalid username was provided"));
      goto error;
    }
    else
      goto generic_error;
  }
  
  /* Get data from reply */
  g_vfs_afp_reply_read_uint16 (reply, &id);

  /* read g */
  g_vfs_afp_reply_get_data (reply, 4, &tmp_buf);
  gcry_err = gcry_mpi_scan (&g, GCRYMPI_FMT_USG, tmp_buf, 4, NULL);
  g_assert (gcry_err == 0);

  g_vfs_afp_reply_read_uint16 (reply, &len);
  bits = len * 8;

  /* read p */
  g_vfs_afp_reply_get_data (reply, len, &tmp_buf);
  gcry_err = gcry_mpi_scan (&p, GCRYMPI_FMT_USG, tmp_buf, len, NULL);
  g_assert (gcry_err == 0);

  /* read Mb */
  g_vfs_afp_reply_get_data (reply, len, &tmp_buf);
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

  res = g_vfs_afp_connection_send_command_sync (afp_serv->conn, comm,
                                                cancellable, error);
  g_object_unref (comm);
  if (!res)
    goto error;

  
  reply = g_vfs_afp_connection_read_reply_sync (afp_serv->conn, cancellable, error);
  if (!reply)
    goto error;

  
  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_AUTH_CONTINUE)
  {
    g_object_unref (reply);
    goto generic_error;
  }

  /* read data from reply 2 */
  g_vfs_afp_reply_read_uint16 (reply, &id);

  g_vfs_afp_reply_get_data (reply, 32, &tmp_buf);
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


  res = g_vfs_afp_connection_send_command_sync (afp_serv->conn, comm,
                                                cancellable, error);
  g_object_unref (comm);
  if (!res)
    goto error;
  
  reply = g_vfs_afp_connection_read_reply_sync (afp_serv->conn, cancellable, error);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("AFP server %s declined the submitted password"),
                   afp_serv->server_name);
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
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Login to AFP server %s failed"), afp_serv->server_name);
  goto error;
}

static gboolean
dhx_login (GVfsAfpServer *afp_serv,
           const char *username,
           const char *password,
           GCancellable *cancellable,
           GError **error)
{
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
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         _("Server doesn't support passwords longer than 64 characters"));
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
  g_vfs_afp_command_put_pascal (comm, afp_version_to_string (afp_serv->version));
  g_vfs_afp_command_put_pascal (comm, AFP_UAM_DHX);
  g_vfs_afp_command_put_pascal (comm, username);
  g_vfs_afp_command_pad_to_even (comm);
  g_output_stream_write_all (G_OUTPUT_STREAM(comm), ma_buf, G_N_ELEMENTS (ma_buf),
                             NULL, NULL, NULL);

  res = g_vfs_afp_connection_send_command_sync (afp_serv->conn, comm,
                                                cancellable, error);
  g_object_unref (comm);
  if (!res)
    goto done;

  reply = g_vfs_afp_connection_read_reply_sync (afp_serv->conn, cancellable, error);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_AUTH_CONTINUE)
  {
    g_object_unref (reply);
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("An invalid username was provided"));
      goto error;
    }
    else
      goto generic_error;
  }

  g_vfs_afp_reply_read_uint16 (reply, &id);

  /* read Mb */
  g_vfs_afp_reply_get_data (reply, 16, &tmp_buf);
  gcry_err = gcry_mpi_scan (&mb, GCRYMPI_FMT_USG, tmp_buf, 16, NULL);
  g_assert (gcry_err == 0);

  /* read Nonce */
  g_vfs_afp_reply_get_data (reply, 32, &tmp_buf);
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


  res = g_vfs_afp_connection_send_command_sync (afp_serv->conn, comm,
                                                cancellable, error);
  g_object_unref (comm);
  if (!res)
    goto done;

  reply = g_vfs_afp_connection_read_reply_sync (afp_serv->conn, cancellable, error);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("AFP server %s declined the submitted password"),
                   afp_serv->server_name);
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
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Login to AFP server %s failed"), afp_serv->server_name);
  res = FALSE;
  goto done;
}
#endif

static gboolean
do_login (GVfsAfpServer *afp_serv,
          const char *username,
          const char *password,
          gboolean anonymous,
          GCancellable *cancellable,
          GError **error)
{
  /* anonymous login */
  if (anonymous)
  {
    GVfsAfpCommand *comm;
    gboolean res;
    GVfsAfpReply *reply;
    AfpResultCode res_code;
    
    if (!g_slist_find_custom (afp_serv->uams, AFP_UAM_NO_USER, g_str_equal))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("AFP server %s doesn't support anonymous login"),
                   afp_serv->server_name);
      return FALSE;
    }

    comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN);

    g_vfs_afp_command_put_pascal (comm, afp_version_to_string (afp_serv->version));
    g_vfs_afp_command_put_pascal (comm, AFP_UAM_NO_USER);
    res = g_vfs_afp_connection_send_command_sync (afp_serv->conn, comm,
                                                  cancellable, error);
    g_object_unref (comm);
    if (!res)
      return FALSE;

    reply = g_vfs_afp_connection_read_reply_sync (afp_serv->conn, cancellable, error);
    if (!reply)
      return FALSE;

    res_code = g_vfs_afp_reply_get_result_code (reply);
    g_object_unref (reply);
    
    if (res_code != AFP_RESULT_NO_ERROR)
    {
      if (res_code == AFP_RESULT_USER_NOT_AUTH)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("AFP server %s declined anonymous login"),
                     afp_serv->server_name);
        return FALSE;
      }
      else
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("Anonymous login to AFP server %s failed"), afp_serv->server_name);
        return FALSE;
      }
    }

    return TRUE;
  }

  else {

#ifdef HAVE_GCRYPT
    /* Diffie-Hellman 2 */
    if (g_slist_find_custom (afp_serv->uams, AFP_UAM_DHX2, g_str_equal))
      return dhx2_login (afp_serv, username, password, cancellable, error);
    
    /* Diffie-Hellman */
    if (g_slist_find_custom (afp_serv->uams, AFP_UAM_DHX, g_str_equal))
      return dhx_login (afp_serv, username, password, cancellable, error); 
#endif

	g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
	             _("Login to AFP server %s failed (no suitable authentication mechanism found)"),
	             afp_serv->server_name);
	return FALSE;
  }
}

static gboolean
get_server_info (GVfsAfpServer *afp_serv,
                 GCancellable *cancellable,
                 GError **error)
{
  GVfsAfpReply *reply;

  guint16 MachineType_offset, AFPVersionCount_offset, UAMCount_offset;
  
  guint8 count;
  guint i;

  reply = g_vfs_afp_connection_get_server_info (afp_serv->conn, cancellable,
                                                error);
  if (!reply)
    return FALSE;

  g_vfs_afp_reply_read_uint16 (reply, &MachineType_offset);
  g_vfs_afp_reply_read_uint16 (reply, &AFPVersionCount_offset);
  g_vfs_afp_reply_read_uint16 (reply, &UAMCount_offset);
  /* VolumeIconAndMask_offset */
  g_vfs_afp_reply_read_uint16 (reply, NULL);

  g_vfs_afp_reply_read_uint16 (reply, &afp_serv->flags);

  g_vfs_afp_reply_read_pascal (reply, &afp_serv->server_name);

  /* Parse UTF-8 ServerName */
  if (afp_serv->flags & (0x1 << 8)) {
    guint16 UTF8ServerName_offset;
    GVfsAfpName *utf8_server_name;

    g_vfs_afp_reply_skip_to_even (reply);
    g_vfs_afp_reply_seek (reply, 6, G_SEEK_CUR);
    g_vfs_afp_reply_read_uint16 (reply, &UTF8ServerName_offset);

    g_vfs_afp_reply_seek (reply, UTF8ServerName_offset, G_SEEK_SET);
    g_vfs_afp_reply_read_afp_name (reply, FALSE, &utf8_server_name);
    afp_serv->utf8_server_name = g_vfs_afp_name_get_string (utf8_server_name);
    g_vfs_afp_name_unref (utf8_server_name);
  }
    
  /* Parse MachineType */
  g_vfs_afp_reply_seek (reply, MachineType_offset, G_SEEK_SET);
  g_vfs_afp_reply_read_pascal (reply, &afp_serv->machine_type);
  
  /* Parse Versions */
  g_vfs_afp_reply_seek (reply, AFPVersionCount_offset, G_SEEK_SET);
  g_vfs_afp_reply_read_byte (reply, &count);
  for (i = 0; i < count; i++)
  {
    char *version;
    AfpVersion afp_version;

    g_vfs_afp_reply_read_pascal (reply, &version);
    afp_version = string_to_afp_version (version);
    if (afp_version > afp_serv->version)
      afp_serv->version = afp_version;
  }

  if (afp_serv->version == AFP_VERSION_INVALID)
  {
    g_object_unref (reply);
    g_set_error (error,
                 G_IO_ERROR, G_IO_ERROR_FAILED,
                 _("Failed to connect to server (%s)"), "Server doesn't support AFP version 3.0 or later");
    return FALSE;
  }

  /* Parse UAMs */
  g_vfs_afp_reply_seek (reply, UAMCount_offset, G_SEEK_SET);
  g_vfs_afp_reply_read_byte (reply, &count);
  for (i = 0; i < count; i++)
  {
    char *uam;

    g_vfs_afp_reply_read_pascal (reply, &uam);
    afp_serv->uams = g_slist_prepend (afp_serv->uams, uam);
  }

  
  return TRUE;
}

gboolean
g_vfs_afp_server_login (GVfsAfpServer *afp_serv,
                        const char     *initial_user,
                        GMountSource   *mount_source,
                        GCancellable   *cancellable,
                        GError         **error)
{
  gboolean res;
  char *user, *olduser;
  char *password;
  gboolean anonymous;
  GPasswordSave password_save;
  char *prompt = NULL;
  GError *err = NULL;

  res = get_server_info (afp_serv, cancellable, error);
  if (!res)
    return FALSE;

  olduser = g_strdup (initial_user);

  if (initial_user)
  {
    if (g_str_equal (initial_user, "anonymous") &&
        g_slist_find_custom (afp_serv->uams, AFP_UAM_NO_USER, g_str_equal))
    {
      user = NULL;
      password = NULL;
      anonymous = TRUE;
      goto try_login;
    }

    else if (g_vfs_keyring_lookup_password (initial_user,
                                            g_network_address_get_hostname (afp_serv->addr),
                                            NULL,
                                            "afp",
                                            NULL,
                                            NULL,
                                            g_network_address_get_port (afp_serv->addr),
                                            &user,
                                            NULL,
                                            &password) &&
             user != NULL &&
             password != NULL)
    {
      anonymous = FALSE;
      goto try_login;
    }
  }

  while (TRUE)
  {
    GString *str;
    GAskPasswordFlags flags;
    gboolean aborted;

    g_free (prompt);

    str = g_string_new (NULL);

    if (err)
    {
      g_string_append_printf (str, "%s\n", err->message);
      g_clear_error (&err);
    }
    
    /* create prompt */
    if (initial_user)
      /* Translators: the first %s is the username, the second the host name */
      g_string_append_printf (str, _("Enter password for afp as %s on %s"), initial_user, afp_serv->server_name);
    else
      /* translators: %s here is the hostname */
      g_string_append_printf (str, _("Enter password for afp on %s"), afp_serv->server_name);

    prompt = g_string_free (str, FALSE);

    flags = G_ASK_PASSWORD_NEED_PASSWORD;

    if (!initial_user)
    {
      flags |= G_ASK_PASSWORD_NEED_USERNAME;
      
      if (g_slist_find_custom (afp_serv->uams, AFP_UAM_NO_USER, g_str_equal))
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
                           _("Password dialog cancelled"));
      res = FALSE;
      break;
    }

try_login:

    /* Open connection */
    res = g_vfs_afp_connection_open (afp_serv->conn, cancellable, &err);
    if (!res)
      break;

    res = do_login (afp_serv, user, password, anonymous,
                    cancellable, &err);
    if (!res)
    {
      g_vfs_afp_connection_close (afp_serv->conn, cancellable, NULL);

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
  {
    g_free (user);
    g_free (password);

    g_propagate_error (error, err);
    return FALSE;
  }

  if (prompt && !anonymous)
  {
    /* a prompt was created, so we have to save the password */
    g_vfs_keyring_save_password (user,
                                 g_network_address_get_hostname (afp_serv->addr),
                                 NULL,
                                 "afp",
                                 NULL,
                                 NULL,
                                 g_network_address_get_port (afp_serv->addr),
                                 password,
                                 password_save);
    g_free (prompt);
  }

  g_free (user);
  g_free (password);

  return TRUE;
}

GVfsAfpServer *
g_vfs_afp_server_new (GNetworkAddress *addr)
{
  GVfsAfpServer *afp_serv;

  afp_serv = g_object_new (G_VFS_TYPE_AFP_SERVER, NULL);

  afp_serv->addr = addr;
  afp_serv->conn = g_vfs_afp_connection_new (G_SOCKET_CONNECTABLE (addr));

  return afp_serv;
}

static void
g_vfs_afp_server_init (GVfsAfpServer *afp_serv)
{
  afp_serv->machine_type = NULL;
  afp_serv->server_name = NULL;
  afp_serv->utf8_server_name = NULL;
  afp_serv->uams = NULL;
  afp_serv->version = AFP_VERSION_INVALID;
}

static void
g_vfs_afp_server_finalize (GObject *object)
{
  GVfsAfpServer *afp_serv = G_VFS_AFP_SERVER (object);
  
  g_free (afp_serv->machine_type);
  g_free (afp_serv->server_name);
  g_free (afp_serv->utf8_server_name);
  
  g_slist_free_full (afp_serv->uams, g_free);

  G_OBJECT_CLASS (g_vfs_afp_server_parent_class)->finalize (object);
}

static void
g_vfs_afp_server_class_init (GVfsAfpServerClass *klass)
{
  GObjectClass* object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = g_vfs_afp_server_finalize;
}

