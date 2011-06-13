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
#include <gio/gio.h>

#ifdef HAVE_GCRYPT
#include <gcrypt.h>
#endif

#include "gvfsjobmount.h"
#include "gvfsjobenumerate.h"
#include "gvfskeyring.h"


#include "gvfsbackendafp.h"



G_DEFINE_TYPE (GVfsBackendAfp, g_vfs_backend_afp, G_VFS_TYPE_BACKEND);


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

static void
get_srvr_parms_cb (GVfsAfpConnection *afp_connection,
                   GVfsAfpReply      *reply,
                   GError            *error,
                   gpointer           user_data)
{
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  AfpResultCode res_code;
  guint8 num_volumes, i;

  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Volume enumeration failed"));
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  /* server time */
  (void)g_data_input_stream_read_int32 (G_DATA_INPUT_STREAM (reply), NULL, NULL);

  num_volumes = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  for (i = 0; i < num_volumes; i++)
  {
    guint8 flags;
    char *vol_name;

    GFileInfo *info;
    GIcon *icon;
    GMountSpec *mount_spec;
    char *uri;

    flags = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
    vol_name = g_vfs_afp_reply_read_pascal (reply);
    if (!vol_name)
      continue;
    
    info = g_file_info_new ();
    
    g_file_info_set_name (info, vol_name);
    g_file_info_set_display_name (info, vol_name);
    g_file_info_set_edit_name (info, vol_name);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
    g_file_info_set_content_type (info, "inode/directory");
    g_file_info_set_file_type (info, G_FILE_TYPE_MOUNTABLE);

    g_file_info_set_attribute_boolean (info, "afp::volume-password-protected", (flags & 0x01));

    icon = g_themed_icon_new ("folder-remote");
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
    
    mount_spec = g_mount_spec_new ("afp-volume");
    g_mount_spec_set (mount_spec, "host",
                      g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
    g_mount_spec_set (mount_spec, "volume", vol_name);

    if (g_mount_tracker_has_mount_spec (afp_backend->mount_tracker, mount_spec))
    {
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, TRUE);
    }
    else
    {
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, TRUE);
      g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, FALSE);
    }
    g_mount_spec_unref (mount_spec);

    uri = g_strdup_printf ("afp://%s/%s",
                           g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)),
                           vol_name);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
                                      uri);
    g_free (uri);

    g_vfs_job_enumerate_add_info (job, info);
    g_object_unref (info);
  }

  g_vfs_job_enumerate_done (job);
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
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  GVfsAfpCommand *comm;
  
  if (!is_root(filename))
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      _("File doesn't exist"));
    return TRUE;
  }

  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_SRVR_PARMS);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  g_vfs_afp_connection_queue_command (afp_backend->conn, comm,
                                      get_srvr_parms_cb,
                                      G_VFS_JOB (job)->cancellable, job);
  g_object_unref (comm);

  return TRUE;
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  if (is_root (filename))
  {
    GIcon *icon;
    
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_name (info, "/");
    g_file_info_set_display_name (info, g_vfs_backend_get_display_name (backend));
    g_file_info_set_content_type (info, "inode/directory");
    icon = g_vfs_backend_get_icon (backend);
    if (icon != NULL)
      g_file_info_set_icon (info, icon);
    g_vfs_job_succeeded (G_VFS_JOB (job));
  }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));
  
  return TRUE;
}

#ifdef HAVE_GCRYPT
static gboolean
dhx_login (GVfsBackendAfp *afp_backend,
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

  /* Mb */
  guint8 mb_buf[16];
  gcry_mpi_t mb;

  /* Nonce */
  guint8 nonce_buf[32];
  gcry_mpi_t nonce, nonce1;

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
  g_vfs_afp_command_put_pascal (comm, afp_version_to_string (afp_backend->version));
  g_vfs_afp_command_put_pascal (comm, AFP_UAM_DHX);
  g_vfs_afp_command_put_pascal (comm, username);
  g_vfs_afp_command_pad_to_even (comm);
  g_output_stream_write_all (G_OUTPUT_STREAM(comm), ma_buf, G_N_ELEMENTS (ma_buf),
                             NULL, NULL, NULL);

  res = g_vfs_afp_connection_send_command_sync (afp_backend->conn, comm,
                                                cancellable, error);
  g_object_unref (comm);
  if (!res)
    goto done;

  reply = g_vfs_afp_connection_read_reply_sync (afp_backend->conn, cancellable, error);
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

  id = g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply),
                                        NULL, NULL);

  /* read Mb */
  g_input_stream_read_all (G_INPUT_STREAM (reply), mb_buf, G_N_ELEMENTS (mb_buf),
                           NULL, NULL, NULL);
  gcry_err = gcry_mpi_scan (&mb, GCRYMPI_FMT_USG, mb_buf, G_N_ELEMENTS (mb_buf), NULL);
  g_assert (gcry_err == 0);

  /* read Nonce */
  g_input_stream_read_all (G_INPUT_STREAM (reply), nonce_buf, G_N_ELEMENTS (nonce_buf),
                           NULL, NULL, NULL);

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
  nonce1 = gcry_mpi_new (128);
  gcry_mpi_add_ui (nonce1, nonce, 1);
  gcry_mpi_release (nonce);

  /* set client->server initialization vector */
  gcry_cipher_setiv (cipher, C2SIV, G_N_ELEMENTS (C2SIV));
  
  /* create encrypted answer */
  gcry_err = gcry_mpi_print (GCRYMPI_FMT_USG, answer_buf, 16, &len, nonce1);
  g_assert (gcry_err == 0);
  gcry_mpi_release (nonce1);

  if (len < 16)
  {
    memmove(answer_buf + 16 - len, answer_buf, len);
    memset(answer_buf, 0, 16 - len);
  }
  
  memcpy (answer_buf + 16, password, strlen (password));

  gcry_err = gcry_cipher_encrypt (cipher, answer_buf, G_N_ELEMENTS (answer_buf),
                                  NULL, 0);
  g_assert (gcry_err == 0);


  /* Create Login Continue command */
  comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN_CONT);
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), id, NULL, NULL);
  g_output_stream_write_all (G_OUTPUT_STREAM (comm), answer_buf,
                             G_N_ELEMENTS (answer_buf), NULL, NULL, NULL);


  res = g_vfs_afp_connection_send_command_sync (afp_backend->conn, comm,
                                                cancellable, error);
  g_object_unref (comm);
  if (!res)
    goto done;

  reply = g_vfs_afp_connection_read_reply_sync (afp_backend->conn, cancellable, error);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    if (res_code == AFP_RESULT_USER_NOT_AUTH)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("Server \"%s\" declined the submitted password"),
                   afp_backend->server_name);
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
               _("Login to server \"%s\" failed"), afp_backend->server_name);
  res = FALSE;
  goto done;
}
#endif

static gboolean
do_login (GVfsBackendAfp *afp_backend,
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
    
    if (!g_slist_find_custom (afp_backend->uams, AFP_UAM_NO_USER, g_str_equal))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("Server \"%s\" doesn't support anonymous login"),
                   afp_backend->server_name);
      return FALSE;
    }

    comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN);

    g_vfs_afp_command_put_pascal (comm, afp_version_to_string (afp_backend->version));
    g_vfs_afp_command_put_pascal (comm, AFP_UAM_NO_USER);
    res = g_vfs_afp_connection_send_command_sync (afp_backend->conn, comm,
                                                  cancellable, error);
    g_object_unref (comm);
    if (!res)
      return FALSE;

    reply = g_vfs_afp_connection_read_reply_sync (afp_backend->conn, cancellable, error);
    if (!reply)
      return FALSE;

    res_code = g_vfs_afp_reply_get_result_code (reply);
    g_object_unref (reply);
    
    if (res_code != AFP_RESULT_NO_ERROR)
    {
      if (res_code == AFP_RESULT_USER_NOT_AUTH)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("Anonymous login on server \"%s\" failed"),
                     afp_backend->server_name);
        return FALSE;
      }
      else
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("Login to server \"%s\" failed"), afp_backend->server_name);
        return FALSE;
      }
    }

    return TRUE;
  }

  else {

#ifdef HAVE_GCRYPT
    /* Diffie-Hellman */
    if (g_slist_find_custom (afp_backend->uams, AFP_UAM_DHX, g_str_equal))
      return dhx_login (afp_backend, username, password, cancellable, error); 
#endif
    
    else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Login to server \"%s\" failed, no suitable authentication mechanism found"),
                   afp_backend->server_name);
      return FALSE;
    }
  }
}

static gboolean
g_vfs_afp_server_login (GVfsBackendAfp *afp_backend,
                        char           *initial_user,
                        GMountSource   *mount_source,
                        GCancellable   *cancellable,
                        GError         **error)
{
  GNetworkAddress *addr;
  char *user, *olduser;
  char *password;
  gboolean anonymous;
  GPasswordSave password_save;
  char *prompt = NULL;
  gboolean res;
  GError *err = NULL;

  addr = G_NETWORK_ADDRESS (afp_backend->addr);

  olduser = g_strdup (initial_user);

  if (initial_user)
  {
    if (g_str_equal (initial_user, "anonymous"))
    {
      user = NULL;
      password = NULL;
      anonymous = TRUE;
      goto try_login;
    }

    else if (g_vfs_keyring_lookup_password (initial_user,
                                            g_network_address_get_hostname (addr),
                                            NULL,
                                            "afp",
                                            NULL,
                                            NULL,
                                            g_network_address_get_port (addr),
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
    GAskPasswordFlags flags;
    gboolean aborted;

    if (prompt == NULL)
    {
      /* create prompt */
      if (initial_user)
        /* Translators: the first %s is the username, the second the host name */
        prompt = g_strdup_printf (_("Enter password for afp as %s on %s"), initial_user, afp_backend->server_name);
      else
        /* translators: %s here is the hostname */
        prompt = g_strdup_printf (_("Enter password for afp on %s"), afp_backend->server_name);
    }
    
    flags = G_ASK_PASSWORD_NEED_PASSWORD;

    if (!initial_user)
      flags |= G_ASK_PASSWORD_NEED_USERNAME | G_ASK_PASSWORD_ANONYMOUS_SUPPORTED;

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
    res = g_vfs_afp_connection_open (afp_backend->conn, cancellable, &err);
    if (!res)
      break;
    
    res = do_login (afp_backend, user, password, anonymous,
                    cancellable, &err);
    if (!res)
    {
      g_vfs_afp_connection_close (afp_backend->conn, cancellable, NULL);
      
      if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        g_clear_error (&err);
      else
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
                                 g_network_address_get_hostname (addr),
                                 NULL,
                                 "afp",
                                 NULL,
                                 NULL,
                                 g_network_address_get_port (addr),
                                 password,
                                 password_save);
    g_free (prompt);
  }

  g_free (user);
  g_free (password);

  return TRUE;
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  gboolean res;
  GError *err = NULL;
  GVfsAfpReply *reply;

  guint16 MachineType_offset, AFPVersionCount_offset, UAMCount_offset;
  guint16 VolumeIconAndMask_offset, Flags;
  guint8 count;
  guint i;

  GMountSpec *afp_mount_spec;
  char       *display_name;

  afp_backend->conn = g_vfs_afp_connection_new (afp_backend->addr);

  reply = g_vfs_afp_connection_get_server_info (afp_backend->conn,
                                                G_VFS_JOB (job)->cancellable,
                                                &err);
  if (!reply)
    goto error;

  MachineType_offset =
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  AFPVersionCount_offset = 
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  UAMCount_offset =
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  VolumeIconAndMask_offset =
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  Flags =
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  
  afp_backend->server_name = g_vfs_afp_reply_read_pascal (reply);

  /* Parse Versions */
  g_vfs_afp_reply_seek (reply, AFPVersionCount_offset, G_SEEK_SET);
  count = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  for (i = 0; i < count; i++)
  {
    char *version;
    AfpVersion afp_version;

    version = g_vfs_afp_reply_read_pascal (reply);
    afp_version = string_to_afp_version (version);
    if (afp_version > afp_backend->version)
      afp_backend->version = afp_version;
  }

  if (afp_backend->version == AFP_VERSION_INVALID)
  {
    g_object_unref (reply);
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Failed to connect to server (%s)"), "Server doesn't support AFP version 3.0 or later");
    return;
  }
  
  /* Parse UAMs */
  g_vfs_afp_reply_seek (reply, UAMCount_offset, G_SEEK_SET);
  count = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  for (i = 0; i < count; i++)
  {
    char *uam;

    uam = g_vfs_afp_reply_read_pascal (reply);
    afp_backend->uams = g_slist_prepend (afp_backend->uams, uam);
  }
  g_object_unref (reply);

  res = g_vfs_afp_server_login (afp_backend, afp_backend->user, mount_source,
                                G_VFS_JOB (job)->cancellable, &err);
  if (!res)
    goto error;
  
  /* set mount info */
  afp_mount_spec = g_mount_spec_new ("afp-server");
  g_mount_spec_set (afp_mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  if (afp_backend->user)
    g_mount_spec_set (afp_mount_spec, "user", afp_backend->user);

  g_vfs_backend_set_mount_spec (backend, afp_mount_spec);
  g_mount_spec_unref (afp_mount_spec);
  
  display_name = g_strdup_printf (_("AFP shares on %s"), afp_backend->server_name);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  g_vfs_backend_set_icon_name (backend, "network-server");
  g_vfs_backend_set_user_visible (backend, FALSE);

    
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
}
  
static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
	GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
	
	const char *host, *portstr, *user;
	guint16 port = 548;
	
	host = g_mount_spec_get (mount_spec, "host");
	if (host == NULL)
		{
			g_vfs_job_failed (G_VFS_JOB (job),
			                  G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			                  _("No hostname specified"));
			return TRUE;
		}

	portstr = g_mount_spec_get (mount_spec, "port");
	if (portstr != NULL)
		{
			port = atoi (portstr);
		}

	afp_backend->addr = g_network_address_new (host, port);
	
	user = g_mount_spec_get (mount_spec, "user");
	afp_backend->user = g_strdup (user);
	
	return FALSE;
}

static void
g_vfs_backend_afp_init (GVfsBackendAfp *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);

  afp_backend->mount_tracker = g_mount_tracker_new (NULL);
  
  afp_backend->server_name = NULL;
  afp_backend->uams = NULL;
  afp_backend->version = AFP_VERSION_INVALID;
}

static void
g_vfs_backend_afp_finalize (GObject *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);
  
  g_free (afp_backend->server_name);
  g_slist_free_full (afp_backend->uams, g_free);
  
	G_OBJECT_CLASS (g_vfs_backend_afp_parent_class)->finalize (object);
}

static void
g_vfs_backend_afp_class_init (GVfsBackendAfpClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

	object_class->finalize = g_vfs_backend_afp_finalize;

	backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
}

void
g_vfs_afp_daemon_init (void)
{
  g_set_application_name (_("Apple Filing Protocol Service"));

#ifdef HAVE_GCRYPT
  gcry_check_version (NULL);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
#endif
}
