/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2016 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * LTTng UST application context provider.
 */

#define _LGPL_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#include <ust-context-provider.h>

#include "context-internal.h"
#include "lttng-tracer-core.h"
#include "jhash.h"
#include "context-provider-internal.h"
#include <ust-helper.h>

#define CONTEXT_PROVIDER_HT_BITS	12
#define CONTEXT_PROVIDER_HT_SIZE	(1U << CONTEXT_PROVIDER_HT_BITS)
struct context_provider_ht {
	struct cds_hlist_head table[CONTEXT_PROVIDER_HT_SIZE];
};

static struct context_provider_ht context_provider_ht;

static struct lttng_ust_context_provider *
		lookup_provider_by_name(const char *name)
{
	struct cds_hlist_head *head;
	struct cds_hlist_node *node;
	struct lttng_ust_context_provider *provider;
	uint32_t hash;
	const char *end;
	size_t len;

	/* Lookup using everything before first ':' as key. */
	end = strchr(name, ':');
	if (end)
		len = end - name;
	else
		len = strlen(name);
	hash = jhash(name, len, 0);
	head = &context_provider_ht.table[hash & (CONTEXT_PROVIDER_HT_SIZE - 1)];
	cds_hlist_for_each_entry(provider, node, head, node) {
		if (!strncmp(provider->name, name, len))
			return provider;
	}
	return NULL;
}

int lttng_ust_context_provider_register(struct lttng_ust_context_provider *provider)
{
	struct cds_hlist_head *head;
	size_t name_len = strlen(provider->name);
	uint32_t hash;
	int ret = 0;

	lttng_ust_fixup_tls();

	/* Provider name starts with "$app.". */
	if (strncmp("$app.", provider->name, strlen("$app.")) != 0)
		return -EINVAL;
	/* Provider name cannot contain a colon character. */
	if (strchr(provider->name, ':'))
		return -EINVAL;
	if (ust_lock()) {
		ret = -EBUSY;
		goto end;
	}
	if (lookup_provider_by_name(provider->name)) {
		ret = -EBUSY;
		goto end;
	}
	hash = jhash(provider->name, name_len, 0);
	head = &context_provider_ht.table[hash & (CONTEXT_PROVIDER_HT_SIZE - 1)];
	cds_hlist_add_head(&provider->node, head);

	lttng_ust_context_set_session_provider(provider->name,
		provider->get_size, provider->record,
		provider->get_value);

	lttng_ust_context_set_event_notifier_group_provider(provider->name,
		provider->get_size, provider->record,
		provider->get_value);
end:
	ust_unlock();
	return ret;
}

void lttng_ust_context_provider_unregister(struct lttng_ust_context_provider *provider)
{
	lttng_ust_fixup_tls();

	if (ust_lock())
		goto end;
	lttng_ust_context_set_session_provider(provider->name,
		lttng_ust_dummy_get_size, lttng_ust_dummy_record,
		lttng_ust_dummy_get_value);

	lttng_ust_context_set_event_notifier_group_provider(provider->name,
		lttng_ust_dummy_get_size, lttng_ust_dummy_record,
		lttng_ust_dummy_get_value);

	cds_hlist_del(&provider->node);
end:
	ust_unlock();
}

/*
 * Called with ust mutex held.
 * Add application context to array of context, even if the application
 * context is not currently loaded by application. It will then use the
 * dummy callbacks in that case.
 * Always performed before tracing is started, since it modifies
 * metadata describing the context.
 */
int lttng_ust_add_app_context_to_ctx_rcu(const char *name,
		struct lttng_ust_ctx **ctx)
{
	struct lttng_ust_context_provider *provider;
	struct lttng_ust_ctx_field *new_field = NULL;
	int ret;

	if (*ctx && lttng_find_context(*ctx, name))
		return -EEXIST;
	new_field = zmalloc(sizeof(struct lttng_ust_ctx_field));
	if (!new_field) {
		ret = -ENOMEM;
		goto error_field_alloc;
	}
	new_field->struct_size = sizeof(struct lttng_ust_ctx_field);
	new_field->event_field = zmalloc(sizeof(struct lttng_ust_event_field));
	if (!new_field->event_field) {
		ret = -ENOMEM;
		goto error_event_field_alloc;
	}
	new_field->event_field->name = strdup(name);
	if (!new_field->event_field->name) {
		ret = -ENOMEM;
		goto error_field_name_alloc;
	}
	new_field->event_field->type = zmalloc(sizeof(struct lttng_ust_type_common));
	if (!new_field->event_field->type) {
		ret = -ENOMEM;
		goto error_field_type_alloc;
	}
	new_field->event_field->type->type = lttng_ust_type_dynamic;
	/*
	 * If provider is not found, we add the context anyway, but
	 * it will provide a dummy context.
	 */
	provider = lookup_provider_by_name(name);
	if (provider) {
		new_field->get_size = provider->get_size;
		new_field->record = provider->record;
		new_field->get_value = provider->get_value;
	} else {
		new_field->get_size = lttng_ust_dummy_get_size;
		new_field->record = lttng_ust_dummy_record;
		new_field->get_value = lttng_ust_dummy_get_value;
	}
	/*
	 * For application context, add it by expanding
	 * ctx array. Ownership of new_field is passed to the callee on
	 * success.
	 */
	ret = lttng_context_add_rcu(ctx, new_field);
	if (ret) {
		free(new_field->event_field->type);
		free((char *) new_field->event_field->name);
		free(new_field->event_field);
		free(new_field);
		return ret;
	}
	return 0;

error_field_type_alloc:
	free((char *) new_field->event_field->name);
error_field_name_alloc:
	free(new_field->event_field);
error_event_field_alloc:
	free(new_field);
error_field_alloc:
	return ret;
}
