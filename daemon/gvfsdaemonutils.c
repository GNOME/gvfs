/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>

#include <gio/gio.h>
#ifdef HAVE_GCR
#define GCR_API_SUBJECT_TO_CHANGE
#include <gcr/gcr.h>
#endif
#include "gvfsdaemonutils.h"
#include "gvfsdaemonprotocol.h"


char *
g_error_to_daemon_reply (GError *error, guint32 seq_nr, gsize *len_out)
{
  char *buffer;
  const char *domain;
  gsize domain_len, message_len;
  GVfsDaemonSocketProtocolReply *reply;
  gsize len;
  
  domain = g_quark_to_string (error->domain);
  domain_len = strlen (domain);
  message_len = strlen (error->message);

  len = G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE +
    domain_len + 1 + message_len + 1;
  buffer = g_malloc (len);

  reply = (GVfsDaemonSocketProtocolReply *)buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR);
  reply->seq_nr = g_htonl (seq_nr);
  reply->arg1 = g_htonl (error->code);
  reply->arg2 = g_htonl (domain_len + 1 + message_len + 1);

  memcpy (buffer + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
	  domain, domain_len + 1);
  memcpy (buffer + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE + domain_len + 1,
	  error->message, message_len + 1);
  
  *len_out = len;
  
  return buffer;
}

/**
 * gvfs_file_info_populate_default:
 * @info: file info to populate
 * @name_string: a bytes string of possibly the full path to the given file
 * @type: type of this file
 *
 * Calls gvfs_file_info_populate_names_as_local() and 
 * gvfs_file_info_populate_content_types() on the given @name_string.
 **/
void
gvfs_file_info_populate_default (GFileInfo  *info,
                                 const char *name_string,
			         GFileType   type)
{
  char *edit_name;

  g_return_if_fail (G_IS_FILE_INFO (info));
  g_return_if_fail (name_string != NULL);

  edit_name = gvfs_file_info_populate_names_as_local (info, name_string);
  gvfs_file_info_populate_content_types (info, edit_name, type);
  g_free (edit_name);
}

/**
 * gvfs_file_info_populate_names_as_local:
 * @info: the file info to fill
 * @name_string: a bytes string of possibly the full path to the given file
 *
 * Sets the name of the file info to @name_string and determines display and 
 * edit name for it.
 *
 * This generates the display name based on what encoding is used for local filenames.
 * It might be a good thing to use if you have no idea of the remote system filename
 * encoding, but if you know the actual encoding use, or if you allow per-mount
 * configuration of filename encoding in your backend you should not use this.
 * 
 * Returns: the utf-8 encoded edit name for the given file.
 **/
char *
gvfs_file_info_populate_names_as_local (GFileInfo  *info,
					const char *name_string)
{
  //const char *slash;
  char *edit_name;

  g_return_val_if_fail (G_IS_FILE_INFO (info), NULL);
  g_return_val_if_fail (name_string != NULL, NULL);

#if 0
  slash = strrchr (name_string, '/');
  if (slash && slash[1])
    name_string = slash + 1;
#endif
  edit_name = g_filename_display_basename (name_string);
  g_file_info_set_edit_name (info, edit_name);

  if (strstr (edit_name, "\357\277\275") != NULL)
    {
      char *display_name;
      
      display_name = g_strconcat (edit_name, _(" (invalid encoding)"), NULL);
      g_file_info_set_display_name (info, display_name);
      g_free (display_name);
    }
  else
    g_file_info_set_display_name (info, edit_name);

  return edit_name;
}

/**
 * gvfs_file_info_populate_content_types:
 * @info: the file info to fill
 * @basename: utf-8 encoded base name of file
 * @type: type of this file
 *
 * Takes the base name and guesses content type and icon with it. This function
 * is intended for remote files. Do not use it for directories.
 **/
void
gvfs_file_info_populate_content_types (GFileInfo  *info,
				       const char *basename,
				       GFileType   type)
{
  char *free_mimetype = NULL;
  const char *mimetype;
  gboolean uncertain_content_type = FALSE;
  GIcon *icon;
  GIcon *symbolic_icon;

  g_return_if_fail (G_IS_FILE_INFO (info));
  g_return_if_fail (basename != NULL);

  g_file_info_set_file_type (info, type);

  switch (type)
    {
      case G_FILE_TYPE_DIRECTORY:
	mimetype = "inode/directory";
	break;
      case G_FILE_TYPE_SYMBOLIC_LINK:
	mimetype = "inode/symlink";
	break;
      case G_FILE_TYPE_SPECIAL:
	mimetype = "inode/special";
	break;
      case G_FILE_TYPE_SHORTCUT:
	mimetype = "inode/shortcut";
	break;
      case G_FILE_TYPE_MOUNTABLE:
	mimetype = "inode/mountable";
	break;
      case G_FILE_TYPE_REGULAR:
	free_mimetype = g_content_type_guess (basename, NULL, 0, &uncertain_content_type);
	mimetype = free_mimetype;
	break;
      case G_FILE_TYPE_UNKNOWN:
      default:
        mimetype = "application/octet-stream";
	break;
    }

  if (!uncertain_content_type)
    g_file_info_set_content_type (info, mimetype);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, mimetype);

  icon = g_content_type_get_icon (mimetype);
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);

  symbolic_icon = g_content_type_get_symbolic_icon (mimetype);
  g_file_info_set_symbolic_icon (info, symbolic_icon);
  g_object_unref (symbolic_icon);

  g_free (free_mimetype);
}

/**
 * gvfs_seek_type_to_lseek:
 * @type: the seek type
 *
 * Takes a GSeekType and converts it to an lseek type.
 **/
int
gvfs_seek_type_to_lseek (GSeekType type)
{
  switch (type)
    {
    case G_SEEK_CUR:
      return SEEK_CUR;
    case G_SEEK_SET:
      return SEEK_SET;
    case G_SEEK_END:
      return SEEK_END;
    default:
      return -1;
    }
}

#ifdef HAVE_GCR
/* Convert GTlsCertificateFlags into a message to display to the user. */
static char *
certificate_flags_to_string (GTlsCertificateFlags errors)
{
  GString *reason;

  g_return_val_if_fail (errors, NULL);

  reason = g_string_new (NULL);

  if (errors & G_TLS_CERTIFICATE_UNKNOWN_CA)
    g_string_append_printf (reason, "\n\t%s", _("The signing certificate authority is not known."));
  if (errors & G_TLS_CERTIFICATE_BAD_IDENTITY)
    g_string_append_printf (reason, "\n\t%s", _("The certificate does not match the identity of the site."));
  if (errors & G_TLS_CERTIFICATE_NOT_ACTIVATED)
    g_string_append_printf (reason, "\n\t%s", _("The certificate’s activation time is in the future."));
  if (errors & G_TLS_CERTIFICATE_EXPIRED)
    g_string_append_printf (reason, "\n\t%s", _("The certificate has expired."));
  if (errors & G_TLS_CERTIFICATE_REVOKED)
    g_string_append_printf (reason, "\n\t%s", _("The certificate has been revoked."));
  if (errors & G_TLS_CERTIFICATE_INSECURE)
    g_string_append_printf (reason, "\n\t%s", _("The certificate’s algorithm is considered insecure."));
  if (errors & G_TLS_CERTIFICATE_GENERIC_ERROR)
    g_string_append_printf (reason, "\n\t%s", _("Error occurred when validating the certificate."));

  return g_string_free (reason, FALSE);
}

/* Convert a GTlsCertificate into a string to display to the user.
 * It contains the identity, the issuer, the expiry date and the certificate
 * fingerprint. With this information, a user can make an informed decision
 * whether to trust it or not. */
static char *
certificate_to_string (GTlsCertificate *certificate)
{
  GByteArray *certificate_data;
  GcrCertificate *simple_certificate;
  GDateTime *date;
  gchar *date_str;
  char *subject_name, *issuer_name, *fingerprint, *certificate_str;

  g_object_get (certificate, "certificate", &certificate_data, NULL);
  simple_certificate = gcr_simple_certificate_new_static (certificate_data->data,
						          certificate_data->len);

  date = gcr_certificate_get_expiry_date (simple_certificate);
  date_str = g_date_time_format (date, "%x");
  g_date_time_unref (date);

  subject_name = gcr_certificate_get_subject_name (simple_certificate);
  issuer_name = gcr_certificate_get_issuer_name (simple_certificate);
  fingerprint = gcr_certificate_get_fingerprint_hex (simple_certificate, G_CHECKSUM_SHA1);

  certificate_str = g_strdup_printf ("Certificate information:\n"
                                     "\tIdentity: %s\n"
                                     "\tVerified by: %s\n"
                                     "\tExpires: %s\n"
                                     "\tFingerprint (SHA1): %s",
                                     subject_name,
                                     issuer_name,
                                     date_str,
                                     fingerprint);
  g_object_unref (simple_certificate);
  g_byte_array_unref (certificate_data);
  g_free (date_str);
  g_free (subject_name);
  g_free (issuer_name);
  g_free (fingerprint);

  return certificate_str;
}

/**
 * gvfs_accept_certificate:
 * @mount_source: a GMountSource to ask the user a question
 * @certificate: the certificate presented by the site
 * @errors: flags describing the verification failure(s)
 *
 * Given a certificate presented by a site whose identity can't be verified,
 * query the user whether they accept the certificate.
 **/
gboolean
gvfs_accept_certificate (GMountSource *mount_source,
                         GTlsCertificate *certificate,
                         GTlsCertificateFlags errors)
{
  const char *choices[] = {_("Yes"), _("No"), NULL};
  int choice;
  gboolean handled, aborted = FALSE;
  char *certificate_str, *reason, *message;

  if (certificate == NULL)
    return FALSE;

  certificate_str = certificate_to_string (certificate);
  reason = certificate_flags_to_string (errors);

  /* Translators: The first %s is the reason why verification failed, the second a certificate */
  message = g_strdup_printf (_("Identity Verification Failed\n"
                               "%s\n\n"
                               "%s\n\n"
                               "Are you really sure you would like to continue?"),
                             reason,
                             certificate_str);
  handled = g_mount_source_ask_question (mount_source,
                                         message,
                                         choices,
                                         &aborted,
                                         &choice);
  g_free (certificate_str);
  g_free (reason);
  g_free (message);

  if (handled && choice == 0)
    return TRUE;

  return FALSE;
}
#else
gboolean
gvfs_accept_certificate (GMountSource *mount_source,
                         GTlsCertificate *certificate,
                         GTlsCertificateFlags errors)
{
  return FALSE;
}
#endif

gssize
gvfs_output_stream_splice (GOutputStream *stream,
                           GInputStream *source,
                           GOutputStreamSpliceFlags flags,
                           goffset total_size,
                           GFileProgressCallback progress_callback,
                           gpointer progress_callback_data,
                           GCancellable *cancellable,
                           GError **error)
{
  gssize n_read, n_written;
  gsize bytes_copied;
  char buffer[8192], *p;
  gboolean res;

  bytes_copied = 0;
  res = TRUE;
  do
    {
      n_read = g_input_stream_read (source, buffer, sizeof (buffer), cancellable, error);
      if (n_read == -1)
        {
          res = FALSE;
          break;
        }

      if (n_read == 0)
        break;

      p = buffer;
      while (n_read > 0)
        {
          n_written = g_output_stream_write (stream, p, n_read, cancellable, error);
          if (n_written == -1)
            {
              res = FALSE;
              break;
            }

          p += n_written;
          n_read -= n_written;
          bytes_copied += n_written;

          if (progress_callback)
            progress_callback (bytes_copied, total_size, progress_callback_data);
        }

      if (bytes_copied > G_MAXSSIZE)
        bytes_copied = G_MAXSSIZE;
    }
  while (res);

  if (!res)
    error = NULL; /* Ignore further errors */

  if (flags & G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE)
    {
      /* Don't care about errors in source here */
      g_input_stream_close (source, cancellable, NULL);
    }

  if (flags & G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET)
    {
      /* But write errors on close are bad! */
      if (!g_output_stream_close (stream, cancellable, error))
        res = FALSE;
    }

  if (res)
    return bytes_copied;

  return -1;
}
