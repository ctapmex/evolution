/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "evolution-config.h"

#include "e-mail-conversation-index.h"

#include <libedataserver/libedataserver.h>

#include "e-mail-folder-utils.h"

/* How many times to follow replies-of-replies before stopping. */
#define CONVERSATION_INDEX_MAX_PASSES 8

typedef struct _ConvMsg {
	gchar *folder_name;
	gchar *uid;
	guint32 flags;
	guint64 message_id;
	GArray *references; /* guint64 */
} ConvMsg;

typedef struct _FolderSlice {
	gchar *folder_name;
	GPtrArray *messages; /* ConvMsg * */
} FolderSlice;

typedef struct _StoreIndex {
	GMutex lock;
	GHashTable *folders; /* folder name -> FolderSlice * */
	GHashTable *by_id; /* guint64 * -> GPtrArray of ConvMsg * */
	GHashTable *dirty; /* folder names whose slice must be reread locally */
	GHashTable *saved_counts; /* folder name -> saved message count */
	gboolean scanned;
} StoreIndex;

static GMutex index_create_lock;

static guint
id_hash (gconstpointer key)
{
	guint64 value = * ((const guint64 *) key);

	return (guint) (value ^ (value >> 32));
}

static gboolean
id_equal (gconstpointer a,
          gconstpointer b)
{
	return * ((const guint64 *) a) == * ((const guint64 *) b);
}

static guint64 *
id_new (guint64 value)
{
	guint64 *id = g_new (guint64, 1);

	*id = value;
	return id;
}

static void
id_set_add (GHashTable *set,
            guint64 value)
{
	if (value == 0 || g_hash_table_contains (set, &value))
		return;

	g_hash_table_add (set, id_new (value));
}

static void
conv_msg_free (ConvMsg *msg)
{
	if (!msg)
		return;

	g_free (msg->folder_name);
	g_free (msg->uid);
	g_clear_pointer (&msg->references, g_array_unref);
	g_free (msg);
}

static void
by_id_remove_msg (StoreIndex *index,
                  guint64 id,
                  ConvMsg *msg)
{
	guint64 key = id;
	GPtrArray *list;
	guint ii;

	if (id == 0)
		return;

	list = g_hash_table_lookup (index->by_id, &key);
	if (list == NULL)
		return;

	for (ii = 0; ii < list->len; ii++) {
		if (list->pdata[ii] == msg) {
			g_ptr_array_remove_index_fast (list, ii);
			break;
		}
	}

	if (list->len == 0)
		g_hash_table_remove (index->by_id, &key);
}

static void
by_id_add_msg (StoreIndex *index,
               guint64 id,
               ConvMsg *msg)
{
	guint64 key = id;
	GPtrArray *list;

	if (id == 0)
		return;

	list = g_hash_table_lookup (index->by_id, &key);
	if (list == NULL) {
		list = g_ptr_array_new ();
		g_hash_table_insert (index->by_id, id_new (id), list);
	}

	g_ptr_array_add (list, msg);
}

static void
folder_slice_free (FolderSlice *slice,
                   StoreIndex *index)
{
	guint ii;

	if (!slice)
		return;

	for (ii = 0; slice->messages && ii < slice->messages->len; ii++) {
		ConvMsg *msg = slice->messages->pdata[ii];
		guint jj;

		by_id_remove_msg (index, msg->message_id, msg);
		if (msg->references) {
			for (jj = 0; jj < msg->references->len; jj++)
				by_id_remove_msg (index, g_array_index (msg->references, guint64, jj), msg);
		}
		conv_msg_free (msg);
	}

	g_clear_pointer (&slice->messages, g_ptr_array_unref);
	g_free (slice->folder_name);
	g_free (slice);
}

static void
store_index_free (StoreIndex *index)
{
	if (!index)
		return;

	g_mutex_lock (&index->lock);
	if (index->folders) {
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init (&iter, index->folders);
		while (g_hash_table_iter_next (&iter, NULL, &value))
			folder_slice_free (value, index);
		g_hash_table_destroy (index->folders);
		index->folders = NULL;
	}
	g_hash_table_destroy (index->by_id);
	g_clear_pointer (&index->dirty, g_hash_table_destroy);
	g_clear_pointer (&index->saved_counts, g_hash_table_destroy);
	g_mutex_unlock (&index->lock);
	g_mutex_clear (&index->lock);
	g_free (index);
}

static StoreIndex *
store_index_get (CamelStore *store)
{
	StoreIndex *index;

	g_mutex_lock (&index_create_lock);
	index = g_object_get_data (G_OBJECT (store), "evo-conversation-index");
	if (index == NULL) {
		index = g_new0 (StoreIndex, 1);
		g_mutex_init (&index->lock);
		index->folders = g_hash_table_new (g_str_hash, g_str_equal);
		index->by_id = g_hash_table_new_full (id_hash, id_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
		index->dirty = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		index->saved_counts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		g_object_set_data_full (
			G_OBJECT (store), "evo-conversation-index",
			index, (GDestroyNotify) store_index_free);
	}
	g_mutex_unlock (&index_create_lock);

	return index;
}

static gboolean
decode_part (const gchar *db_part,
             guint64 *out_message_id,
             GArray **out_references)
{
	CamelSummaryMessageID message_id;
	gchar *part = (gchar *) db_part;
	gint count, ii;

	*out_message_id = 0;
	*out_references = NULL;

	if (db_part == NULL || db_part[0] == '\0')
		return FALSE;

	message_id.id.part.hi = camel_util_bdata_get_number (&part, 0);
	message_id.id.part.lo = camel_util_bdata_get_number (&part, 0);
	*out_message_id = message_id.id.id;

	count = camel_util_bdata_get_number (&part, 0);
	if (count <= 0 || count > 1024)
		return TRUE;

	*out_references = g_array_sized_new (FALSE, FALSE, sizeof (guint64), count);
	for (ii = 0; ii < count; ii++) {
		guint64 id;

		message_id.id.part.hi = camel_util_bdata_get_number (&part, 0);
		message_id.id.part.lo = camel_util_bdata_get_number (&part, 0);
		id = message_id.id.id;
		if (id != 0)
			g_array_append_val (*out_references, id);
	}

	return TRUE;
}

static gboolean
read_message_cb (CamelStoreDB *storedb,
                 const CamelStoreDBMessageRecord *record,
                 gpointer user_data)
{
	GPtrArray *messages = user_data;
	ConvMsg *msg;
	guint64 message_id = 0;
	GArray *references = NULL;

	if (record->uid == NULL)
		return TRUE;

	decode_part (record->part, &message_id, &references);
	if (message_id == 0 && (references == NULL || references->len == 0)) {
		g_clear_pointer (&references, g_array_unref);
		return TRUE;
	}

	msg = g_new0 (ConvMsg, 1);
	msg->uid = g_strdup (record->uid);
	msg->flags = record->flags;
	msg->message_id = message_id;
	msg->references = references;
	g_ptr_array_add (messages, msg);

	return TRUE;
}

static FolderSlice *
folder_slice_load (EMailSession *session,
                   CamelStore *store,
                   const gchar *folder_name,
                   GCancellable *cancellable,
                   GError **error)
{
	CamelStoreDB *db;
	CamelFolder *open_folder;
	FolderSlice *slice;
	GPtrArray *messages;
	GError *local_error = NULL;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return NULL;

	/* New messages of an open folder reach the database only when its
	 * summary is saved. */
	open_folder = mail_folder_cache_ref_folder (
		e_mail_session_get_folder_cache (session), store, folder_name);
	if (open_folder != NULL) {
		camel_folder_summary_save (camel_folder_get_folder_summary (open_folder), NULL);
		g_object_unref (open_folder);
	}

	db = camel_store_get_db (store);
	if (db == NULL)
		return NULL;

	messages = g_ptr_array_new ();
	if (!camel_store_db_read_messages (db, folder_name, read_message_cb, messages, &local_error)) {
		g_ptr_array_set_free_func (messages, (GDestroyNotify) conv_msg_free);
		g_ptr_array_unref (messages);
		/* A folder that has never been opened has no summary yet. */
		g_clear_error (&local_error);
		messages = g_ptr_array_new ();
	}

	slice = g_new0 (FolderSlice, 1);
	slice->folder_name = g_strdup (folder_name);
	slice->messages = messages;

	return slice;
}

static void
folder_slice_install (StoreIndex *index,
                      FolderSlice *slice)
{
	guint ii;

	g_hash_table_insert (index->folders, slice->folder_name, slice);

	for (ii = 0; ii < slice->messages->len; ii++) {
		ConvMsg *msg = slice->messages->pdata[ii];
		guint jj;

		msg->folder_name = g_strdup (slice->folder_name);
		by_id_add_msg (index, msg->message_id, msg);
		if (msg->references == NULL)
			continue;
		for (jj = 0; jj < msg->references->len; jj++)
			by_id_add_msg (index, g_array_index (msg->references, guint64, jj), msg);
	}
}

static gboolean
folder_type_skipped (guint32 flags)
{
	guint32 type = flags & CAMEL_FOLDER_TYPE_MASK;

	if ((flags & CAMEL_FOLDER_NOSELECT) != 0)
		return TRUE;

	switch (type) {
		case CAMEL_FOLDER_TYPE_TRASH:
		case CAMEL_FOLDER_TYPE_JUNK:
		case CAMEL_FOLDER_TYPE_CONTACTS:
		case CAMEL_FOLDER_TYPE_EVENTS:
		case CAMEL_FOLDER_TYPE_MEMOS:
		case CAMEL_FOLDER_TYPE_TASKS:
			return TRUE;
		default:
			return FALSE;
	}
}

static gboolean
collect_uri_cb (const gchar *uri,
                gpointer user_data)
{
	g_ptr_array_add (user_data, g_strdup (uri));

	return TRUE;
}

static gboolean
folder_flags (EMailSession *session,
              CamelStore *store,
              const gchar *folder_name,
              guint32 *out_flags)
{
	CamelFolderInfoFlags flags = 0;

	if (!mail_folder_cache_get_folder_info_flags (
		e_mail_session_get_folder_cache (session),
		store, folder_name, &flags))
		return FALSE;

	*out_flags = flags;

	return TRUE;
}

/* Folder names of @store known to the folder tree. Read from memory:
 * camel_store_get_folder_info_sync() asks the server on EWS and IMAP
 * and holds the store's folder lock for the whole request. */
static gboolean
collect_folder_names (EMailSession *session,
                      CamelStore *store,
                      GPtrArray *names,
                      GHashTable *skipped)
{
	MailFolderCache *cache;
	GPtrArray *uris;
	guint ii;

	cache = e_mail_session_get_folder_cache (session);
	uris = g_ptr_array_new_with_free_func (g_free);
	mail_folder_cache_foreach_remote_folder_uri (cache, collect_uri_cb, uris);
	mail_folder_cache_foreach_local_folder_uri (cache, collect_uri_cb, uris);

	for (ii = 0; ii < uris->len; ii++) {
		CamelStore *uri_store = NULL;
		gchar *folder_name = NULL;
		guint32 flags = 0;

		if (!e_mail_folder_uri_parse (CAMEL_SESSION (session), uris->pdata[ii], &uri_store, &folder_name, NULL))
			continue;

		if (uri_store == store && folder_name != NULL) {
			folder_flags (session, store, folder_name, &flags);
			if (folder_type_skipped (flags))
				g_hash_table_add (skipped, g_steal_pointer (&folder_name));
			else
				g_ptr_array_add (names, g_steal_pointer (&folder_name));
		}

		g_clear_object (&uri_store);
		g_free (folder_name);
	}

	g_ptr_array_unref (uris);

	return names->len > 0;
}

static void
ensure_folders (StoreIndex *index,
                EMailSession *session,
                CamelStore *store,
                const gchar *only_folder_name,
                GCancellable *cancellable)
{
	GPtrArray *needed = NULL;
	GHashTable *skipped = NULL;
	gboolean listed = FALSE;
	gboolean complete = TRUE;
	guint ii;

	g_mutex_lock (&index->lock);
	if (only_folder_name != NULL) {
		if (g_hash_table_contains (index->folders, only_folder_name)) {
			g_mutex_unlock (&index->lock);
			return;
		}
		needed = g_ptr_array_new_with_free_func (g_free);
		g_ptr_array_add (needed, g_strdup (only_folder_name));
	} else if (!index->scanned) {
		needed = g_ptr_array_new_with_free_func (g_free);
		skipped = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	} else if (index->dirty != NULL && g_hash_table_size (index->dirty) > 0) {
		GHashTableIter iter;
		gpointer key;

		needed = g_ptr_array_new_with_free_func (g_free);
		g_hash_table_iter_init (&iter, index->dirty);
		while (g_hash_table_iter_next (&iter, &key, NULL))
			g_ptr_array_add (needed, g_strdup (key));
		g_hash_table_remove_all (index->dirty);
	} else {
		g_mutex_unlock (&index->lock);
		return;
	}
	g_mutex_unlock (&index->lock);

	if (skipped != NULL)
		listed = collect_folder_names (session, store, needed, skipped);

	for (ii = 0; needed != NULL && ii < needed->len; ii++) {
		const gchar *folder_name = needed->pdata[ii];
		FolderSlice *slice;
		guint32 flags = 0;
		gboolean have;

		if (g_cancellable_is_cancelled (cancellable)) {
			complete = FALSE;
			break;
		}

		g_mutex_lock (&index->lock);
		have = g_hash_table_contains (index->folders, folder_name);
		g_mutex_unlock (&index->lock);
		if (have)
			continue;

		if (folder_flags (session, store, folder_name, &flags) &&
		    folder_type_skipped (flags)) {
			slice = g_new0 (FolderSlice, 1);
			slice->folder_name = g_strdup (folder_name);
			slice->messages = g_ptr_array_new ();
		} else {
			slice = folder_slice_load (session, store, folder_name, cancellable, NULL);
		}
		if (slice == NULL) {
			complete = FALSE;
			continue;
		}

		g_mutex_lock (&index->lock);
		if (g_hash_table_contains (index->folders, folder_name))
			folder_slice_free (slice, index);
		else
			folder_slice_install (index, slice);
		g_mutex_unlock (&index->lock);
	}

	/* Remember skipped folders so a later invalidate can drop them. */
	if (skipped != NULL) {
		GHashTableIter iter;
		gpointer key;

		g_mutex_lock (&index->lock);
		g_hash_table_iter_init (&iter, skipped);
		while (g_hash_table_iter_next (&iter, &key, NULL)) {
			if (!g_hash_table_contains (index->folders, key)) {
				FolderSlice *empty = g_new0 (FolderSlice, 1);

				empty->folder_name = g_strdup (key);
				empty->messages = g_ptr_array_new ();
				g_hash_table_insert (index->folders, empty->folder_name, empty);
			}
		}
		/* A cancelled or empty pass must not mark the index as
		 * built, or it stays empty for the whole session. */
		if (listed && complete)
			index->scanned = TRUE;
		g_mutex_unlock (&index->lock);
		g_hash_table_destroy (skipped);
	} else if (!complete && only_folder_name == NULL) {
		g_mutex_lock (&index->lock);
		for (ii = 0; ii < needed->len; ii++) {
			if (!g_hash_table_contains (index->folders, needed->pdata[ii]))
				g_hash_table_add (index->dirty, g_strdup (needed->pdata[ii]));
		}
		g_mutex_unlock (&index->lock);
	}

	g_clear_pointer (&needed, g_ptr_array_unref);
}

static gboolean
message_hidden (const ConvMsg *msg,
                gboolean hide_deleted,
                gboolean hide_junk)
{
	if (hide_deleted && (msg->flags & CAMEL_MESSAGE_DELETED) != 0)
		return TRUE;
	if (hide_junk && (msg->flags & CAMEL_MESSAGE_JUNK) != 0)
		return TRUE;
	return FALSE;
}

static void
expand_message_ids (GHashTable *into,
                    const ConvMsg *msg)
{
	guint ii;

	id_set_add (into, msg->message_id);
	if (msg->references == NULL)
		return;
	for (ii = 0; ii < msg->references->len; ii++)
		id_set_add (into, g_array_index (msg->references, guint64, ii));
}

static void
query_store (StoreIndex *index,
             CamelStore *store,
             const gchar *skip_folder_name,
             GHashTable *seeds,
             GHashTable *known_message_ids,
             gboolean hide_deleted,
             gboolean hide_junk,
             GPtrArray *hits)
{
	GHashTable *frontier;
	GHashTable *seen_msgs;
	gint pass;

	frontier = g_hash_table_new_full (id_hash, id_equal, g_free, NULL);
	seen_msgs = g_hash_table_new (g_direct_hash, g_direct_equal);

	{
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init (&iter, seeds);
		while (g_hash_table_iter_next (&iter, &key, NULL))
			id_set_add (frontier, * ((guint64 *) key));
	}

	g_mutex_lock (&index->lock);

	for (pass = 0; pass < CONVERSATION_INDEX_MAX_PASSES && g_hash_table_size (frontier) > 0; pass++) {
		GHashTable *next = g_hash_table_new_full (id_hash, id_equal, g_free, NULL);
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init (&iter, frontier);
		while (g_hash_table_iter_next (&iter, &key, NULL)) {
			GPtrArray *list;
			guint jj;

			list = g_hash_table_lookup (index->by_id, key);
			for (jj = 0; list != NULL && jj < list->len; jj++) {
				ConvMsg *msg = list->pdata[jj];
				gboolean hidden;
				gboolean duplicate;

				if (g_hash_table_contains (seen_msgs, msg))
					continue;
				g_hash_table_add (seen_msgs, msg);

				if (skip_folder_name != NULL &&
				    g_strcmp0 (msg->folder_name, skip_folder_name) == 0)
					continue;

				hidden = message_hidden (msg, hide_deleted, hide_junk);
				duplicate = msg->message_id != 0 &&
					known_message_ids != NULL &&
					g_hash_table_contains (known_message_ids, &msg->message_id);

				/* Hidden and duplicate messages still extend the
				 * conversation, so replies that only point at them
				 * are found on the next pass. */
				expand_message_ids (next, msg);
				expand_message_ids (seeds, msg);

				if (hidden || duplicate || msg->uid == NULL)
					continue;

				if (msg->message_id != 0)
					id_set_add (known_message_ids, msg->message_id);

				{
					EMailConversationHit *hit = g_new0 (EMailConversationHit, 1);

					hit->store = g_object_ref (store);
					hit->folder_name = g_strdup (msg->folder_name);
					hit->uid = g_strdup (msg->uid);
					g_ptr_array_add (hits, hit);
				}
			}
		}

		g_hash_table_destroy (frontier);
		frontier = next;
	}

	g_mutex_unlock (&index->lock);

	g_hash_table_destroy (frontier);
	g_hash_table_destroy (seen_msgs);
}

static gboolean
account_sent_folder (EMailSession *session,
                     CamelStore *store,
                     CamelStore **out_store,
                     gchar **out_folder_name)
{
	ESourceRegistry *registry;
	ESource *source;
	ESource *identity = NULL;
	const gchar *service_uid;
	const gchar *identity_uid;
	const gchar *sent_uri;
	gboolean found = FALSE;

	*out_store = NULL;
	*out_folder_name = NULL;

	registry = e_mail_session_get_registry (session);
	service_uid = camel_service_get_uid (CAMEL_SERVICE (store));
	source = e_source_registry_ref_source (registry, service_uid);
	if (source == NULL || !e_source_has_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT)) {
		g_clear_object (&source);
		return FALSE;
	}

	identity_uid = e_source_mail_account_get_identity_uid (
		e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT));
	if (identity_uid != NULL)
		identity = e_source_registry_ref_source (registry, identity_uid);
	g_clear_object (&source);

	if (identity == NULL || !e_source_has_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION)) {
		g_clear_object (&identity);
		return FALSE;
	}

	if (e_source_mail_submission_get_use_sent_folder (
		e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION))) {
		sent_uri = e_source_mail_submission_get_sent_folder (
			e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION));
		if (sent_uri != NULL &&
		    e_mail_folder_uri_parse (CAMEL_SESSION (session), sent_uri, out_store, out_folder_name, NULL))
			found = TRUE;
	}

	g_clear_object (&identity);

	return found;
}

void
e_mail_conversation_hit_free (EMailConversationHit *hit)
{
	if (hit == NULL)
		return;

	g_clear_object (&hit->store);
	g_free (hit->folder_name);
	g_free (hit->uid);
	g_free (hit);
}

GPtrArray *
e_mail_conversation_index_find_related (EMailSession *session,
                                        CamelFolder *folder,
                                        GHashTable *seed_ids,
                                        GHashTable *known_message_ids,
                                        gboolean hide_deleted,
                                        gboolean hide_junk,
                                        GCancellable *cancellable,
                                        GError **error)
{
	CamelStore *store;
	CamelStore *sent_store = NULL;
	gchar *sent_folder_name = NULL;
	StoreIndex *index;
	GPtrArray *hits;

	g_return_val_if_fail (E_IS_MAIL_SESSION (session), NULL);
	g_return_val_if_fail (CAMEL_IS_FOLDER (folder), NULL);
	g_return_val_if_fail (seed_ids != NULL, NULL);
	g_return_val_if_fail (known_message_ids != NULL, NULL);

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return NULL;

	store = camel_folder_get_parent_store (folder);
	index = store_index_get (store);
	ensure_folders (index, session, store, NULL, cancellable);

	hits = g_ptr_array_new_with_free_func ((GDestroyNotify) e_mail_conversation_hit_free);
	query_store (
		index, store, camel_folder_get_full_name (folder),
		seed_ids, known_message_ids, hide_deleted, hide_junk, hits);

	if (account_sent_folder (session, store, &sent_store, &sent_folder_name) &&
	    sent_store != store) {
		StoreIndex *sent_index = store_index_get (sent_store);

		ensure_folders (sent_index, session, sent_store, sent_folder_name, cancellable);
		query_store (
			sent_index, sent_store, NULL,
			seed_ids, known_message_ids, hide_deleted, hide_junk, hits);
	}

	g_clear_object (&sent_store);
	g_free (sent_folder_name);

	return hits;
}

void
e_mail_conversation_index_invalidate_folder (CamelStore *store,
                                             const gchar *folder_name)
{
	StoreIndex *index;
	FolderSlice *slice;

	g_return_if_fail (CAMEL_IS_STORE (store));

	index = g_object_get_data (G_OBJECT (store), "evo-conversation-index");
	if (index == NULL)
		return;

	g_mutex_lock (&index->lock);
	if (folder_name != NULL) {
		slice = g_hash_table_lookup (index->folders, folder_name);
		if (slice != NULL) {
			g_hash_table_steal (index->folders, folder_name);
			folder_slice_free (slice, index);
		}
		if (index->dirty != NULL)
			g_hash_table_add (index->dirty, g_strdup (folder_name));
	} else {
		index->scanned = FALSE;
	}
	g_mutex_unlock (&index->lock);
}

/* TRUE only when this folder's saved message count differs from the
 * count already seen. The first observation is remembered and does
 * not count: opening a folder to read its summary also emits
 * folder-changed, and treating that as new mail rebuilds the list
 * forever. */
static gboolean
saved_count_changed (CamelStore *store,
                     const gchar *folder_name,
                     guint32 saved_count)
{
	StoreIndex *index;
	gpointer old = NULL;
	gboolean known;
	gboolean changed;

	index = store_index_get (store);

	g_mutex_lock (&index->lock);
	if (index->saved_counts == NULL)
		index->saved_counts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	known = g_hash_table_lookup_extended (index->saved_counts, folder_name, NULL, &old);
	changed = known && GPOINTER_TO_UINT (old) != saved_count;
	g_hash_table_insert (index->saved_counts, g_strdup (folder_name), GUINT_TO_POINTER (saved_count));
	g_mutex_unlock (&index->lock);

	return changed;
}

/* The open folder's summary is current; the saved record lags until
 * the summary is written back. */
static gboolean
folder_message_count (EMailSession *session,
                      CamelStore *store,
                      const gchar *folder_name,
                      guint32 *out_count)
{
	CamelFolder *folder;
	CamelStoreDB *db;
	CamelStoreDBFolderRecord record;
	gboolean found = FALSE;

	folder = mail_folder_cache_ref_folder (
		e_mail_session_get_folder_cache (session), store, folder_name);
	if (folder != NULL) {
		*out_count = camel_folder_summary_count (camel_folder_get_folder_summary (folder));
		g_object_unref (folder);
		return TRUE;
	}

	memset (&record, 0, sizeof (record));
	db = camel_store_get_db (store);
	if (db != NULL && camel_store_db_read_folder (db, folder_name, &record, NULL)) {
		*out_count = record.saved_count;
		found = TRUE;
		camel_store_db_folder_record_clear (&record);
	}

	return found;
}

gboolean
e_mail_conversation_index_change_affects_folder (EMailSession *session,
                                                 CamelFolder *view_folder,
                                                 CamelStore *changed_store,
                                                 const gchar *changed_folder_name,
                                                 gint new_messages)
{
	CamelStore *view_store;
	CamelStore *sent_store = NULL;
	gchar *sent_folder_name = NULL;
	guint32 count = 0;
	guint32 type = 0;
	gboolean watched = FALSE;
	gboolean affects = FALSE;

	g_return_val_if_fail (E_IS_MAIL_SESSION (session), FALSE);
	g_return_val_if_fail (CAMEL_IS_STORE (changed_store), FALSE);

	if (view_folder == NULL || changed_folder_name == NULL)
		return FALSE;
	if (CAMEL_IS_VEE_FOLDER (view_folder))
		return FALSE;

	view_store = camel_folder_get_parent_store (view_folder);
	if (changed_store == view_store &&
	    g_strcmp0 (changed_folder_name, camel_folder_get_full_name (view_folder)) == 0)
		return FALSE;

	if (new_messages > 0 && changed_store == view_store) {
		if (folder_message_count (session, changed_store, changed_folder_name, &count))
			saved_count_changed (changed_store, changed_folder_name, count);
		return TRUE;
	}

	if (changed_store == view_store &&
	    folder_flags (session, changed_store, changed_folder_name, &type)) {
		type &= CAMEL_FOLDER_TYPE_MASK;
		watched = type == CAMEL_FOLDER_TYPE_SENT ||
			type == CAMEL_FOLDER_TYPE_DRAFTS ||
			type == CAMEL_FOLDER_TYPE_OUTBOX;
	}

	if (!watched &&
	    account_sent_folder (session, view_store, &sent_store, &sent_folder_name) &&
	    sent_store == changed_store &&
	    g_strcmp0 (sent_folder_name, changed_folder_name) == 0)
		watched = TRUE;

	if (watched &&
	    folder_message_count (session, changed_store, changed_folder_name, &count))
		affects = saved_count_changed (changed_store, changed_folder_name, count);

	g_clear_object (&sent_store);
	g_free (sent_folder_name);

	return affects;
}
