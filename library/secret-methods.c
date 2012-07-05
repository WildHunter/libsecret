/* libsecret - GLib wrapper for Secret Service
 *
 * Copyright 2011 Collabora Ltd.
 * Copyright 2012 Red Hat Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Author: Stef Walter <stefw@gnome.org>
 */

#include "config.h"

#include "secret-collection.h"
#include "secret-dbus-generated.h"
#include "secret-item.h"
#include "secret-paths.h"
#include "secret-private.h"
#include "secret-service.h"
#include "secret-types.h"
#include "secret-value.h"

typedef struct {
	GCancellable *cancellable;
	GHashTable *items;
	gchar **unlocked;
	gchar **locked;
	guint loading;
} SearchClosure;

static void
search_closure_free (gpointer data)
{
	SearchClosure *closure = data;
	g_clear_object (&closure->cancellable);
	g_hash_table_unref (closure->items);
	g_strfreev (closure->unlocked);
	g_strfreev (closure->locked);
	g_slice_free (SearchClosure, closure);
}

static void
search_closure_take_item (SearchClosure *closure,
                          SecretItem *item)
{
	const gchar *path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (item));
	g_hash_table_insert (closure->items, (gpointer)path, item);
}

static void
on_search_loaded (GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	SearchClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	GError *error = NULL;
	SecretItem *item;

	closure->loading--;

	item = secret_item_new_finish (result, &error);
	if (error != NULL)
		g_simple_async_result_take_error (res, error);

	if (item != NULL)
		search_closure_take_item (closure, item);
	if (closure->loading == 0)
		g_simple_async_result_complete (res);

	g_object_unref (res);
}

static void
search_load_item_async (SecretService *self,
                        GSimpleAsyncResult *res,
                        SearchClosure *closure,
                        const gchar *path)
{
	SecretItem *item;

	item = _secret_service_find_item_instance (self, path);
	if (item == NULL) {
		secret_item_new (self, path, SECRET_ITEM_NONE, closure->cancellable,
		                  on_search_loaded, g_object_ref (res));
		closure->loading++;
	} else {
		search_closure_take_item (closure, item);
	}
}

static void
on_search_paths (GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	SearchClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	SecretService *self = SECRET_SERVICE (source);
	GError *error = NULL;
	guint i;

	if (!secret_service_search_for_paths_finish (self, result, &closure->unlocked,
	                                              &closure->locked, &error)) {
		g_simple_async_result_take_error (res, error);
		g_simple_async_result_complete (res);
	}

	for (i = 0; closure->unlocked[i] != NULL; i++)
		search_load_item_async (self, res, closure, closure->unlocked[i]);
	for (i = 0; closure->locked[i] != NULL; i++)
		search_load_item_async (self, res, closure, closure->locked[i]);

	if (closure->loading == 0)
		g_simple_async_result_complete (res);

	g_object_unref (res);
}

/**
 * secret_service_search:
 * @self: the secret service
 * @attributes: (element-type utf8 utf8): search for items matching these attributes
 * @cancellable: optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to pass to the callback
 *
 * Search for items matching the @attributes. All collections are searched.
 * The @attributes should be a table of string keys and string values.
 *
 * This function returns immediately and completes asynchronously.
 *
 * When your callback is called use secret_service_search_finish()
 * to get the results of this function. #SecretItem proxy objects will be
 * returned. If you prefer to only have the items D-Bus object paths returned,
 * then then use the secret_service_search_for_paths() function.
 */
void
secret_service_search (SecretService *self,
                       GHashTable *attributes,
                       GCancellable *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer user_data)
{
	GSimpleAsyncResult *res;
	SearchClosure *closure;

	g_return_if_fail (SECRET_IS_SERVICE (self));
	g_return_if_fail (attributes != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	res = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
	                                 secret_service_search);
	closure = g_slice_new0 (SearchClosure);
	closure->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
	closure->items = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
	g_simple_async_result_set_op_res_gpointer (res, closure, search_closure_free);

	secret_service_search_for_paths (self, attributes, cancellable,
	                                 on_search_paths, g_object_ref (res));

	g_object_unref (res);
}

static GList *
search_finish_build (gchar **paths,
                     SearchClosure *closure)
{
	GList *results = NULL;
	SecretItem *item;
	guint i;

	for (i = 0; paths[i]; i++) {
		item = g_hash_table_lookup (closure->items, paths[i]);
		if (item != NULL)
			results = g_list_prepend (results, g_object_ref (item));
	}

	return g_list_reverse (results);
}

/**
 * secret_service_search_finish:
 * @self: the secret service
 * @result: asynchronous result passed to callback
 * @unlocked: (out) (transfer full) (element-type Secret.Item) (allow-none):
 *            location to place a list of matching items which were not locked.
 * @locked: (out) (transfer full) (element-type Secret.Item) (allow-none):
 *          location to place a list of matching items which were locked.
 * @error: location to place error on failure
 *
 * Complete asynchronous operation to search for items.
 *
 * Matching items that are locked or unlocked are placed in the @locked or
 * @unlocked lists respectively.
 *
 * #SecretItem proxy objects will be returned. If you prefer to only have
 * the items' D-Bus object paths returned, then then use the
 * secret_service_search_for_paths() function.
 *
 * Returns: whether the search was successful or not
 */
gboolean
secret_service_search_finish (SecretService *self,
                              GAsyncResult *result,
                              GList **unlocked,
                              GList **locked,
                              GError **error)
{
	GSimpleAsyncResult *res;
	SearchClosure *closure;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
	                      secret_service_search), FALSE);

	res = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (res, error))
		return FALSE;

	closure = g_simple_async_result_get_op_res_gpointer (res);
	if (unlocked)
		*unlocked = search_finish_build (closure->unlocked, closure);
	if (locked)
		*locked = search_finish_build (closure->locked, closure);

	return TRUE;
}

static gboolean
service_load_items_sync (SecretService *self,
                         GCancellable *cancellable,
                         gchar **paths,
                         GList **items,
                         GError **error)
{
	SecretItem *item;
	GList *result = NULL;
	guint i;

	for (i = 0; paths[i] != NULL; i++) {
		item = _secret_service_find_item_instance (self, paths[i]);
		if (item == NULL)
			item = secret_item_new_sync (self, paths[i], SECRET_ITEM_NONE,
			                             cancellable, error);
		if (item == NULL) {
			g_list_free_full (result, g_object_unref);
			return FALSE;
		} else {
			result = g_list_prepend (result, item);
		}
	}

	*items = g_list_reverse (result);
	return TRUE;
}

/**
 * secret_service_search_sync:
 * @self: the secret service
 * @attributes: (element-type utf8 utf8): search for items matching these attributes
 * @cancellable: optional cancellation object
 * @unlocked: (out) (transfer full) (element-type Secret.Item) (allow-none):
 *            location to place a list of matching items which were not locked.
 * @locked: (out) (transfer full) (element-type Secret.Item) (allow-none):
 *          location to place a list of matching items which were locked.
 * @error: location to place error on failure
 *
 * Search for items matching the @attributes. All collections are searched.
 * The @attributes should be a table of string keys and string values.
 *
 * This function may block indefinetely. Use the asynchronous version
 * in user interface threads.
 *
 * Matching items that are locked or unlocked are placed
 * in the @locked or @unlocked lists respectively.
 *
 * #SecretItem proxy objects will be returned. If you prefer to only have
 * the items' D-Bus object paths returned, then then use the
 * secret_service_search_sync() function.
 *
 * Returns: whether the search was successful or not
 */
gboolean
secret_service_search_sync (SecretService *self,
                            GHashTable *attributes,
                            GCancellable *cancellable,
                            GList **unlocked,
                            GList **locked,
                            GError **error)
{
	gchar **unlocked_paths = NULL;
	gchar **locked_paths = NULL;
	gboolean ret;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	if (!secret_service_search_for_paths_sync (self, attributes, cancellable,
	                                           unlocked ? &unlocked_paths : NULL,
	                                           locked ? &locked_paths : NULL, error))
		return FALSE;

	ret = TRUE;

	if (unlocked)
		ret = service_load_items_sync (self, cancellable, unlocked_paths, unlocked, error);
	if (ret && locked)
		ret = service_load_items_sync (self, cancellable, locked_paths, locked, error);

	g_strfreev (unlocked_paths);
	g_strfreev (locked_paths);

	return ret;
}

typedef struct {
	GCancellable *cancellable;
	GVariant *in;
	GVariant *out;
	GHashTable *items;
} GetClosure;

static void
get_closure_free (gpointer data)
{
	GetClosure *closure = data;
	if (closure->in)
		g_variant_unref (closure->in);
	if (closure->out)
		g_variant_unref (closure->out);
	g_clear_object (&closure->cancellable);
	g_slice_free (GetClosure, closure);
}

static void
on_get_secrets_complete (GObject *source,
                         GAsyncResult *result,
                         gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	GetClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	GError *error = NULL;

	closure->out = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
	if (error != NULL)
		g_simple_async_result_take_error (res, error);
	g_simple_async_result_complete (res);

	g_object_unref (res);
}

static void
on_get_secrets_session (GObject *source,
                        GAsyncResult *result,
                        gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	GetClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	GError *error = NULL;
	const gchar *session;

	session = secret_service_ensure_session_finish (SECRET_SERVICE (source),
	                                                result, &error);
	if (error != NULL) {
		g_simple_async_result_take_error (res, error);
		g_simple_async_result_complete (res);
	} else {
		g_dbus_proxy_call (G_DBUS_PROXY (source), "GetSecrets",
		                   g_variant_new ("(@aoo)", closure->in, session),
		                   G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
		                   closure->cancellable, on_get_secrets_complete,
		                   g_object_ref (res));
	}

	g_object_unref (res);
}


SecretValue *
_secret_service_decode_get_secrets_first (SecretService *self,
                                          GVariant *out)
{
	SecretSession *session;
	SecretValue *value = NULL;
	GVariantIter *iter;
	GVariant *variant;
	const gchar *path;

	g_variant_get (out, "(a{o(oayays)})", &iter);
	while (g_variant_iter_next (iter, "{&o@(oayays)}", &path, &variant)) {
		session = _secret_service_get_session (self);
		value = _secret_session_decode_secret (session, variant);
		g_variant_unref (variant);
		break;
	}
	g_variant_iter_free (iter);
	return value;
}

GHashTable *
_secret_service_decode_get_secrets_all (SecretService *self,
                                        GVariant *out)
{
	SecretSession *session;
	GVariantIter *iter;
	GVariant *variant;
	GHashTable *values;
	SecretValue *value;
	gchar *path;

	session = _secret_service_get_session (self);
	values = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                g_free, secret_value_unref);
	g_variant_get (out, "(a{o(oayays)})", &iter);
	while (g_variant_iter_loop (iter, "{o@(oayays)}", &path, &variant)) {
		value = _secret_session_decode_secret (session, variant);
		if (value && path)
			g_hash_table_insert (values, g_strdup (path), value);
	}
	g_variant_iter_free (iter);
	return values;
}

/**
 * secret_service_get_secrets:
 * @self: the secret service
 * @items: (element-type Secret.Item): the items to retrieve secrets for
 * @cancellable: optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to pass to the callback
 *
 * Get the secret values for an secret items stored in the service.
 *
 * This method takes a list of #SecretItem proxy objects. If you only have the
 * D-Bus object paths of the items, use secret_service_get_secrets_for_paths()
 * instead.
 *
 * This function returns immediately and completes asynchronously.
 */
void
secret_service_get_secrets (SecretService *self,
                            GList *items,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
	GSimpleAsyncResult *res;
	GetClosure *closure;
	GPtrArray *paths;
	const gchar *path;
	GList *l;

	g_return_if_fail (SECRET_IS_SERVICE (self));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	res = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
	                                 secret_service_get_secrets);
	closure = g_slice_new0 (GetClosure);
	closure->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
	closure->items = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                        g_free, g_object_unref);

	paths = g_ptr_array_new ();
	for (l = items; l != NULL; l = g_list_next (l)) {
		path = g_dbus_proxy_get_object_path (l->data);
		g_hash_table_insert (closure->items, g_strdup (path), g_object_ref (l->data));
		g_ptr_array_add (paths, (gpointer)path);
	}

	closure->in = g_variant_new_objv ((const gchar * const *)paths->pdata, paths->len);
	g_variant_ref_sink (closure->in);

	g_ptr_array_free (paths, TRUE);
	g_simple_async_result_set_op_res_gpointer (res, closure, get_closure_free);

	secret_service_ensure_session (self, cancellable,
	                                on_get_secrets_session,
	                                g_object_ref (res));

	g_object_unref (res);
}

/**
 * secret_service_get_secrets_finish:
 * @self: the secret service
 * @result: asynchronous result passed to callback
 * @error: location to place an error on failure
 *
 * Complete asynchronous operation to get the secret values for an
 * secret items stored in the service.
 *
 * Items that are locked will not be included the results.
 *
 * Returns: (transfer full): a newly allocated hash table of #SecretItem keys
 *          to #SecretValue values.
 */
GHashTable *
secret_service_get_secrets_finish (SecretService *self,
                                   GAsyncResult *result,
                                   GError **error)
{
	GSimpleAsyncResult *res;
	GetClosure *closure;
	GHashTable *with_paths;
	GHashTable *with_items;
	GHashTableIter iter;
	const gchar *path;
	SecretValue *value;
	SecretItem *item;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), NULL);
	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
	                      secret_service_get_secrets), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	res = G_SIMPLE_ASYNC_RESULT (result);
	if (g_simple_async_result_propagate_error (res, error))
		return NULL;

	closure = g_simple_async_result_get_op_res_gpointer (res);
	with_paths = _secret_service_decode_get_secrets_all (self, closure->out);
	g_return_val_if_fail (with_paths != NULL, NULL);

	with_items = g_hash_table_new_full (g_direct_hash, g_direct_equal,
	                                    g_object_unref, secret_value_unref);

	g_hash_table_iter_init (&iter, with_paths);
	while (g_hash_table_iter_next (&iter, (gpointer *)&path, (gpointer *)&value)) {
		item = g_hash_table_lookup (closure->items, path);
		if (item != NULL)
			g_hash_table_insert (with_items, g_object_ref (item),
			                     secret_value_ref (value));
	}

	g_hash_table_unref (with_paths);
	return with_items;
}

/**
 * secret_service_get_secrets_sync:
 * @self: the secret service
 * @items: (element-type Secret.Item): the items to retrieve secrets for
 * @cancellable: optional cancellation object
 * @error: location to place an error on failure
 *
 * Get the secret values for an secret items stored in the service.
 *
 * This method takes a list of #SecretItem proxy objects. If you only have the
 * D-Bus object paths of the items, use secret_service_get_secrets_for_paths_sync()
 * instead.
 *
 * This method may block indefinitely and should not be used in user interface
 * threads.
 *
 * Items that are locked will not be included the results.
 *
 * Returns: (transfer full): a newly allocated hash table of #SecretItem keys
 *          to #SecretValue values.
 */
GHashTable *
secret_service_get_secrets_sync (SecretService *self,
                                 GList *items,
                                 GCancellable *cancellable,
                                 GError **error)
{
	SecretSync *sync;
	GHashTable *secrets;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	sync = _secret_sync_new ();
	g_main_context_push_thread_default (sync->context);

	secret_service_get_secrets (self, items, cancellable,
	                             _secret_sync_on_result, sync);

	g_main_loop_run (sync->loop);

	secrets = secret_service_get_secrets_finish (self, sync->result, error);

	g_main_context_pop_thread_default (sync->context);
	_secret_sync_free (sync);

	return secrets;
}

typedef struct {
	GCancellable *cancellable;
	SecretPrompt *prompt;
	GHashTable *objects;
	GPtrArray *xlocked;
} XlockClosure;

static void
xlock_closure_free (gpointer data)
{
	XlockClosure *closure = data;
	g_clear_object (&closure->cancellable);
	g_clear_object (&closure->prompt);
	if (closure->xlocked)
		g_ptr_array_unref (closure->xlocked);
	if (closure->objects)
		g_hash_table_unref (closure->objects);
	g_slice_free (XlockClosure, closure);
}

static void
on_xlock_prompted (GObject *source,
                   GAsyncResult *result,
                   gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	XlockClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	SecretService *self = SECRET_SERVICE (source);
	GError *error = NULL;
	GVariantIter iter;
	GVariant *retval;
	gchar *path;

	retval = secret_service_prompt_finish (self, result, G_VARIANT_TYPE ("ao"), &error);
	if (error != NULL)
		g_simple_async_result_take_error (res, error);

	if (retval != NULL) {
		g_variant_iter_init (&iter, retval);
		while (g_variant_iter_loop (&iter, "o", &path))
			g_ptr_array_add (closure->xlocked, g_strdup (path));
		g_variant_unref (retval);
	}

	g_simple_async_result_complete (res);
	g_object_unref (res);
}

static void
on_xlock_called (GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	XlockClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	SecretService *self = SECRET_SERVICE (g_async_result_get_source_object (user_data));
	const gchar *prompt = NULL;
	gchar **xlocked = NULL;
	GError *error = NULL;
	GVariant *retval;
	guint i;

	retval = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
	if (error != NULL) {
		g_simple_async_result_take_error (res, error);
		g_simple_async_result_complete (res);

	} else {
		g_variant_get (retval, "(^ao&o)", &xlocked, &prompt);

		if (_secret_util_empty_path (prompt)) {
			for (i = 0; xlocked[i]; i++)
				g_ptr_array_add (closure->xlocked, g_strdup (xlocked[i]));
			g_simple_async_result_complete (res);

		} else {
			closure->prompt = _secret_prompt_instance (self, prompt);
			secret_service_prompt (self, closure->prompt, closure->cancellable,
			                        on_xlock_prompted, g_object_ref (res));
		}

		g_strfreev (xlocked);
		g_variant_unref (retval);
	}

	g_object_unref (self);
	g_object_unref (res);
}

static GSimpleAsyncResult *
service_xlock_paths_async (SecretService *self,
                           const gchar *method,
                           const gchar **paths,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
	GSimpleAsyncResult *res;
	XlockClosure *closure;

	res = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
	                                 service_xlock_paths_async);
	closure = g_slice_new0 (XlockClosure);
	closure->cancellable = cancellable ? g_object_ref (cancellable) : cancellable;
	closure->xlocked = g_ptr_array_new_with_free_func (g_free);
	g_simple_async_result_set_op_res_gpointer (res, closure, xlock_closure_free);

	g_dbus_proxy_call (G_DBUS_PROXY (self), method,
	                   g_variant_new ("(@ao)", g_variant_new_objv (paths, -1)),
	                   G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
	                   cancellable, on_xlock_called, g_object_ref (res));

	return res;
}

static gint
service_xlock_paths_finish (SecretService *self,
                            GAsyncResult *result,
                            gchar ***xlocked,
                            GError **error)
{
	GSimpleAsyncResult *res;
	XlockClosure *closure;
	gint count;

	res = G_SIMPLE_ASYNC_RESULT (result);
	if (g_simple_async_result_propagate_error (res, error))
		return -1;

	closure = g_simple_async_result_get_op_res_gpointer (res);
	count = closure->xlocked->len;

	if (xlocked != NULL) {
		g_ptr_array_add (closure->xlocked, NULL);
		*xlocked = (gchar **)g_ptr_array_free (closure->xlocked, FALSE);
		closure->xlocked = NULL;
	}

	return count;
}

static void
service_xlock_async (SecretService *self,
                     const gchar *method,
                     GList *objects,
                     GCancellable *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
	GSimpleAsyncResult *res;
	XlockClosure *closure;
	GHashTable *table;
	GPtrArray *paths;
	const gchar *path;
	GList *l;

	table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
	paths = g_ptr_array_new ();

	for (l = objects; l != NULL; l = g_list_next (l)) {
		path = g_dbus_proxy_get_object_path (l->data);
		g_ptr_array_add (paths, (gpointer)path);
		g_hash_table_insert (table, g_strdup (path), g_object_ref (l->data));
	}
	g_ptr_array_add (paths, NULL);

	res = service_xlock_paths_async (self, method, (const gchar **)paths->pdata,
	                                 cancellable, callback, user_data);

	closure = g_simple_async_result_get_op_res_gpointer (res);
	closure->objects = table;

	g_ptr_array_free (paths, TRUE);
	g_object_unref (res);
}

static gint
service_xlock_finish (SecretService *self,
                      GAsyncResult *result,
                      GList **xlocked,
                      GError **error)
{
	XlockClosure *closure;
	gchar **paths = NULL;
	GObject *object;
	gint count;
	guint i;

	count = service_xlock_paths_finish (self, result,
	                                    xlocked ? &paths : NULL,
	                                    error);

	if (count > 0 && xlocked) {
		closure = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result));
		*xlocked = NULL;

		for (i = 0; paths[i] != NULL; i++) {
			object = g_hash_table_lookup (closure->objects, paths[i]);
			if (object != NULL)
				*xlocked = g_list_prepend (*xlocked, g_object_ref (object));
		}

		*xlocked = g_list_reverse (*xlocked);
	}

	return count;

}

/**
 * secret_service_lock:
 * @self: the secret service
 * @objects: (element-type GLib.DBusProxy): the items or collections to lock
 * @cancellable: optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to pass to the callback
 *
 * Lock items or collections in the secret service.
 *
 * This method takes a list of #SecretItem or #SecretCollection proxy objects.
 * If you only have the D-Bus object paths of the items or collections, use
 * secret_service_lock_paths() instead.
 *
 * The secret service may not be able to lock items individually, and may
 * lock an entire collection instead.
 *
 * This method returns immediately and completes asynchronously. The secret
 * service may prompt the user. secret_service_prompt() will be used to handle
 * any prompts that show up.
 */
void
secret_service_lock (SecretService *self,
                     GList *objects,
                     GCancellable *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
	g_return_if_fail (SECRET_IS_SERVICE (self));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	service_xlock_async (self, "Lock", objects, cancellable, callback, user_data);
}

/**
 * secret_service_lock_finish:
 * @self: the secret service
 * @result: asynchronous result passed to the callback
 * @locked: (out) (element-type GLib.DBusProxy) (transfer full) (allow-none):
 *          location to place list of items or collections that were locked
 * @error: location to place an error on failure
 *
 * Complete asynchronous operation to lock items or collections in the secret
 * service.
 *
 * The secret service may not be able to lock items individually, and may
 * lock an entire collection instead.
 *
 * Returns: the number of items or collections that were locked
 */
gint
secret_service_lock_finish (SecretService *self,
                            GAsyncResult *result,
                            GList **locked,
                            GError **error)
{
	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	return service_xlock_finish (self, result, locked, error);
}

/**
 * secret_service_lock_sync:
 * @self: the secret service
 * @objects: (element-type GLib.DBusProxy): the items or collections to lock
 * @cancellable: optional cancellation object
 * @locked: (out) (element-type GLib.DBusProxy) (transfer full) (allow-none):
 *          location to place list of items or collections that were locked
 * @error: location to place an error on failure
 *
 * Lock items or collections in the secret service.
 *
 * This method takes a list of #SecretItem or #SecretCollection proxy objects.
 * If you only have the D-Bus object paths of the items or collections, use
 * secret_service_lock_paths_sync() instead.
 *
 * The secret service may not be able to lock items individually, and may
 * lock an entire collection instead.
 *
 * This method may block indefinitely and should not be used in user
 * interface threads. The secret service may prompt the user.
 * secret_service_prompt() will be used to handle any prompts that show up.
 *
 * Returns: the number of items or collections that were locked
 */
gint
secret_service_lock_sync (SecretService *self,
                          GList *objects,
                          GCancellable *cancellable,
                          GList **locked,
                          GError **error)
{
	SecretSync *sync;
	gint count;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), -1);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), -1);
	g_return_val_if_fail (error == NULL || *error == NULL, -1);

	sync = _secret_sync_new ();
	g_main_context_push_thread_default (sync->context);

	secret_service_lock (self, objects, cancellable,
	                      _secret_sync_on_result, sync);

	g_main_loop_run (sync->loop);

	count = secret_service_lock_finish (self, sync->result, locked, error);

	g_main_context_pop_thread_default (sync->context);
	_secret_sync_free (sync);

	return count;
}

/**
 * secret_service_unlock:
 * @self: the secret service
 * @objects: (element-type GLib.DBusProxy): the items or collections to unlock
 * @cancellable: optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to pass to the callback
 *
 * Unlock items or collections in the secret service.
 *
 * This method takes a list of #SecretItem or #SecretCollection proxy objects.
 * If you only have the D-Bus object paths of the items or collections, use
 * secret_service_unlock_paths() instead.
 *
 * The secret service may not be able to unlock items individually, and may
 * unlock an entire collection instead.
 *
 * This method may block indefinitely and should not be used in user
 * interface threads. The secret service may prompt the user.
 * secret_service_prompt() will be used to handle any prompts that show up.
 */
void
secret_service_unlock (SecretService *self,
                       GList *objects,
                       GCancellable *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer user_data)
{
	g_return_if_fail (SECRET_IS_SERVICE (self));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	service_xlock_async (self, "Unlock", objects, cancellable, callback, user_data);
}

/**
 * secret_service_unlock_finish:
 * @self: the secret service
 * @result: asynchronous result passed to the callback
 * @unlocked: (out) (element-type GLib.DBusProxy) (transfer full) (allow-none):
 *            location to place list of items or collections that were unlocked
 * @error: location to place an error on failure
 *
 * Complete asynchronous operation to unlock items or collections in the secret
 * service.
 *
 * The secret service may not be able to unlock items individually, and may
 * unlock an entire collection instead.
 *
 * Returns: the number of items or collections that were unlocked
 */
gint
secret_service_unlock_finish (SecretService *self,
                              GAsyncResult *result,
                              GList **unlocked,
                              GError **error)
{
	g_return_val_if_fail (SECRET_IS_SERVICE (self), -1);
	g_return_val_if_fail (error == NULL || *error == NULL, -1);
	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
	                      service_xlock_paths_async), -1);

	return service_xlock_finish (self, result, unlocked, error);
}

/**
 * secret_service_unlock_sync:
 * @self: the secret service
 * @objects: (element-type GLib.DBusProxy): the items or collections to unlock
 * @cancellable: optional cancellation object
 * @unlocked: (out) (element-type GLib.DBusProxy) (transfer full) (allow-none):
 *            location to place list of items or collections that were unlocked
 * @error: location to place an error on failure
 *
 * Unlock items or collections in the secret service.
 *
 * This method takes a list of #SecretItem or #SecretCollection proxy objects.
 * If you only have the D-Bus object paths of the items or collections, use
 * secret_service_unlock_paths_sync() instead.
 *
 * The secret service may not be able to unlock items individually, and may
 * unlock an entire collection instead.
 *
 * This method may block indefinitely and should not be used in user
 * interface threads. The secret service may prompt the user.
 * secret_service_prompt() will be used to handle any prompts that show up.
 *
 * Returns: the number of items or collections that were unlocked
 */
gint
secret_service_unlock_sync (SecretService *self,
                            GList *objects,
                            GCancellable *cancellable,
                            GList **unlocked,
                            GError **error)
{
	SecretSync *sync;
	gint count;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), -1);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), -1);
	g_return_val_if_fail (error == NULL || *error == NULL, -1);

	sync = _secret_sync_new ();
	g_main_context_push_thread_default (sync->context);

	secret_service_unlock (self, objects, cancellable,
	                        _secret_sync_on_result, sync);

	g_main_loop_run (sync->loop);

	count = secret_service_unlock_finish (self, sync->result,
	                                       unlocked, error);

	g_main_context_pop_thread_default (sync->context);
	_secret_sync_free (sync);

	return count;
}

/**
 * secret_service_store:
 * @self: the secret service
 * @schema: (allow-none): the schema to use to check attributes
 * @attributes: (element-type utf8 utf8): the attribute keys and values
 * @collection_path: (allow-none): the D-Bus path to the collection where to store the secret
 * @label: label for the secret
 * @value: the secret value
 * @cancellable: optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to be passed to the callback
 *
 * Store a secret value in the secret service.
 *
 * The @attributes should be a set of key and value string pairs.
 *
 * If the attributes match a secret item already stored in the collection, then
 * the item will be updated with these new values.
 *
 * If @collection_path is not specified, then the default collection will be
 * used. Use #SECRET_COLLECTION_SESSION to store the password in the session
 * collection, which doesn't get stored across login sessions.
 *
 * This method will return immediately and complete asynchronously.
 */
void
secret_service_store (SecretService *self,
                      const SecretSchema *schema,
                      GHashTable *attributes,
                      const gchar *collection_path,
                      const gchar *label,
                      SecretValue *value,
                      GCancellable *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
	const gchar *schema_name;
	GHashTable *properties;
	GVariant *propval;

	g_return_if_fail (SECRET_IS_SERVICE (self));
	g_return_if_fail (attributes != NULL);
	g_return_if_fail (label != NULL);
	g_return_if_fail (value != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* Warnings raised already */
	if (schema != NULL && !_secret_attributes_validate (schema, attributes))
		return;

	properties = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
	                                    (GDestroyNotify)g_variant_unref);

	propval = g_variant_new_string (label);
	g_hash_table_insert (properties,
	                     SECRET_ITEM_INTERFACE ".Label",
	                     g_variant_ref_sink (propval));

	/* Always store the schema name in the attributes */
	schema_name = (schema == NULL) ? NULL : schema->name;
	propval = _secret_attributes_to_variant (attributes, schema_name);
	g_hash_table_insert (properties,
	                     SECRET_ITEM_INTERFACE ".Attributes",
	                     g_variant_ref_sink (propval));

	secret_service_create_item_path (self, collection_path, properties, value,
	                                 TRUE, cancellable, callback, user_data);

	g_hash_table_unref (properties);
}

/**
 * secret_service_store_finish:
 * @self: the secret service
 * @result: the asynchronous result passed to the callback
 * @error: location to place an error on failure
 *
 * Finish asynchronous operation to store a secret value in the secret service.
 *
 * Returns: whether the storage was successful or not
 */
gboolean
secret_service_store_finish (SecretService *self,
                             GAsyncResult *result,
                             GError **error)
{
	gchar *path;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	path = secret_service_create_item_path_finish (self, result, error);

	g_free (path);
	return path != NULL;
}

/**
 * secret_service_store_sync:
 * @self: the secret service
 * @schema: (allow-none): the schema for the attributes
 * @attributes: (element-type utf8 utf8): the attribute keys and values
 * @collection_path: (allow-none): the D-Bus path to the collection where to store the secret
 * @label: label for the secret
 * @value: the secret value
 * @cancellable: optional cancellation object
 * @error: location to place an error on failure
 *
 * Store a secret value in the secret service.
 *
 * The @attributes should be a set of key and value string pairs.
 *
 * If the attributes match a secret item already stored in the collection, then
 * the item will be updated with these new values.
 *
 * If @collection_path is %NULL, then the default collection will be
 * used. Use #SECRET_COLLECTION_SESSION to store the password in the session
 * collection, which doesn't get stored across login sessions.
 *
 * This method may block indefinitely and should not be used in user interface
 * threads.
 *
 * Returns: whether the storage was successful or not
 */
gboolean
secret_service_store_sync (SecretService *self,
                           const SecretSchema *schema,
                           GHashTable *attributes,
                           const gchar *collection_path,
                           const gchar *label,
                           SecretValue *value,
                           GCancellable *cancellable,
                           GError **error)
{
	SecretSync *sync;
	gboolean ret;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (attributes != NULL, FALSE);
	g_return_val_if_fail (label != NULL, FALSE);
	g_return_val_if_fail (value != NULL, FALSE);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* Warnings raised already */
	if (schema != NULL && !_secret_attributes_validate (schema, attributes))
		return FALSE;

	sync = _secret_sync_new ();
	g_main_context_push_thread_default (sync->context);

	secret_service_store (self, schema, attributes, collection_path,
	                       label, value, cancellable, _secret_sync_on_result, sync);

	g_main_loop_run (sync->loop);

	ret = secret_service_store_finish (self, sync->result, error);

	g_main_context_pop_thread_default (sync->context);
	_secret_sync_free (sync);

	return ret;
}

typedef struct {
	SecretValue *value;
	GCancellable *cancellable;
} LookupClosure;

static void
lookup_closure_free (gpointer data)
{
	LookupClosure *closure = data;
	if (closure->value)
		secret_value_unref (closure->value);
	g_clear_object (&closure->cancellable);
	g_slice_free (LookupClosure, closure);
}

static void
on_lookup_get_secret (GObject *source,
                      GAsyncResult *result,
                      gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	LookupClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	SecretService *self = SECRET_SERVICE (source);
	GError *error = NULL;

	closure->value = secret_service_get_secret_for_path_finish (self, result, &error);
	if (error != NULL)
		g_simple_async_result_take_error (res, error);

	g_simple_async_result_complete (res);
	g_object_unref (res);
}

static void
on_lookup_unlocked (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	LookupClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	SecretService *self = SECRET_SERVICE (source);
	GError *error = NULL;
	gchar **unlocked = NULL;

	secret_service_unlock_paths_finish (SECRET_SERVICE (source),
	                                     result, &unlocked, &error);
	if (error != NULL) {
		g_simple_async_result_take_error (res, error);
		g_simple_async_result_complete (res);

	} else if (unlocked && unlocked[0]) {
		secret_service_get_secret_for_path (self, unlocked[0],
		                                    closure->cancellable,
		                                    on_lookup_get_secret,
		                                    g_object_ref (res));

	} else {
		g_simple_async_result_complete (res);
	}

	g_strfreev (unlocked);
	g_object_unref (res);
}

static void
on_lookup_searched (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	LookupClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	SecretService *self = SECRET_SERVICE (source);
	GError *error = NULL;
	gchar **unlocked = NULL;
	gchar **locked = NULL;

	secret_service_search_for_paths_finish (self, result, &unlocked, &locked, &error);
	if (error != NULL) {
		g_simple_async_result_take_error (res, error);
		g_simple_async_result_complete (res);

	} else if (unlocked && unlocked[0]) {
		secret_service_get_secret_for_path (self, unlocked[0],
		                                    closure->cancellable,
		                                    on_lookup_get_secret,
		                                    g_object_ref (res));

	} else if (locked && locked[0]) {
		const gchar *paths[] = { locked[0], NULL };
		secret_service_unlock_paths (self, paths,
		                             closure->cancellable,
		                             on_lookup_unlocked,
		                             g_object_ref (res));

	} else {
		g_simple_async_result_complete (res);
	}

	g_strfreev (unlocked);
	g_strfreev (locked);
	g_object_unref (res);
}

/**
 * secret_service_lookup:
 * @self: the secret service
 * @schema: (allow-none): the schema for the attributes
 * @attributes: (element-type utf8 utf8): the attribute keys and values
 * @cancellable: optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to be passed to the callback
 *
 * Lookup a secret value in the secret service.
 *
 * The @attributes should be a set of key and value string pairs.
 *
 * This method will return immediately and complete asynchronously.
 */
void
secret_service_lookup (SecretService *self,
                       const SecretSchema *schema,
                       GHashTable *attributes,
                       GCancellable *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer user_data)
{
	const gchar *schema_name = NULL;
	GSimpleAsyncResult *res;
	LookupClosure *closure;
	GVariant *variant;

	g_return_if_fail (SECRET_IS_SERVICE (self));
	g_return_if_fail (attributes != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* Warnings raised already */
	if (schema != NULL && !_secret_attributes_validate (schema, attributes))
		return;

	res = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
	                                 secret_service_lookup);
	closure = g_slice_new0 (LookupClosure);
	closure->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
	g_simple_async_result_set_op_res_gpointer (res, closure, lookup_closure_free);

	if (schema != NULL && !(schema->flags & SECRET_SCHEMA_DONT_MATCH_NAME))
		schema_name = schema->name;
	variant = _secret_attributes_to_variant (attributes, schema_name);

	_secret_service_search_for_paths_variant (self, variant, cancellable,
	                                          on_lookup_searched, g_object_ref (res));

	g_object_unref (res);
}

/**
 * secret_service_lookup_finish:
 * @self: the secret service
 * @result: the asynchronous result passed to the callback
 * @error: location to place an error on failure
 *
 * Finish asynchronous operation to lookup a secret value in the secret service.
 *
 * If no secret is found then %NULL is returned.
 *
 * Returns: (transfer full): a newly allocated #SecretValue, which should be
 *          released with secret_value_unref(), or %NULL if no secret found
 */
SecretValue *
secret_service_lookup_finish (SecretService *self,
                              GAsyncResult *result,
                              GError **error)
{
	GSimpleAsyncResult *res;
	LookupClosure *closure;
	SecretValue *value;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);
	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
	                      secret_service_lookup), NULL);

	res = G_SIMPLE_ASYNC_RESULT (result);
	if (g_simple_async_result_propagate_error (res, error))
		return NULL;

	closure = g_simple_async_result_get_op_res_gpointer (res);
	value = closure->value;
	closure->value = NULL;
	return value;
}

/**
 * secret_service_lookup_sync:
 * @self: the secret service
 * @schema: (allow-none): the schema for the attributes
 * @attributes: (element-type utf8 utf8): the attribute keys and values
 * @cancellable: optional cancellation object
 * @error: location to place an error on failure
 *
 * Lookup a secret value in the secret service.
 *
 * The @attributes should be a set of key and value string pairs.
 *
 * This method may block indefinitely and should not be used in user interface
 * threads.
 *
 * Returns: (transfer full): a newly allocated #SecretValue, which should be
 *          released with secret_value_unref(), or %NULL if no secret found
 */
SecretValue *
secret_service_lookup_sync (SecretService *self,
                            const SecretSchema *schema,
                            GHashTable *attributes,
                            GCancellable *cancellable,
                            GError **error)
{
	SecretSync *sync;
	SecretValue *value;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), NULL);
	g_return_val_if_fail (attributes != NULL, NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);

	/* Warnings raised already */
	if (schema != NULL && !_secret_attributes_validate (schema, attributes))
		return NULL;

	sync = _secret_sync_new ();
	g_main_context_push_thread_default (sync->context);

	secret_service_lookup (self, schema, attributes, cancellable,
	                       _secret_sync_on_result, sync);

	g_main_loop_run (sync->loop);

	value = secret_service_lookup_finish (self, sync->result, error);

	g_main_context_pop_thread_default (sync->context);
	_secret_sync_free (sync);

	return value;
}

typedef struct {
	GCancellable *cancellable;
	SecretPrompt *prompt;
	gboolean deleted;
} DeleteClosure;

static void
delete_closure_free (gpointer data)
{
	DeleteClosure *closure = data;
	g_clear_object (&closure->prompt);
	g_clear_object (&closure->cancellable);
	g_slice_free (DeleteClosure, closure);
}

static void
on_delete_password_complete (GObject *source,
                             GAsyncResult *result,
                             gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	SecretService *self = SECRET_SERVICE (g_async_result_get_source_object (user_data));
	DeleteClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	GError *error = NULL;

	closure->deleted = secret_service_delete_path_finish (self, result, &error);
	if (error != NULL)
		g_simple_async_result_take_error (res, error);

	g_simple_async_result_complete (res);

	g_object_unref (self);
	g_object_unref (res);
}

static void
on_search_delete_password (GObject *source,
                           GAsyncResult *result,
                           gpointer user_data)
{
	GSimpleAsyncResult *res = G_SIMPLE_ASYNC_RESULT (user_data);
	DeleteClosure *closure = g_simple_async_result_get_op_res_gpointer (res);
	SecretService *self = SECRET_SERVICE (g_async_result_get_source_object (user_data));
	const gchar *path = NULL;
	GError *error = NULL;
	gchar **locked;
	gchar **unlocked;

	secret_service_search_for_paths_finish (self, result, &unlocked, &locked, &error);
	if (error != NULL) {
		g_simple_async_result_take_error (res, error);
		g_simple_async_result_complete (res);

	} else {
		/* Choose the first path */
		if (unlocked && unlocked[0])
			path = unlocked[0];
		else if (locked && locked[0])
			path = locked[0];

		/* Nothing to delete? */
		if (path == NULL) {
			closure->deleted = FALSE;
			g_simple_async_result_complete (res);

		/* Delete the first path */
		} else {
			closure->deleted = TRUE;
			_secret_service_delete_path (self, path, TRUE,
			                             closure->cancellable,
			                             on_delete_password_complete,
			                             g_object_ref (res));
		}
	}

	g_strfreev (locked);
	g_strfreev (unlocked);
	g_object_unref (self);
	g_object_unref (res);
}

/**
 * secret_service_remove:
 * @self: the secret service
 * @schema: (allow-none): the schema for the attributes
 * @attributes: (element-type utf8 utf8): the attribute keys and values
 * @cancellable: optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to be passed to the callback
 *
 * Remove a secret value from the secret service.
 *
 * The @attributes should be a set of key and value string pairs.
 *
 * If multiple items match the attributes, then only one will be deleted.
 *
 * This method will return immediately and complete asynchronously.
 */
void
secret_service_remove (SecretService *self,
                       const SecretSchema *schema,
                       GHashTable *attributes,
                       GCancellable *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer user_data)
{
	const gchar *schema_name = NULL;
	GSimpleAsyncResult *res;
	DeleteClosure *closure;
	GVariant *variant;

	g_return_if_fail (SECRET_SERVICE (self));
	g_return_if_fail (attributes != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* Warnings raised already */
	if (schema != NULL && !_secret_attributes_validate (schema, attributes))
		return;

	res = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
	                                 secret_service_remove);
	closure = g_slice_new0 (DeleteClosure);
	closure->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
	g_simple_async_result_set_op_res_gpointer (res, closure, delete_closure_free);

	if (schema != NULL && !(schema->flags & SECRET_SCHEMA_DONT_MATCH_NAME))
		schema_name = schema->name;
	variant = _secret_attributes_to_variant (attributes, schema_name);

	_secret_service_search_for_paths_variant (self, variant, cancellable,
	                                          on_search_delete_password, g_object_ref (res));

	g_object_unref (res);
}

/**
 * secret_service_remove_finish:
 * @self: the secret service
 * @result: the asynchronous result passed to the callback
 * @error: location to place an error on failure
 *
 * Finish asynchronous operation to remove a secret value from the secret
 * service.
 *
 * Returns: whether the removal was successful or not
 */
gboolean
secret_service_remove_finish (SecretService *self,
                              GAsyncResult *result,
                              GError **error)
{
	GSimpleAsyncResult *res;
	DeleteClosure *closure;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
	                      secret_service_remove), FALSE);

	res = G_SIMPLE_ASYNC_RESULT (result);
	if (g_simple_async_result_propagate_error (res, error))
		return FALSE;

	closure = g_simple_async_result_get_op_res_gpointer (res);
	return closure->deleted;
}

/**
 * secret_service_remove_sync:
 * @self: the secret service
 * @schema: (allow-none): the schema for the attributes
 * @attributes: (element-type utf8 utf8): the attribute keys and values
 * @cancellable: optional cancellation object
 * @error: location to place an error on failure
 *
 * Remove a secret value from the secret service.
 *
 * The @attributes should be a set of key and value string pairs.
 *
 * If multiple items match the attributes, then only one will be deleted.
 *
 * This method may block indefinitely and should not be used in user interface
 * threads.
 *
 * Returns: whether the removal was successful or not
 */
gboolean
secret_service_remove_sync (SecretService *self,
                            const SecretSchema *schema,
                            GHashTable *attributes,
                            GCancellable *cancellable,
                            GError **error)
{
	SecretSync *sync;
	gboolean result;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* Warnings raised already */
	if (schema != NULL && !_secret_attributes_validate (schema, attributes))
		return FALSE;

	sync = _secret_sync_new ();
	g_main_context_push_thread_default (sync->context);

	secret_service_remove (self, schema, attributes, cancellable,
	                       _secret_sync_on_result, sync);

	g_main_loop_run (sync->loop);

	result = secret_service_remove_finish (self, sync->result, error);

	g_main_context_pop_thread_default (sync->context);
	_secret_sync_free (sync);

	return result;
}

typedef struct {
	GCancellable *cancellable;
	SecretCollection *collection;
} ReadClosure;

static void
read_closure_free (gpointer data)
{
	ReadClosure *read = data;
	if (read->collection)
		g_object_unref (read->collection);
	if (read->cancellable)
		g_object_unref (read->cancellable);
	g_slice_free (ReadClosure, read);
}

static void
on_read_alias_collection (GObject *source,
                          GAsyncResult *result,
                          gpointer user_data)
{
	GSimpleAsyncResult *async = G_SIMPLE_ASYNC_RESULT (user_data);
	ReadClosure *read = g_simple_async_result_get_op_res_gpointer (async);
	GError *error = NULL;

	read->collection = secret_collection_new_finish (result, &error);
	if (error != NULL)
		g_simple_async_result_take_error (async, error);

	g_simple_async_result_complete (async);
	g_object_unref (async);
}

static void
on_read_alias_path (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	GSimpleAsyncResult *async = G_SIMPLE_ASYNC_RESULT (user_data);
	ReadClosure *read = g_simple_async_result_get_op_res_gpointer (async);
	SecretService *self = SECRET_SERVICE (source);
	GError *error = NULL;
	gchar *collection_path;

	collection_path = secret_service_read_alias_path_finish (self, result, &error);
	if (error == NULL) {

		/* No collection for this alias */
		if (collection_path == NULL) {
			g_simple_async_result_complete (async);

		} else {
			read->collection = _secret_service_find_collection_instance (self,
			                                                             collection_path);
			if (read->collection != NULL) {
				g_simple_async_result_complete (async);

			/* No collection loaded, but valid path, load */
			} else {
				secret_collection_new (self, collection_path, read->cancellable,
				                       on_read_alias_collection, g_object_ref (async));
			}
		}

	} else {
		g_simple_async_result_take_error (async, error);
		g_simple_async_result_complete (async);
	}

	g_free (collection_path);
	g_object_unref (async);
}

/**
 * secret_service_read_alias:
 * @self: a secret service object
 * @alias: the alias to lookup
 * @cancellable: (allow-none): optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to pass to the callback
 *
 * Lookup which collection is assigned to this alias. Aliases help determine
 * well known collections, such as 'default'.
 *
 * This method will return immediately and complete asynchronously.
 */
void
secret_service_read_alias (SecretService *self,
                           const gchar *alias,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
	GSimpleAsyncResult *async;
	ReadClosure *read;

	g_return_if_fail (SECRET_IS_SERVICE (self));
	g_return_if_fail (alias != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	async = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
	                                   secret_service_read_alias);
	read = g_slice_new0 (ReadClosure);
	read->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
	g_simple_async_result_set_op_res_gpointer (async, read, read_closure_free);

	secret_service_read_alias_path (self, alias, cancellable,
	                                on_read_alias_path, g_object_ref (async));

	g_object_unref (async);
}

/**
 * secret_service_read_alias_finish:
 * @self: a secret service object
 * @result: asynchronous result passed to callback
 * @error: location to place error on failure
 *
 * Finish an asynchronous operation to lookup which collection is assigned
 * to an alias.
 *
 * Returns: (transfer full): the collection, or %NULL if none assigned to the alias
 */
SecretCollection *
secret_service_read_alias_finish (SecretService *self,
                                  GAsyncResult *result,
                                  GError **error)
{
	GSimpleAsyncResult *async;
	ReadClosure *read;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), NULL);
	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
	                      secret_service_read_alias), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	async = G_SIMPLE_ASYNC_RESULT (result);
	if (g_simple_async_result_propagate_error (async, error))
		return NULL;
	read = g_simple_async_result_get_op_res_gpointer (async);
	if (read->collection)
		g_object_ref (read->collection);
	return read->collection;
}

/**
 * secret_service_read_alias_sync:
 * @self: a secret service object
 * @alias: the alias to lookup
 * @cancellable: (allow-none): optional cancellation object
 * @error: location to place error on failure
 *
 * Lookup which collection is assigned to this alias. Aliases help determine
 * well known collections, such as 'default'.
 *
 * This method may block and should not be used in user interface threads.
 *
 * Returns: (transfer full): the collection, or %NULL if none assigned to the alias
 */
SecretCollection *
secret_service_read_alias_sync (SecretService *self,
                                const gchar *alias,
                                GCancellable *cancellable,
                                GError **error)
{
	SecretCollection *collection;
	gchar *collection_path;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), NULL);
	g_return_val_if_fail (alias != NULL, NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	collection_path = secret_service_read_alias_path_sync (self, alias,
	                                                       cancellable, error);
	if (collection_path == NULL)
		return NULL;

	/* No collection for this alias */
	if (collection_path == NULL) {
		collection = NULL;

	} else {
		collection = _secret_service_find_collection_instance (self,
		                                                       collection_path);

		/* No collection loaded, but valid path, load */
		if (collection == NULL) {
			collection = secret_collection_new_sync (self, collection_path,
			                                         cancellable, error);
		}
	}

	g_free (collection_path);
	return collection;
}

/**
 * secret_service_set_alias:
 * @self: a secret service object
 * @alias: the alias to assign the collection to
 * @collection: (allow-none): the collection to assign to the alias
 * @cancellable: (allow-none): optional cancellation object
 * @callback: called when the operation completes
 * @user_data: data to pass to the callback
 *
 * Assign a collection to this alias. Aliases help determine
 * well known collections, such as 'default'.
 *
 * This method will return immediately and complete asynchronously.
 */
void
secret_service_set_alias (SecretService *self,
                          const gchar *alias,
                          SecretCollection *collection,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
	const gchar *collection_path;

	g_return_if_fail (SECRET_IS_SERVICE (self));
	g_return_if_fail (alias != NULL);
	g_return_if_fail (collection == NULL || SECRET_IS_COLLECTION (collection));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	if (collection) {
		collection_path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (collection));
		g_return_if_fail (collection != NULL);
	} else {
		collection_path = NULL;
	}

	secret_service_set_alias_path (self, alias, collection_path, cancellable,
	                               callback, user_data);
}

/**
 * secret_service_set_alias_finish:
 * @self: a secret service object
 * @result: asynchronous result passed to callback
 * @error: location to place error on failure
 *
 * Finish an asynchronous operation to assign a collection to an alias.
 *
 * Returns: %TRUE if successful
 */
gboolean
secret_service_set_alias_finish (SecretService *self,
                                 GAsyncResult *result,
                                 GError **error)
{
	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	return secret_service_set_alias_path_finish (self, result, error);
}

/**
 * secret_service_set_alias_sync:
 * @self: a secret service object
 * @alias: the alias to assign the collection to
 * @collection: (allow-none): the collection to assign to the alias
 * @cancellable: (allow-none): optional cancellation object
 * @error: location to place error on failure
 *
 * Assign a collection to this alias. Aliases help determine
 * well known collections, such as 'default'.
 *
 * This method may block and should not be used in user interface threads.
 *
 * Returns: %TRUE if successful
 */
gboolean
secret_service_set_alias_sync (SecretService *self,
                               const gchar *alias,
                               SecretCollection *collection,
                               GCancellable *cancellable,
                               GError **error)
{
	SecretSync *sync;
	gboolean ret;

	g_return_val_if_fail (SECRET_IS_SERVICE (self), FALSE);
	g_return_val_if_fail (alias != NULL, FALSE);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	sync = _secret_sync_new ();
	g_main_context_push_thread_default (sync->context);

	secret_service_set_alias (self, alias, collection, cancellable,
	                          _secret_sync_on_result, sync);

	g_main_loop_run (sync->loop);

	ret = secret_service_set_alias_finish (self, sync->result, error);

	g_main_context_pop_thread_default (sync->context);
	_secret_sync_free (sync);

	return ret;
}
