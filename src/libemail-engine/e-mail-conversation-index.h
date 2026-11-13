/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef E_MAIL_CONVERSATION_INDEX_H
#define E_MAIL_CONVERSATION_INDEX_H

#include <libemail-engine/e-mail-session.h>

G_BEGIN_DECLS

/**
 * EMailConversationHit:
 * @store: store that contains the message
 * @folder_name: full folder name inside @store
 * @uid: message UID inside that folder
 *
 * One message, outside the folder being viewed, that belongs to a
 * conversation already present in that folder.
 **/
typedef struct _EMailConversationHit {
	CamelStore *store;
	gchar *folder_name;
	gchar *uid;
} EMailConversationHit;

void		e_mail_conversation_hit_free	(EMailConversationHit *hit);

GPtrArray *	e_mail_conversation_index_find_related
						(EMailSession *session,
						 CamelFolder *folder,
						 GHashTable *seed_ids,
						 GHashTable *known_message_ids,
						 gboolean hide_deleted,
						 gboolean hide_junk,
						 GCancellable *cancellable,
						 GError **error);
void		e_mail_conversation_index_invalidate_folder
						(CamelStore *store,
						 const gchar *folder_name);
gboolean	e_mail_conversation_index_change_affects_folder
						(EMailSession *session,
						 CamelFolder *view_folder,
						 CamelStore *changed_store,
						 const gchar *changed_folder_name,
						 gint new_messages);

G_END_DECLS

#endif /* E_MAIL_CONVERSATION_INDEX_H */
