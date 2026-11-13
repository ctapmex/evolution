/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "evolution-config.h"

#include <string.h>

#include <e-util/e-util.h>

#include "e-mail-parser-extension.h"
#include "e-mail-part-utils.h"

typedef EMailParserExtension EMailParserMultipartMixed;
typedef EMailParserExtensionClass EMailParserMultipartMixedClass;

GType e_mail_parser_multipart_mixed_get_type (void);

G_DEFINE_TYPE (
	EMailParserMultipartMixed,
	e_mail_parser_multipart_mixed,
	E_TYPE_MAIL_PARSER_EXTENSION)

static const gchar *parser_mime_types[] = {
	"multipart/mixed",
	"multipart/report",
	"multipart/*",
	NULL
};

static void
empe_mp_mixed_append_html (CamelMimePart *part,
			   GString *html,
			   GCancellable *cancellable)
{
	CamelContentType *ct;
	CamelDataWrapper *dw;

	ct = camel_mime_part_get_content_type (part);
	dw = camel_medium_get_content (CAMEL_MEDIUM (part));

	if (!ct || !dw)
		return;

	if (camel_content_type_is (ct, "text", "html")) {
		CamelStream *mem;
		GByteArray *bytes;

		mem = camel_stream_mem_new ();
		camel_data_wrapper_decode_to_stream_sync (dw, mem, cancellable, NULL);

		bytes = camel_stream_mem_get_byte_array (CAMEL_STREAM_MEM (mem));
		if (bytes && bytes->len)
			g_string_append_len (html, (const gchar *) bytes->data, bytes->len);

		g_object_unref (mem);
	} else if (CAMEL_IS_MULTIPART (dw) && (
		   camel_content_type_is (ct, "multipart", "alternative") ||
		   camel_content_type_is (ct, "multipart", "related"))) {
		gint ii, n;

		n = camel_multipart_get_number (CAMEL_MULTIPART (dw));
		for (ii = 0; ii < n; ii++)
			empe_mp_mixed_append_html (camel_multipart_get_part (CAMEL_MULTIPART (dw), ii), html, cancellable);
	}
}

/* Whether an HTML body among the parts of @mp shows @cid. The HTML is read
 * only once, on the first call. */
static gboolean
empe_mp_mixed_html_refers (CamelMultipart *mp,
			   GString **inout_html,
			   const gchar *cid,
			   GCancellable *cancellable)
{
	if (!*inout_html) {
		gint ii, n;

		*inout_html = g_string_new ("");

		n = camel_multipart_get_number (mp);
		for (ii = 0; ii < n; ii++)
			empe_mp_mixed_append_html (camel_multipart_get_part (mp, ii), *inout_html, cancellable);
	}

	return e_mail_part_utils_body_refers ((*inout_html)->str, cid);
}

static gboolean
empe_mp_mixed_parse (EMailParserExtension *extension,
                     EMailParser *parser,
                     CamelMimePart *part,
                     GString *part_id,
                     GCancellable *cancellable,
                     GQueue *out_mail_parts)
{
	CamelMultipart *mp;
	CamelMimePart *pgp_encrypted = NULL, *pgp_octet_stream = NULL;
	GString *html = NULL;
	gint i, nparts, len;

	mp = (CamelMultipart *) camel_medium_get_content ((CamelMedium *) part);

	if (!CAMEL_IS_MULTIPART (mp))
		return e_mail_parser_parse_part_as (
			parser, part, part_id,
			"application/vnd.evolution.source",
			cancellable, out_mail_parts);

	len = part_id->len;
	nparts = camel_multipart_get_number (mp);

	if ((nparts == 2 || nparts == 3) &&
	    !g_str_has_suffix (part_id->str, ".mixed-as-pgp-encrypted")) {
		for (i = 0; i < nparts; i++) {
			CamelMimePart *subpart;
			CamelContentType *ct;

			subpart = camel_multipart_get_part (mp, i);
			ct = camel_mime_part_get_content_type (subpart);

			if (ct) {
				if (camel_content_type_is (ct, "application", "pgp-encrypted")) {
					if (pgp_encrypted) {
						pgp_encrypted = NULL;
						break;
					}

					pgp_encrypted = subpart;
				} else if (camel_content_type_is (ct, "application", "octet-stream")) {
					if (pgp_octet_stream) {
						pgp_octet_stream = NULL;
						break;
					}

					pgp_octet_stream = subpart;
				}
			}
		}

		if (!pgp_encrypted || !pgp_octet_stream) {
			pgp_encrypted = NULL;
			pgp_octet_stream = NULL;
		}
	}

	for (i = 0; i < nparts; i++) {
		GQueue work_queue = G_QUEUE_INIT;
		EMailPart *mail_part;
		CamelMimePart *subpart;
		CamelContentType *ct;
		gboolean handled;

		subpart = camel_multipart_get_part (mp, i);

		if (subpart == pgp_encrypted ||
		    subpart == pgp_octet_stream) {
			/* Garbled PGP enctryped message by an Exchange server; show it
			   at the position, where the pgp-encrypted part is. */
			if (subpart == pgp_encrypted &&
			    pgp_encrypted && pgp_octet_stream) {
				CamelMultipart *encrypted;
				CamelMimePart *tmp_part;

				encrypted = CAMEL_MULTIPART (camel_multipart_encrypted_new ());
				camel_data_wrapper_set_mime_type (CAMEL_DATA_WRAPPER (encrypted), "multipart/encrypted; protocol=\"application/pgp-encrypted\"");
				camel_multipart_add_part (encrypted, pgp_encrypted);
				camel_multipart_add_part (encrypted, pgp_octet_stream);

				tmp_part = camel_mime_part_new ();
				camel_mime_part_set_content_type (tmp_part, "multipart/encrypted; protocol=\"application/pgp-encrypted\"");
				camel_medium_set_content (CAMEL_MEDIUM (tmp_part), CAMEL_DATA_WRAPPER (encrypted));

				g_string_append (part_id, ".mixed-as-pgp-encrypted");

				e_mail_parser_parse_part_as (parser, tmp_part, part_id,
					"multipart/encrypted", cancellable, out_mail_parts);

				g_string_truncate (part_id, len);

				g_object_unref (tmp_part);
				g_object_unref (encrypted);
			}
			continue;
		}

		if (i == 0 && (g_str_has_suffix (part_id->str, ".encrypted-pgp") || g_str_has_suffix (part_id->str, ".encrypted-pgp.signed.0"))) {
			ct = camel_mime_part_get_content_type (subpart);
			if (ct && camel_content_type_is (ct, "text", "rfc822-headers") &&
			    camel_content_type_param (ct, "protected-headers")) {
				/* Skip the text/rfc822-headers part, it's not needed to be shown */
				continue;
			}
		}

		g_string_append_printf (part_id, ".mixed.%d", i);

		handled = FALSE;
		ct = camel_mime_part_get_content_type (subpart);
		if (ct)
			ct = camel_content_type_ref (ct);

		if (!e_mail_parser_get_parsers_for_part (parser, subpart)) {
			gchar *guessed_mime_type;
			CamelContentType *guessed_ct = NULL;

			guessed_mime_type = e_mail_part_guess_mime_type (subpart);
			if (guessed_mime_type)
				guessed_ct = camel_content_type_decode (guessed_mime_type);

			if (guessed_ct && guessed_ct->type && guessed_ct->subtype && (
			    !ct || g_ascii_strcasecmp (guessed_ct->type, ct->type) != 0 ||
			    g_ascii_strcasecmp (guessed_ct->subtype, ct->subtype) != 0)) {
				CamelStream *mem_stream;

				mem_stream = camel_stream_mem_new ();
				if (camel_data_wrapper_decode_to_stream_sync (
					camel_medium_get_content (CAMEL_MEDIUM (subpart)),
					mem_stream, cancellable, NULL)) {
					CamelMimePart *opart;
					CamelDataWrapper *dw;

					g_seekable_seek (G_SEEKABLE (mem_stream), 0, G_SEEK_SET, cancellable, NULL);

					opart = camel_mime_part_new ();

					dw = camel_data_wrapper_new ();
					camel_data_wrapper_set_mime_type (dw, guessed_mime_type);
					if (camel_data_wrapper_construct_from_stream_sync (dw, mem_stream, cancellable, NULL)) {
						const gchar *disposition;

						camel_medium_set_content (CAMEL_MEDIUM (opart), dw);

						/* Copy Content-Disposition header, if available */
						disposition = camel_medium_get_header (CAMEL_MEDIUM (subpart), "Content-Disposition");
						if (disposition)
							camel_medium_set_header (CAMEL_MEDIUM (opart), "Content-Disposition", disposition);

						/* Copy also any existing parameters of the Content-Type, like 'name' or 'charset'. */
						if (ct && ct->params) {
							CamelHeaderParam *param;
							for (param = ct->params; param; param = param->next) {
								camel_content_type_set_param (guessed_ct, param->name, param->value);
							}
						}

						camel_content_type_set_param (guessed_ct, E_MAIL_PART_X_EVOLUTION_GUESSED, "1");
						camel_data_wrapper_set_mime_type_field (CAMEL_DATA_WRAPPER (opart), guessed_ct);

						handled = e_mail_parser_parse_part (parser, opart, part_id, cancellable, &work_queue);
						if (handled) {
							camel_content_type_unref (ct);
							ct = camel_content_type_ref (guessed_ct);
						}
					}

					g_object_unref (opart);
					g_object_unref (dw);
				}

				g_object_unref (mem_stream);
			}

			if (guessed_ct)
				camel_content_type_unref (guessed_ct);
			g_free (guessed_mime_type);
		}

		if (!handled) {
			handled = e_mail_parser_parse_part (
				parser, subpart, part_id, cancellable, &work_queue);
		}

		mail_part = g_queue_peek_head (&work_queue);

		/* A Content-ID does not by itself make a file attachment.
		 * Inline images and text stay in the message body and are
		 * shown through cid: when the HTML refers to them. List the
		 * part when Camel treats it as an attachment, the same rule
		 * as the message-list paperclip, or when it is hidden and no
		 * HTML shows it. */
		if (mail_part != NULL &&
		    e_mail_part_get_cid (mail_part) != NULL &&
		    (!e_mail_part_get_is_attachment (mail_part) ||
		     mail_part->is_hidden) &&
		    (camel_content_disposition_is_attachment_ex (
			camel_mime_part_get_content_disposition (subpart),
			ct,
			camel_mime_part_get_content_type (part)) ||
		     (mail_part->is_hidden &&
		      !empe_mp_mixed_html_refers (mp, &html, e_mail_part_get_cid (mail_part), cancellable)))) {

			e_mail_parser_wrap_as_attachment (parser, subpart, part_id, E_MAIL_PARSER_WRAP_ATTACHMENT_FLAG_NONE, &work_queue);

		/* Force messages to be expandable */
		} else if ((mail_part == NULL && !handled) ||
		    (camel_content_type_is (ct, "message", "*") &&
		     mail_part != NULL &&
		     !e_mail_part_get_is_attachment (mail_part))) {

			e_mail_parser_wrap_as_attachment (parser, subpart, part_id, E_MAIL_PARSER_WRAP_ATTACHMENT_FLAG_NONE, &work_queue);

			mail_part = g_queue_peek_head (&work_queue);

			if (mail_part != NULL)
				mail_part->force_inline = TRUE;
		}

		e_queue_transfer (&work_queue, out_mail_parts);

		g_string_truncate (part_id, len);

		if (ct)
			camel_content_type_unref (ct);
	}

	if (html)
		g_string_free (html, TRUE);

	return TRUE;
}

static void
e_mail_parser_multipart_mixed_class_init (EMailParserExtensionClass *class)
{
	class->mime_types = parser_mime_types;
	class->priority = G_PRIORITY_LOW;
	class->flags = E_MAIL_PARSER_EXTENSION_COMPOUND_TYPE;
	class->parse = empe_mp_mixed_parse;
}

static void
e_mail_parser_multipart_mixed_init (EMailParserExtension *extension)
{
}
