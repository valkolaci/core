/* Copyright (c) 2005-2023 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "llist.h"
#include "str.h"
#include "strescape.h"
#include "event-filter-private.h"
#include "var-expand.h"
#include "wildcard-match.h"
#include "mmap-util.h"
#include "settings.h"

enum set_seen_type {
	/* Setting has not been changed */
	SET_SEEN_NO,
	/* Setting has been changed */
	SET_SEEN_YES,
	/* Setting has been changed with SETTINGS_OVERRIDE_TYPE_2ND_DEFAULT.
	   If SETTINGS_OVERRIDE_TYPE_DEFAULT override is still found, use that
	   instead. */
	SET_SEEN_DEFAULT,
};

struct settings_mmap_pool {
	struct pool pool;
	int refcount;

	struct settings_mmap_pool *prev, *next;

	const char *source_filename;
	unsigned int source_linenum;

	pool_t parent_pool;
	struct settings_mmap *mmap; /* NULL for unit tests */
	struct settings_root *root;
};

struct settings_override {
	pool_t pool;
	enum settings_override_type type;

	/* Number of '/' characters in orig_key + 1 */
	unsigned int path_element_count;
	/* key += value is used, i.e. append this value to existing value */
	bool append;
	/* TRUE once all the filter elements have been processed in "key",
	   and it points to a non-filter suffix of the path. */
	bool filter_finished;
	/* Always apply this override, regardless of any filters. */
	bool always_match;
	/* Original key for the overridden setting, e.g.
	   namespace/inbox/mailbox/Sent/mail_attribute/dict_driver */
	const char *orig_key;
	/* key starts as orig_key, but it keeps being updated to skip over
	   the filter elements, e.g. finally when filter_finished=TRUE, it's
	   just "dict_driver" */
	const char *key;
	/* Value for the overridden setting */
	const char *value;

	/* Event filter being generated from the key as it's being processed. */
	struct event_filter *filter;
	/* Event being generated from the key as it's being processed.
	   The event contains every named list filter as key=value, and any
	   named filter as settings_filter_name=name. */
	struct event *filter_event;
	/* Last filter element's key, and for list filters the value, while
	   the key is being processed. In the above example:
	   - "namespace", "inbox"
	   - "mailbox", "Sent"
	   - "mail_attribute", NULL
	*/
	const char *last_filter_key, *last_filter_value;
};
ARRAY_DEFINE_TYPE(settings_override, struct settings_override);
ARRAY_DEFINE_TYPE(settings_override_p, struct settings_override *);

struct settings_mmap_block {
	const char *name;
	size_t block_end_offset;

	const char *error; /* if non-NULL, accessing the block must fail */
	size_t base_start_offset, base_end_offset;

	uint32_t filter_count;
	size_t filter_indexes_start_offset;
	size_t filter_offsets_start_offset;

	uint32_t settings_count;
	size_t settings_keys_offset;
	/* TRUE if settings have been validated against setting_parser_info */
	bool settings_validated;
};

struct settings_mmap {
	int refcount;
	pool_t pool;
	struct settings_root *root;

	void *mmap_base;
	size_t mmap_size;

	struct event_filter **event_filters;
	unsigned int event_filters_count;

	HASH_TABLE(const char *, struct settings_mmap_block *) blocks;
};

struct settings_root {
	pool_t pool;
	const char *protocol_name;
	struct settings_mmap *mmap;
	ARRAY_TYPE(settings_override) overrides;

	struct settings_mmap_pool *settings_pools;
};

struct settings_instance {
	pool_t pool;
	struct settings_root *root;
	struct settings_mmap *mmap;
	ARRAY_TYPE(settings_override) overrides;
};

struct settings_apply_ctx {
	struct event *event;
	struct settings_root *root;
	struct settings_instance *instance;
	const struct setting_parser_info *info;
	enum settings_get_flags flags;
	pool_t temp_pool;

	const char *filter_key;
	const char *filter_value;
	const char *filter_name;
	bool filter_name_required;

	struct setting_parser_context *parser;
	struct settings_mmap_pool *mpool;
	void *set_struct;
	ARRAY(enum set_seen_type) set_seen;
	ARRAY_TYPE(settings_override_p) overrides;

	string_t *str;
	const struct var_expand_table *const *tables;
	const struct var_expand_func_table *const *func_tables;
	void *const *func_contexts;
};

static ARRAY(const struct setting_parser_info *) set_registered_infos;

static const char *settings_override_type_names[] = {
	"default", "userdb", "-o parameter",
	"2nd default", "2nd parameter", "hardcoded"
};
static_assert_array_size(settings_override_type_names,
			 SETTINGS_OVERRIDE_TYPE_COUNT);

static struct event_filter event_filter_match_never, event_filter_match_always;
#define EVENT_FILTER_MATCH_ALWAYS (&event_filter_match_always)
#define EVENT_FILTER_MATCH_NEVER (&event_filter_match_never)

static int
settings_instance_override(struct settings_apply_ctx *ctx,
			   struct event_filter *event_filter,
			   const char **error_r);

static unsigned int path_element_count(const char *key)
{
	unsigned int count = 1;
	while ((key = strchr(key, '/')) != NULL) {
		key++;
		count++;
	}
	return count;
}

static int
settings_block_read_uint32(struct settings_mmap *mmap,
			   size_t *offset, size_t end_offset,
			   const char *name, uint32_t *num_r,
			   const char **error_r)
{
	if (*offset + sizeof(*num_r) > end_offset) {
		*error_r = t_strdup_printf(
			"Area too small when reading uint of '%s' "
			"(offset=%zu, end_offset=%zu, file_size=%zu)", name,
			*offset, end_offset, mmap->mmap_size);
		return -1;
	}
	*num_r = be32_to_cpu_unaligned(CONST_PTR_OFFSET(mmap->mmap_base, *offset));
	*offset += sizeof(*num_r);
	return 0;
}

static int
settings_block_read_size(struct settings_mmap *mmap,
			 size_t *offset, size_t end_offset,
			 const char *name, uint64_t *size_r,
			 const char **error_r)
{
	if (*offset + sizeof(*size_r) > end_offset) {
		*error_r = t_strdup_printf(
			"Area too small when reading size of '%s' "
			"(offset=%zu, end_offset=%zu, file_size=%zu)", name,
			*offset, end_offset, mmap->mmap_size);
		return -1;
	}
	*size_r = be64_to_cpu_unaligned(CONST_PTR_OFFSET(mmap->mmap_base, *offset));
	if (*size_r > end_offset - *offset - sizeof(*size_r)) {
		*error_r = t_strdup_printf(
			"'%s' points outside area "
			"(offset=%zu, size=%"PRIu64", end_offset=%zu, file_size=%zu)",
			name, *offset, *size_r, end_offset,
			mmap->mmap_size);
		return -1;
	}
	*offset += sizeof(*size_r);
	return 0;
}

static int
settings_block_read_str(struct settings_mmap *mmap,
			size_t *offset, size_t end_offset, const char *name,
			const char **str_r, const char **error_r)
{
	*str_r = (const char *)mmap->mmap_base + *offset;
	*offset += strlen(*str_r) + 1;
	if (*offset > end_offset) {
		*error_r = t_strdup_printf("'%s' points outside area "
			"(offset=%zu, end_offset=%zu, file_size=%zu)",
			name, *offset, end_offset, mmap->mmap_size);
		return -1;
	}
	return 0;
}

static int
settings_read_filters(struct settings_mmap *mmap, const char *service_name,
		      enum settings_read_flags flags, size_t *offset,
		      ARRAY_TYPE(const_string) *protocols, const char **error_r)
{
	const char *filter_string, *error;

	if (settings_block_read_uint32(mmap, offset, mmap->mmap_size,
				       "filters count",
				       &mmap->event_filters_count,
				       error_r) < 0)
		return -1;

	mmap->event_filters = mmap->event_filters_count == 0 ? NULL :
		p_new(mmap->pool, struct event_filter *, mmap->event_filters_count);

	for (uint32_t i = 0; i < mmap->event_filters_count; i++) {
		if (settings_block_read_str(mmap, offset, mmap->mmap_size,
					    "filter string", &filter_string,
					    error_r) < 0)
			return -1;
		if (filter_string[0] == '\0') {
			mmap->event_filters[i] = EVENT_FILTER_MATCH_ALWAYS;
			continue;
		}

		struct event_filter *tmp_filter = event_filter_create();
		if (event_filter_parse_case_sensitive(filter_string,
						      tmp_filter, &error) < 0) {
			*error_r = t_strdup_printf(
				"Received invalid filter '%s' at index %u: %s",
				filter_string, i, error);
			event_filter_unref(&tmp_filter);
			return -1;
		}
		bool op_not;
		const char *value =
			event_filter_find_field_exact(tmp_filter, "protocol", &op_not);
		if (value != NULL) {
			if (op_not)
				value = t_strconcat("!", value, NULL);
			if (array_lsearch(protocols, &value, i_strcmp_p) == NULL) {
				value = t_strdup(value);
				array_push_back(protocols, &value);
			}

			if (mmap->root->protocol_name != NULL &&
			    (strcmp(mmap->root->protocol_name, value) == 0) == op_not &&
			    (flags & SETTINGS_READ_NO_PROTOCOL_FILTER) == 0) {
				/* protocol doesn't match */
				mmap->event_filters[i] = EVENT_FILTER_MATCH_NEVER;
				event_filter_unref(&tmp_filter);
				continue;
			}
		}
		value = event_filter_find_field_exact(tmp_filter, "service", &op_not);
		if (value != NULL && service_name != NULL &&
		    (strcmp(value, service_name) == 0) == op_not) {
			/* service name doesn't match */
			mmap->event_filters[i] = EVENT_FILTER_MATCH_NEVER;
			event_filter_unref(&tmp_filter);
			continue;
		}

		mmap->event_filters[i] = event_filter_create_with_pool(mmap->pool);
		pool_ref(mmap->pool);
		event_filter_merge(mmap->event_filters[i], tmp_filter,
				   EVENT_FILTER_MERGE_OP_OR);
		event_filter_unref(&tmp_filter);
	}
	return 0;
}

static int
settings_block_read(struct settings_mmap *mmap, size_t *_offset,
		    const char **error_r)
{
	size_t offset = *_offset;
	size_t block_size_offset = offset;
	const char *key, *error;

	/* <block size> */
	uint64_t block_size;
	if (settings_block_read_size(mmap, &offset, mmap->mmap_size,
				     "block size", &block_size, error_r) < 0)
		return -1;
	size_t block_end_offset = offset + block_size;

	/* Verify that block ends with NUL. This way we can safely use strlen()
	   later on and we know it won't read past the mmaped memory area and
	   cause a crash. The NUL is either from the last settings value or
	   from the last error string. */
	if (((const char *)mmap->mmap_base)[block_end_offset-1] != '\0') {
		*error_r = t_strdup_printf(
			"Settings block doesn't end with NUL at offset %zu",
			block_end_offset-1);
		return -1;
	}

	/* <block name> */
	const char *block_name;
	if (settings_block_read_str(mmap, &offset, block_end_offset,
				    "block name", &block_name, error_r) < 0)
		return -1;

	struct settings_mmap_block *block =
		hash_table_lookup(mmap->blocks, block_name);
	if (block != NULL) {
		*error_r = t_strdup_printf(
			"Duplicate block name '%s' (offset=%zu)",
			block_name, block_size_offset);
		return -1;
	}
	block = p_new(mmap->pool, struct settings_mmap_block, 1);
	block->name = block_name;
	block->block_end_offset = block_end_offset;
	hash_table_insert(mmap->blocks, block->name, block);

	/* <settings count> */
	if (settings_block_read_uint32(mmap, &offset, block_end_offset,
				       "settings count", &block->settings_count,
				       error_r) < 0)
		return -1;
	block->settings_keys_offset = offset;
	/* skip over the settings keys for now - they will be validated later */
	for (uint32_t i = 0; i < block->settings_count; i++) {
		if (settings_block_read_str(mmap, &offset, block_end_offset,
					    "setting key", &key, error_r) < 0)
			return -1;
	}

	/* <base settings size> */
	uint64_t base_settings_size;
	if (settings_block_read_size(mmap, &offset, block_end_offset,
				     "base settings size", &base_settings_size,
				     error_r) < 0)
		return -1;
	block->base_end_offset = offset + base_settings_size;

	/* <base settings error string> */
	if (settings_block_read_str(mmap, &offset,
				    block->base_end_offset,
				    "base settings error", &error,
				    error_r) < 0)
		return -1;
	if (error[0] != '\0')
		block->error = error;
	block->base_start_offset = offset;

	/* skip over the key-value pairs */
	offset = block->base_end_offset;

	/* <filter count> */
	if (settings_block_read_uint32(mmap, &offset, block_end_offset,
				       "filter count", &block->filter_count,
				       error_r) < 0)
		return -1;

	/* filters */
	unsigned int filter_idx;
	for (filter_idx = 0; filter_idx < block->filter_count; filter_idx++) {
		/* <filter settings size> */
		uint64_t filter_settings_size;
		if (settings_block_read_size(mmap, &offset,
				block_end_offset, "filter settings size",
				&filter_settings_size, error_r) < 0)
			return -1;

		size_t tmp_offset = offset;
		size_t filter_end_offset = offset + filter_settings_size;
		if (settings_block_read_str(mmap, &tmp_offset,
					    filter_end_offset,
					    "filter error string", &error,
					    error_r) < 0)
			return -1;

		/* skip over the filter contents for now */
		offset += filter_settings_size;
	}

	block->filter_indexes_start_offset = offset;
	offset += sizeof(uint32_t) * block->filter_count;
	block->filter_offsets_start_offset = offset;
	offset += sizeof(uint64_t) * block->filter_count;
	offset++; /* safety NUL */

	if (offset != block_end_offset) {
		*error_r = t_strdup_printf(
			"Filter end offset mismatch (%zu != %zu)",
			offset, block_end_offset);
		return -1;
	}
	*_offset = offset;
	return 0;
}

static int
settings_mmap_parse(struct settings_mmap *mmap, const char *service_name,
		    enum settings_read_flags flags,
		    const char *const **specific_protocols_r,
		    const char **error_r)
{
	/*
	   See ../config/config-dump-full.c for the binary config file format
	   description.

	   Settings are read until the blob size is reached. There is no
	   padding/alignment. */
	const unsigned char *mmap_base = mmap->mmap_base;
	size_t mmap_size = mmap->mmap_size;
	ARRAY_TYPE(const_string) protocols;

	t_array_init(&protocols, 8);

	const char *magic_prefix = "DOVECOT-CONFIG\t";
	const unsigned int magic_prefix_len = strlen(magic_prefix);
	const unsigned char *eol = memchr(mmap_base, '\n', mmap_size);
	if (mmap_size < magic_prefix_len ||
	    memcmp(magic_prefix, mmap_base, magic_prefix_len) != 0 ||
	    eol == NULL) {
		*error_r = "File header doesn't begin with DOVECOT-CONFIG line";
		return -1;
	}
	if (mmap_base[magic_prefix_len] != '1' ||
	    mmap_base[magic_prefix_len+1] != '.') {
		*error_r = t_strdup_printf(
			"Unsupported config file version '%s'",
			t_strdup_until(mmap_base + magic_prefix_len, eol));
		return -1;
	}

	/* <settings full size> */
	size_t full_size_offset = eol - mmap_base + 1;
	uint64_t settings_full_size =
		be64_to_cpu_unaligned(mmap_base + full_size_offset);
	if (full_size_offset + sizeof(settings_full_size) +
	    settings_full_size != mmap_size) {
		*error_r = t_strdup_printf("Full size mismatch: "
			"Expected %zu + %zu + %"PRIu64", but file size is %zu",
			full_size_offset, sizeof(settings_full_size),
			settings_full_size, mmap_size);
		return -1;
	}

	size_t offset = full_size_offset + sizeof(settings_full_size);
	if (settings_read_filters(mmap, service_name, flags, &offset,
				  &protocols, error_r) < 0)
		return -1;

	do {
		if (settings_block_read(mmap, &offset, error_r) < 0)
			return -1;
	} while (offset < mmap_size);

	if (array_count(&protocols) > 0) {
		array_append_zero(&protocols);
		*specific_protocols_r = array_front(&protocols);
	} else {
		*specific_protocols_r = NULL;
	}
	return 0;
}

static const char *
get_invalid_setting_error(struct settings_apply_ctx *ctx, const char *prefix,
			  const char *key,
			  const char *value, const char *orig_value)
{
	string_t *str = t_str_new(64);
	str_printfa(str, "%s %s=%s", prefix, key, value);
	if (strcmp(value, orig_value) != 0)
		str_printfa(str, " (before expansion: %s)", orig_value);
	str_printfa(str, ": %s", settings_parser_get_error(ctx->parser));
	return str_c(str);
}

static int
settings_mmap_apply_key(struct settings_apply_ctx *ctx, unsigned int key_idx,
			const char *list_key, const char *value,
			const char **error_r)
{
	const char *key = ctx->info->defines[key_idx].key;
	const char *orig_value = value;
	if (list_key != NULL)
		key = t_strdup_printf("%s/%s", key, list_key);

	/* call settings_apply() before variable expansion */
	enum setting_apply_flags apply_flags =
		(ctx->flags & SETTINGS_GET_FLAG_NO_EXPAND) == 0 ? 0 :
		SETTING_APPLY_FLAG_NO_EXPAND;
	if (ctx->info->setting_apply != NULL &&
	    !ctx->info->setting_apply(ctx->event, ctx->set_struct, key, &value,
				      apply_flags, error_r)) {
		*error_r = t_strdup_printf("Invalid setting %s=%s: %s",
					   key, orig_value, *error_r);
		return -1;
	}

	if ((ctx->flags & SETTINGS_GET_FLAG_NO_EXPAND) == 0 &&
	    /* FIXME: plugin { .. } settings aren't expanded here. Eventually
	       plugin strlist should be removed entirely. */
	    (ctx->info->defines[key_idx].type != SET_STRLIST ||
	     strcmp(ctx->info->defines[key_idx].key, "plugin") != 0) &&
	    ctx->info->defines[key_idx].type != SET_STR_NOVARS &&
	    ctx->info->defines[key_idx].type != SET_FILTER_ARRAY) {
		const char *error;
		str_truncate(ctx->str, 0);
		if (var_expand_with_arrays(ctx->str, value, ctx->tables,
					   ctx->func_tables, ctx->func_contexts,
					   &error) <= 0 &&
		    (ctx->flags & SETTINGS_GET_FLAG_FAKE_EXPAND) == 0) {
			*error_r = t_strdup_printf(
				"Failed to expand %s setting variables: %s",
				key, error);
			return -1;
		}
		if (strcmp(value, str_c(ctx->str)) != 0)
			value = p_strdup(&ctx->mpool->pool, str_c(ctx->str));
	}
	/* value points to mmap()ed memory, which is kept
	   referenced by the set_pool for the life time of the
	   settings struct. */
	if (settings_parse_keyidx_value_nodup(ctx->parser, key_idx,
					      key, value) < 0) {
		*error_r = get_invalid_setting_error(ctx, "Invalid setting",
						     key, value, orig_value);
		return -1;
	}
	return 0;
}

static int
settings_mmap_apply_defaults(struct settings_apply_ctx *ctx,
			     const char **error_r)
{
	enum setting_apply_flags apply_flags = SETTING_APPLY_FLAG_OVERRIDE |
		((ctx->flags & SETTINGS_GET_FLAG_NO_EXPAND) == 0 ? 0 :
		 SETTING_APPLY_FLAG_NO_EXPAND);
	const char *error;
	unsigned int key_idx;

	for (key_idx = 0; ctx->info->defines[key_idx].key != NULL; key_idx++) {
		enum set_seen_type *set_seenp =
			array_idx_get_space(&ctx->set_seen, key_idx);
		if (*set_seenp == SET_SEEN_YES)
			continue;

		void *set = PTR_OFFSET(ctx->info->defaults,
				       ctx->info->defines[key_idx].offset);
		if (ctx->info->defines[key_idx].type != SET_STR)
			continue; /* not needed for now */
		const char *key = ctx->info->defines[key_idx].key;
		const char *const *valuep = set;
		const char *value = *valuep;
		if (value == NULL)
			continue;

		if (ctx->info->setting_apply != NULL &&
		    !ctx->info->setting_apply(ctx->event, ctx->set_struct, key,
					      &value, apply_flags, &error))
			i_panic("BUG: Failed to apply default setting %s=%s: %s",
				key, value, error);

		if ((ctx->flags & SETTINGS_GET_FLAG_NO_EXPAND) == 0) {
			const char *error;
			str_truncate(ctx->str, 0);
			if (var_expand_with_arrays(ctx->str, value, ctx->tables,
						   ctx->func_tables,
						   ctx->func_contexts,
						   &error) <= 0 &&
			    (ctx->flags & SETTINGS_GET_FLAG_FAKE_EXPAND) == 0) {
				i_panic("BUG: Failed to expand default setting %s=%s variables: %s",
					key, value, error);
				return -1;
			}
			if (strcmp(value, str_c(ctx->str)) != 0) {
				if (settings_parse_keyidx_value(
						ctx->parser, key_idx,
						key, str_c(ctx->str)) < 0) {
					*error_r = get_invalid_setting_error(ctx,
						"Invalid default setting",
						key, str_c(ctx->str), value);
					return -1;
				}
			}
		}
	}
	return 0;
}

static int
settings_mmap_apply_blob(struct settings_apply_ctx *ctx,
			 struct settings_mmap_block *block,
			 size_t start_offset, size_t end_offset,
			 const char **error_r)
{
	struct settings_mmap *mmap = ctx->instance->mmap;
	size_t offset = start_offset;

	/* list of settings: key, value, ... */
	while (offset < end_offset) {
		/* We already checked that settings blob ends with NUL, so
		   strlen() can be used safely. */
		uint32_t key_idx = be32_to_cpu_unaligned(
			CONST_PTR_OFFSET(mmap->mmap_base, offset));
		if (key_idx >= block->settings_count) {
			*error_r = t_strdup_printf(
				"Settings key index too high (%u >= %u)",
				key_idx, block->settings_count);
			return -1;
		}
		offset += sizeof(key_idx);

		bool set_apply = TRUE;
		const char *list_key = NULL;

		enum set_seen_type *set_seenp =
			array_idx_get_space(&ctx->set_seen, key_idx);
		if (*set_seenp != SET_SEEN_NO) {
			/* Already seen this setting - don't set it again.
			   This check is used also with boollists when the
			   whole list is replaced. */
			set_apply = FALSE;
		}
		if (ctx->info->defines[key_idx].type == SET_STRLIST ||
		    ctx->info->defines[key_idx].type == SET_BOOLLIST) {
			list_key = (const char *)mmap->mmap_base + offset;
			offset += strlen(list_key)+1;
			set_apply = set_apply &&
				!settings_parse_list_has_key(ctx->parser,
					key_idx, settings_section_unescape(list_key));
		} else if (ctx->info->defines[key_idx].type == SET_FILTER_ARRAY)
			set_apply = TRUE;
		else if (set_apply)
			*set_seenp = SET_SEEN_YES;

		if (offset >= end_offset) {
			/* if offset==end_offset, the value is missing. */
			*error_r = t_strdup_printf(
				"Settings key/value points outside blob "
				"(offset=%zu, end_offset=%zu, file_size=%zu)",
				offset, end_offset, mmap->mmap_size);
			return -1;
		}
		const char *value = (const char *)mmap->mmap_base + offset;
		offset += strlen(value)+1;
		if (offset > end_offset) {
			*error_r = t_strdup_printf(
				"Settings value points outside blob "
				"(offset=%zu, end_offset=%zu, file_size=%zu)",
				offset, end_offset, mmap->mmap_size);
			return -1;
		}
		int ret;
		if (!set_apply)
			ret = 0;
		else T_BEGIN {
			ret = settings_mmap_apply_key(ctx, key_idx, list_key,
						      value, error_r);
		} T_END_PASS_STR_IF(ret < 0, error_r);
		if (ret < 0)
			return -1;
	}
	return 0;
}

static int settings_mmap_validate(struct settings_mmap *mmap,
				  struct settings_mmap_block *block,
				  const struct setting_parser_info *info,
				  const char **error_r)
{
	size_t offset = block->settings_keys_offset;
	const char *key;
	uint32_t i;

	for (i = 0; info->defines[i].key != NULL; i++) {
		if (i >= block->settings_count)
			break;
		if (settings_block_read_str(mmap, &offset,
					    block->block_end_offset,
					    "setting key", &key, error_r) < 0) {
			/* shouldn't happen - we already read it once */
			return -1;
		}
		if (strcmp(info->defines[i].key, key) != 0) {
			*error_r = t_strdup_printf(
				"settings struct %s #%u key mismatch %s != %s",
				info->name, i, info->defines[i].key, key);
			return -1;
		}
	}
	if (i != block->settings_count) {
		*error_r = t_strdup_printf(
			"settings struct %s count mismatch %u != %u",
			info->name, i+1, block->settings_count);
		return -1;
	}
	return 0;
}

static int
settings_mmap_apply(struct settings_apply_ctx *ctx, const char **error_r)
{
	struct settings_mmap *mmap = ctx->instance->mmap;
	struct settings_mmap_block *block =
		hash_table_lookup(mmap->blocks, ctx->info->name);
	if (block == NULL) {
		*error_r = t_strdup_printf(
			"BUG: Configuration has no settings struct named '%s'",
			ctx->info->name);
		return -1;
	}
	if (block->error != NULL) {
		*error_r = block->error;
		return -1;
	}

	if (!block->settings_validated &&
	    (ctx->flags & SETTINGS_GET_NO_KEY_VALIDATION) == 0) {
		if (settings_mmap_validate(mmap, block, ctx->info, error_r) < 0)
			return -1;
		block->settings_validated = TRUE;
	}

	const struct failure_context failure_ctx = {
		.type = LOG_TYPE_DEBUG,
	};

	/* go through the filters in reverse sorted order, so we always set the
	   setting just once, never overriding anything. */
	bool seen_filter = FALSE;
	for (uint32_t i = block->filter_count; i > 0; ) {
		i--;
		uint32_t event_filter_idx = be32_to_cpu_unaligned(
			CONST_PTR_OFFSET(mmap->mmap_base,
					 block->filter_indexes_start_offset +
					 sizeof(uint32_t) * i));
		if (event_filter_idx >= mmap->event_filters_count) {
			*error_r = t_strdup_printf("event filter idx %u >= %u",
				event_filter_idx, mmap->event_filters_count);
			return -1;
		}
		struct event_filter *event_filter =
			mmap->event_filters[event_filter_idx];
		if (event_filter == EVENT_FILTER_MATCH_NEVER)
			;
		else if (event_filter == EVENT_FILTER_MATCH_ALWAYS ||
			 event_filter_match(event_filter, ctx->event, &failure_ctx)) {
			uint64_t filter_offset = be64_to_cpu_unaligned(
				CONST_PTR_OFFSET(mmap->mmap_base,
						 block->filter_offsets_start_offset +
						 sizeof(uint64_t) * i));
			uint64_t filter_set_size = be64_to_cpu_unaligned(
				CONST_PTR_OFFSET(mmap->mmap_base, filter_offset));
			filter_offset += sizeof(filter_set_size);
			uint64_t filter_end_offset =
				filter_offset + filter_set_size;

			const char *filter_error =
				CONST_PTR_OFFSET(mmap->mmap_base,
						 filter_offset);
			if (filter_error[0] != '\0') {
				*error_r = filter_error;
				return -1;
			}
			filter_offset += strlen(filter_error) + 1;

			if (ctx->filter_name != NULL && !seen_filter) {
				bool op_not;
				const char *value =
					event_filter_find_field_exact(
						event_filter,
						SETTINGS_EVENT_FILTER_NAME, &op_not);
				/* NOTE: The event filter is using
				   EVENT_FIELD_EXACT, so the value has already
				   removed wildcard escapes. */
				if (value != NULL && !op_not &&
				    strcmp(ctx->filter_name, value) == 0)
					seen_filter = TRUE;
			}

			if (event_filter != EVENT_FILTER_MATCH_ALWAYS) {
				int ret = settings_instance_override(ctx,
						event_filter, error_r);
				if (ret < 0)
					return -1;
				if (ret > 0)
					seen_filter = TRUE;
			}

			if (settings_mmap_apply_blob(ctx, block,
					filter_offset, filter_end_offset,
					error_r) < 0)
				return -1;
		}
	}

	/* Apply any leftover overrides after filters and
	   filter overrides were already handled. This way global
	   setting overrides don't override named filters' settings,
	   unless the override is specifically using the filter name
	   as prefix. */
	int ret = settings_instance_override(ctx, NULL, error_r);
	if (ret < 0)
		return -1;
	if (ret > 0)
		seen_filter = TRUE;

	/* apply the base settings last after all filters */
	if (settings_mmap_apply_blob(ctx, block, block->base_start_offset,
				     block->base_end_offset, error_r) < 0)
		return -1;
	return seen_filter ? 1 : 0;

}

static void settings_mmap_ref(struct settings_mmap *mmap)
{
	i_assert(mmap->refcount > 0);

	mmap->refcount++;
}

static void settings_mmap_unref(struct settings_mmap **_mmap)
{
	struct settings_mmap *mmap = *_mmap;
	if (mmap == NULL)
		return;
	i_assert(mmap->refcount > 0);

	*_mmap = NULL;
	if (--mmap->refcount > 0)
		return;

	for (unsigned int i = 0; i < mmap->event_filters_count; i++) {
		if (mmap->event_filters[i] != EVENT_FILTER_MATCH_ALWAYS &&
		    mmap->event_filters[i] != EVENT_FILTER_MATCH_NEVER)
			event_filter_unref(&mmap->event_filters[i]);
	}
	hash_table_destroy(&mmap->blocks);

	if (munmap(mmap->mmap_base, mmap->mmap_size) < 0)
		i_error("munmap(<config>) failed: %m");
	pool_unref(&mmap->pool);
}

int settings_read(struct settings_root *root, int fd, const char *path,
		  const char *service_name, const char *protocol_name,
		  enum settings_read_flags flags,
		  const char *const **specific_protocols_r,
		  const char **error_r)
{
	pool_t pool = pool_alloconly_create("settings mmap", 1024*16);
	struct settings_mmap *mmap = p_new(pool, struct settings_mmap, 1);
	mmap->refcount = 1;
	mmap->pool = pool;
	mmap->mmap_base = mmap_ro_file(fd, &mmap->mmap_size);
	if (mmap->mmap_base == MAP_FAILED)
		i_fatal("Failed to read config: mmap(%s) failed: %m", path);
	if (mmap->mmap_size == 0)
		i_fatal("Failed to read config: %s file size is empty", path);
	/* Remember the protocol for following settings lookups */
	root->protocol_name = p_strdup(root->pool, protocol_name);

	settings_mmap_unref(&root->mmap);
	mmap->root = root;
	root->mmap = mmap;
	hash_table_create(&mmap->blocks, mmap->pool, 0, str_hash, strcmp);

	return settings_mmap_parse(root->mmap, service_name, flags,
				   specific_protocols_r, error_r);
}

bool settings_has_mmap(struct settings_root *root)
{
	return root->mmap != NULL;
}

static const char *settings_mmap_pool_get_name(pool_t pool)
{
	struct settings_mmap_pool *mpool =
		container_of(pool, struct settings_mmap_pool, pool);

	return pool_get_name(mpool->parent_pool);
}

static void settings_mmap_pool_ref(pool_t pool)
{
	struct settings_mmap_pool *mpool =
		container_of(pool, struct settings_mmap_pool, pool);

	i_assert(mpool->refcount > 0);
	mpool->refcount++;
}

static void settings_mmap_pool_unref(pool_t *pool)
{
	struct settings_mmap_pool *mpool =
		container_of(*pool, struct settings_mmap_pool, pool);

	i_assert(mpool->refcount > 0);
	*pool = NULL;
	if (--mpool->refcount > 0)
		return;

	DLLIST_REMOVE(&mpool->root->settings_pools, mpool);

	settings_mmap_unref(&mpool->mmap);
	pool_external_refs_unref(&mpool->pool);
	pool_unref(&mpool->parent_pool);
}

static void *settings_mmap_pool_malloc(pool_t pool, size_t size)
{
	struct settings_mmap_pool *mpool =
		container_of(pool, struct settings_mmap_pool, pool);

	return p_malloc(mpool->parent_pool, size);
}

static void settings_mmap_pool_free(pool_t pool, void *mem)
{
	struct settings_mmap_pool *mpool =
		container_of(pool, struct settings_mmap_pool, pool);

	p_free(mpool->parent_pool, mem);
}

static void *settings_mmap_pool_realloc(pool_t pool, void *mem,
					size_t old_size, size_t new_size)
{
	struct settings_mmap_pool *mpool =
		container_of(pool, struct settings_mmap_pool, pool);

	return p_realloc(mpool->parent_pool, mem, old_size, new_size);
}

static void settings_mmap_pool_clear(pool_t pool ATTR_UNUSED)
{
	i_panic("settings_mmap_pool_clear() must not be called");
}

static size_t settings_mmap_pool_get_max_easy_alloc_size(pool_t pool)
{
	struct settings_mmap_pool *mpool =
		container_of(pool, struct settings_mmap_pool, pool);

	return p_get_max_easy_alloc_size(mpool->parent_pool);
}

static struct pool_vfuncs static_settings_mmap_pool_vfuncs = {
	settings_mmap_pool_get_name,

	settings_mmap_pool_ref,
	settings_mmap_pool_unref,

	settings_mmap_pool_malloc,
	settings_mmap_pool_free,

	settings_mmap_pool_realloc,

	settings_mmap_pool_clear,
	settings_mmap_pool_get_max_easy_alloc_size
};

static struct settings_mmap_pool *
settings_mmap_pool_create(struct settings_root *root,
			  struct settings_mmap *mmap,
			  const char *source_filename,
			  unsigned int source_linenum)
{
	struct settings_mmap_pool *mpool;
	pool_t parent_pool =
		pool_alloconly_create("settings mmap pool", 256);

	mpool = p_new(parent_pool, struct settings_mmap_pool, 1);
	mpool->pool.v = &static_settings_mmap_pool_vfuncs;
	mpool->pool.alloconly_pool = TRUE;
	mpool->refcount = 1;
	mpool->parent_pool = parent_pool;
	mpool->root = root;
	mpool->mmap = mmap;
	mpool->source_filename = source_filename;
	mpool->source_linenum = source_linenum;
	if (mmap != NULL)
		settings_mmap_ref(mmap);

	DLLIST_PREPEND(&root->settings_pools, mpool);
	return mpool;
}

static void
settings_var_expand_init(struct settings_apply_ctx *ctx)
{
	struct event *event = ctx->event;
	ARRAY(const struct var_expand_table *) tables;
	ARRAY(const struct var_expand_func_table *) func_tables;
	ARRAY(void *) func_contexts;
	const struct var_expand_table *table;
	const struct var_expand_func_table *func_table;
	void *func_context;

	t_array_init(&tables, 4);
	t_array_init(&func_tables, 4);
	t_array_init(&func_contexts, 4);

	while (event != NULL) {
		settings_var_expand_t *callback =
			event_get_ptr(event, SETTINGS_EVENT_VAR_EXPAND_CALLBACK);
		if (callback != NULL) {
			callback(event, &table, &func_table);
			if (table != NULL)
				array_push_back(&tables, &table);
			if (func_table != NULL) {
				func_context = event_get_ptr(event,
					SETTINGS_EVENT_VAR_EXPAND_FUNC_CONTEXT);
				array_push_back(&func_tables, &func_table);
				array_push_back(&func_contexts, &func_context);
			}
		}

		table = event_get_ptr(event, SETTINGS_EVENT_VAR_EXPAND_TABLE);
		if (table != NULL)
			array_push_back(&tables, &table);

		func_table = event_get_ptr(event, SETTINGS_EVENT_VAR_EXPAND_FUNC_TABLE);
		if (func_table != NULL) {
			func_context = event_get_ptr(event,
				SETTINGS_EVENT_VAR_EXPAND_FUNC_CONTEXT);
			array_push_back(&func_tables, &func_table);
			array_push_back(&func_contexts, &func_context);
		}

		event = event_get_parent(event);
	}
	array_append_zero(&tables);
	array_append_zero(&func_tables);

	ctx->tables = array_front(&tables);
	ctx->func_tables = array_front(&func_tables);
	ctx->func_contexts = array_count(&func_contexts) == 0 ? NULL :
		array_front(&func_contexts);
}

static int settings_override_cmp(struct settings_override *const *set1,
				 struct settings_override *const *set2)
{
	int ret = (int)(*set2)->type - (int)(*set1)->type;
	if (ret != 0)
		return ret;

	/* Return more specific filters first. This is mainly necessary with
	   hierarchical settings, e.g. fs_parent/fs_parent/fs_driver so the
	   highest hierarchy count settings are returned first. */
	return (int)(*set2)->path_element_count -
		(int)(*set1)->path_element_count;

}


static bool
settings_key_part_find(struct settings_apply_ctx *ctx, const char **key,
		       const char *last_filter_key,
		       const char *last_filter_value,
		       unsigned int *key_idx_r)
{
	if (last_filter_value != NULL) {
		i_assert(last_filter_key != NULL);
		/* Try filter/name/key -> filter_key. Do this before the
		   non-prefixed check, so e.g. inet_listener/imap/ssl won't
		   try to change the global ssl setting. */
		const char *key_prefix = last_filter_key;
		if (strcmp(key_prefix, SETTINGS_EVENT_MAILBOX_NAME_WITHOUT_PREFIX) == 0)
			key_prefix = SETTINGS_EVENT_MAILBOX_NAME_WITH_PREFIX;
		const char *prefixed_key =
			t_strdup_printf("%s_%s", key_prefix, *key);
		if (setting_parser_info_find_key(ctx->info, prefixed_key,
						 key_idx_r)) {
			*key = prefixed_key;
			return TRUE;
		}
	}
	return setting_parser_info_find_key(ctx->info, *key, key_idx_r);
}

static int
settings_override_filter_match(struct settings_apply_ctx *ctx,
			       struct settings_override *set,
			       const char **error_r)
{
	const struct failure_context failure_ctx = {
		.type = LOG_TYPE_DEBUG
	};
	unsigned int key_idx;
	const char *p, *error;

	/* check the filter that exists so far */
	if (set->filter != NULL &&
	    !event_filter_match(set->filter, ctx->event, &failure_ctx))
		return 0;
	if (set->filter_finished)
		return 1;

	struct event_field *set_filter_names =
		event_find_field_nonrecursive(ctx->event,
					      SETTINGS_EVENT_FILTER_NAME);
	if (set_filter_names != NULL &&
	    set_filter_names->value_type != EVENT_FIELD_VALUE_TYPE_STRLIST)
		set_filter_names = NULL;

	bool filter_finished = TRUE;
	string_t *filter_string = NULL;
	const char *last_filter_key = set->last_filter_key;
	const char *last_filter_value = set->last_filter_value;
	size_t last_filter_key_pos = SIZE_MAX;
	while ((p = strchr(set->key, SETTINGS_SEPARATOR)) != NULL) {
		/* see if the info struct knows about the next part in the key. */
		const char *part = t_strdup_until(set->key, p);
		enum setting_type set_type;

		if (settings_key_part_find(ctx, &part, last_filter_key,
					   last_filter_value, &key_idx))
			set_type = ctx->info->defines[key_idx].type;
		else if (set_filter_names != NULL &&
			 array_lsearch(&set_filter_names->value.strlist,
				       &part, i_strcmp_p) != NULL) {
			/* If SETTINGS_EVENT_FILTER_NAME is set, we can assume
			   that any key prefix with the same name is of type
			   SET_FILTER_NAME. This is mainly intended for
			   "doveadm fs" filter-name parameter matching to work
			   with all filters, which otherwise wouldn't be
			   visible to the settings override code. */
			set_type = SET_FILTER_NAME;
		} else if (strcmp(part, "*") == 0 && set->filter == NULL) {
			/* always match, also for any named list filters */
			set->filter_finished = TRUE;
			set->always_match = TRUE;
			set->key = p + 1;
			return 1;
		} else {
			filter_finished = FALSE;
			break;
		}
		if (set_type == SET_STRLIST || set_type == SET_BOOLLIST)
			break;

		if (filter_string == NULL)
			filter_string = t_str_new(64);
		else
			str_append(filter_string, " AND ");
		if (set->filter_event == NULL)
			set->filter_event = event_create(NULL);
		switch (set_type) {
		case SET_FILTER_NAME:
			last_filter_key = part;
			last_filter_value = NULL;
			str_printfa(filter_string, SETTINGS_EVENT_FILTER_NAME"=\"%s\"",
				    wildcard_str_escape(last_filter_key));
			event_add_str(set->filter_event,
				      SETTINGS_EVENT_FILTER_NAME,
				      last_filter_key);
			break;
		case SET_FILTER_HIERARCHY: {
			/* add the full repeated hierarchy here */
			const char *next;

			str_printfa(filter_string, SETTINGS_EVENT_FILTER_NAME"=\"%s",
				    wildcard_str_escape(part));
			while (str_begins(p + 1, part, &next) &&
			       next[0] == SETTINGS_SEPARATOR) {
				str_append_c(filter_string, '/');
				str_append(filter_string,
					   wildcard_str_escape(part));
				p = next;
			}
			str_append_c(filter_string, '"');

			last_filter_key = t_strdup_until(set->key, p);
			last_filter_value = NULL;
			event_add_str(set->filter_event,
				      SETTINGS_EVENT_FILTER_NAME,
				      last_filter_key);
			break;
		}
		case SET_FILTER_ARRAY: {
			const char *value = p + 1;
			p = strchr(value, SETTINGS_SEPARATOR);
			if (p == NULL) {
				*error_r = t_strdup_printf(
					"Setting override %s is missing filter name child element ('/child' expected)",
					set->key);
				return -1;
			}
			last_filter_key = part;
			if (strcmp(last_filter_key, SETTINGS_EVENT_MAILBOX_NAME_WITH_PREFIX) == 0)
				last_filter_key = SETTINGS_EVENT_MAILBOX_NAME_WITHOUT_PREFIX;
			last_filter_value = t_strdup_until(value, p);
			last_filter_key_pos = str_len(filter_string);
			str_printfa(filter_string, "\"%s\"=\"%s\"",
				    wildcard_str_escape(last_filter_key),
				    str_escape(last_filter_value));
			event_add_str(set->filter_event,
				      last_filter_key, last_filter_value);
			break;
		}
		default:
			*error_r = t_strdup_printf(
				"Setting override %s type doesn't support child elements in the path ('/' not expected)",
				set->key);
			return -1;
		}
		set->key = p + 1;
	}
	if (filter_finished && last_filter_value != NULL) {
		i_assert(last_filter_key_pos != SIZE_MAX);
		str_insert(filter_string, last_filter_key_pos, "(");
		str_printfa(filter_string, " OR "SETTINGS_EVENT_FILTER_NAME"=\"%s/%s\")",
			    last_filter_key, wildcard_str_escape(
				settings_section_escape(last_filter_value)));
	}

	if (filter_string != NULL) {
		struct event_filter *tmp_filter = event_filter_create();
		if (event_filter_parse_case_sensitive(str_c(filter_string),
						      tmp_filter, &error) < 0) {
			i_panic("BUG: Failed to create event filter filter for %s: %s (%s)",
				set->orig_key, error, str_c(filter_string));
		}
		if (set->filter == NULL) {
			set->filter = event_filter_create_with_pool(set->pool);
			pool_ref(set->pool);
		}
		event_filter_merge(set->filter, tmp_filter, EVENT_FILTER_MERGE_OP_AND);
		event_filter_unref(&tmp_filter);
	}

	if (set->last_filter_key != last_filter_key)
		set->last_filter_key = p_strdup(set->pool, last_filter_key);
	if (set->last_filter_value != last_filter_value)
		set->last_filter_value = p_strdup(set->pool, last_filter_value);
	set->filter_finished = filter_finished;
	return filter_finished &&
		(set->filter == NULL ||
		 event_filter_match(set->filter, ctx->event, &failure_ctx)) ? 1 : 0;
}

static int
settings_override_get_value(struct settings_apply_ctx *ctx,
			    const struct settings_override *set,
			    const char **_key, unsigned int *key_idx_r,
			    const char **value_r, const char **error_r)
{
	const char *key = *_key;
	unsigned int key_idx = UINT_MAX;

	if (!settings_key_part_find(ctx, &key, set->last_filter_key,
				    set->last_filter_value, &key_idx))
		key_idx = UINT_MAX;

	if (key_idx == UINT_MAX && strchr(key, '/') == NULL &&
	    set->type == SETTINGS_OVERRIDE_TYPE_USERDB &&
	    setting_parser_info_find_key(ctx->info, "plugin", &key_idx)) {
		/* FIXME: Setting is unknown in this parser. Since the parser
		   doesn't know all settings, we can't be sure if it's because
		   it should simply be ignored or because it's a plugin setting.
		   Just assume it's a plugin setting for now. This code will get
		   removed eventually once all plugin settings have been
		   converted away. */
		key = t_strconcat("plugin/", key, NULL);
	}
	if (key_idx == UINT_MAX)
		return 0;

	/* remove alias */
	const char *list = strchr(key, SETTINGS_SEPARATOR);
	if (list == NULL)
		key = ctx->info->defines[key_idx].key;
	else
		key = t_strconcat(ctx->info->defines[key_idx].key, list, NULL);

	if (!set->append) {
		*_key = key;
		*key_idx_r = key_idx;
		*value_r = set->value;
		return 1;
	}

	if (ctx->info->defines[key_idx].type != SET_STR) {
		*error_r = t_strdup_printf(
			"%s setting is not a string - can't use '+'", key);
		return -1;
	}
	const char *const *strp =
		PTR_OFFSET(ctx->set_struct, ctx->info->defines[key_idx].offset);
	*_key = key;
	*key_idx_r = key_idx;
	*value_r = t_strconcat(*strp, set->value, NULL);
	return 1;
}

static void
settings_instance_overrides_add_filters(struct settings_apply_ctx *ctx,
					const struct setting_parser_info *info)
{
	const struct setting_keyvalue *defaults =
		info->default_filter_settings;
	struct settings_override *set;

	if (defaults == NULL)
		return;
	for (unsigned int i = 0; defaults[i].key != NULL; i++) {
		set = p_new(ctx->temp_pool, struct settings_override, 1);
		set->pool = ctx->temp_pool;
		set->type = SETTINGS_OVERRIDE_TYPE_DEFAULT;
		set->key = set->orig_key = defaults[i].key;
		set->path_element_count = path_element_count(set->key);
		set->value = defaults[i].value;
		array_push_back(&ctx->overrides, &set);
	}
}

static void
settings_instance_override_init(struct settings_apply_ctx *ctx)
{
	struct settings_override *set;
	bool have_2nd_defaults = FALSE;

	t_array_init(&ctx->overrides, 64);
	if (array_is_created(&ctx->instance->overrides)) {
		array_foreach_modifiable(&ctx->instance->overrides, set) {
			if (set->type == SETTINGS_OVERRIDE_TYPE_2ND_DEFAULT)
				have_2nd_defaults = TRUE;
			array_push_back(&ctx->overrides, &set);
		}
	}
	if (array_is_created(&ctx->root->overrides)) {
		array_foreach_modifiable(&ctx->root->overrides, set) {
			if (set->type == SETTINGS_OVERRIDE_TYPE_2ND_DEFAULT)
				have_2nd_defaults = TRUE;
			array_push_back(&ctx->overrides, &set);
		}
	}
	if ((ctx->instance->mmap == NULL || have_2nd_defaults) &&
	    array_is_created(&set_registered_infos)) {
		/* a) No configuration - default_filter_settings won't be
		   applied unless we add them here also. This isn't for any
		   production use, so performance doesn't matter.
		   b) SETTINGS_OVERRIDE_TYPE_2ND_DEFAULT has been used -
		   defaults need to be applied on top of them. This is used
		   only in initialization code. */
		const struct setting_parser_info *info;
		ctx->temp_pool = pool_alloconly_create("settings temp pool", 256);
		array_foreach_elem(&set_registered_infos, info)
			settings_instance_overrides_add_filters(ctx, info);
	}
	/* sort overrides so that the most specific ones are first */
	array_sort(&ctx->overrides, settings_override_cmp);
}

static void settings_override_free(struct settings_override *override)
{
	event_filter_unref(&override->filter);
	event_unref(&override->filter_event);
}

static void
settings_apply_ctx_overrides_deinit(struct settings_apply_ctx *ctx)
{
	struct settings_override *set;

	array_foreach_elem(&ctx->overrides, set) {
		if (set != NULL && set->pool == ctx->temp_pool)
			settings_override_free(set);
	}
}

static int
settings_instance_override(struct settings_apply_ctx *ctx,
			   struct event_filter *event_filter,
			   const char **error_r)
{
	struct settings_override *set, **set_elem;
	const struct failure_context failure_ctx = {
		.type = LOG_TYPE_DEBUG
	};
	enum setting_apply_flags apply_flags = SETTING_APPLY_FLAG_OVERRIDE |
		((ctx->flags & SETTINGS_GET_FLAG_NO_EXPAND) == 0 ? 0 :
		 SETTING_APPLY_FLAG_NO_EXPAND);

	bool seen_filter = FALSE;
	array_foreach_modifiable(&ctx->overrides, set_elem) {
		if (*set_elem == NULL)
			continue; /* already applied */
		set = *set_elem;

		unsigned int key_idx;
		int ret;

		T_BEGIN {
			ret = settings_override_filter_match(ctx, set, error_r);
		} T_END_PASS_STR_IF(ret < 0, error_r);
		if (ret < 0)
			return -1;
		if (ret == 0)
			continue;

		/* If we're being called while applying filters, only apply
		   the overrides that have a matching filter. This preserves
		   the expected order in which settings are applied. */
		if (event_filter != NULL && !set->always_match &&
		    (set->filter_event == NULL ||
		     !event_filter_match(event_filter, set->filter_event,
					 &failure_ctx)))
			continue;

		if (ctx->filter_key != NULL && set->last_filter_key != NULL &&
		    strcmp(ctx->filter_key, set->last_filter_key) == 0 &&
		    null_strcmp(ctx->filter_value, set->last_filter_value) == 0)
			seen_filter = TRUE;

		/* Set key only after settings_override_filter_match() has
		   potentially changed it. */
		const char *key = set->key, *value;
		ret = settings_override_get_value(ctx, set, &key,
						  &key_idx, &value, error_r);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			/* setting doesn't exist in this info */
			continue;
		}
		bool track_seen = ctx->info->defines[key_idx].type != SET_FILTER_ARRAY;
		if (ctx->info->defines[key_idx].type == SET_STRLIST ||
		    ctx->info->defines[key_idx].type == SET_BOOLLIST) {
			const char *suffix;
			if (!str_begins(key, ctx->info->defines[key_idx].key, &suffix))
				i_unreached();
			if (suffix[0] != '/') {
				/* replace full boollist setting
				   (invalid for strlist) */
				i_assert(suffix[0] == '\0');
			} else if (settings_parse_list_has_key(ctx->parser,
					key_idx,
					settings_section_unescape(suffix + 1)))
				continue;
			else
				track_seen = FALSE;
		}
		if (track_seen) {
			enum set_seen_type *set_seenp =
				array_idx_get_space(&ctx->set_seen, key_idx);
			switch (*set_seenp) {
			case SET_SEEN_NO:
				break;
			case SET_SEEN_YES:
				/* already set - skip */
				continue;
			case SET_SEEN_DEFAULT:
				/* already seen, but still apply defaults */
				if (set->type != SETTINGS_OVERRIDE_TYPE_DEFAULT)
					continue;
				break;
			}
			if (set->type == SETTINGS_OVERRIDE_TYPE_2ND_DEFAULT)
				*set_seenp = SET_SEEN_DEFAULT;
			else
				*set_seenp = SET_SEEN_YES;
		}
		/* skip this setting the next time */
		*set_elem = NULL;
		if (set->pool == ctx->temp_pool)
			settings_override_free(set);

		if (value != set->value)
			value = p_strdup(&ctx->mpool->pool, value);
		else {
			/* Add explicit reference to instance->pool, which is
			   kept by the settings struct's pool. This allows
			   settings to survive even if the instance is freed.

			   If there is no instance pool, it means there are
			   only CLI_PARAM settings, which are allocated from
			   FIXME: should figure out some efficient way how to
			   store them. */
			if (array_is_created(&ctx->mpool->pool.external_refs))
				i_assert(array_idx_elem(&ctx->mpool->pool.external_refs, 0) == ctx->instance->pool);
			else if (ctx->instance->pool != NULL)
				pool_add_external_ref(&ctx->mpool->pool, ctx->instance->pool);
		}
		if (ctx->info->setting_apply != NULL &&
		    !ctx->info->setting_apply(ctx->event, ctx->set_struct, key,
					      &value, apply_flags, error_r)) {
			*error_r = t_strdup_printf(
				"Failed to override configuration from %s: "
				"Invalid %s=%s: %s",
				settings_override_type_names[set->type],
				key, value, *error_r);
			return -1;
		}
		if (settings_parse_keyidx_value_nodup(ctx->parser, key_idx, key,
						      value) < 0) {
			*error_r = t_strdup_printf(
				"Failed to override configuration from %s: "
				"Invalid %s=%s: %s",
				settings_override_type_names[set->type],
				key, value, settings_parser_get_error(ctx->parser));
			return -1;
		}
	}
	return seen_filter ? 1 : 0;
}

static int
settings_instance_get(struct settings_apply_ctx *ctx,
		      const char *source_filename,
		      unsigned int source_linenum,
		      const void **set_r, const char **error_r)
{
	const char *error;
	bool seen_filter = FALSE;
	int ret = 0;

	i_assert(ctx->info->pool_offset1 != 0);

	*set_r = NULL;

	if (event_find_field_recursive(ctx->event, "protocol") == NULL)
		event_add_str(ctx->event, "protocol", ctx->root->protocol_name);

	ctx->mpool = settings_mmap_pool_create(ctx->root, ctx->instance->mmap,
					       source_filename, source_linenum);
	pool_t set_pool = &ctx->mpool->pool;
	ctx->parser = settings_parser_init(set_pool, ctx->info,
					   SETTINGS_PARSER_FLAG_IGNORE_UNKNOWN_KEYS |
					   SETTINGS_PARSER_FLAG_INSERT_FILTERS);

	/* Set the pool early on before any callbacks are called. */
	ctx->set_struct = settings_parser_get_set(ctx->parser);
	pool_t *pool_p = PTR_OFFSET(ctx->set_struct,
				    ctx->info->pool_offset1 - 1);
	*pool_p = set_pool;

	ctx->str = str_new(default_pool, 256);
	i_array_init(&ctx->set_seen, 64);
	if ((ctx->flags & SETTINGS_GET_FLAG_NO_EXPAND) == 0 &&
	    (ctx->flags & SETTINGS_GET_FLAG_FAKE_EXPAND) == 0)
		settings_var_expand_init(ctx);

	settings_instance_override_init(ctx);

	if (ctx->instance->mmap != NULL) {
		ret = settings_mmap_apply(ctx, &error);
		if (ret < 0) {
			*error_r = t_strdup_printf(
				"Failed to parse configuration: %s", error);
		}
	} else {
		/* No configuration file - apply all overrides */
		ret = settings_instance_override(ctx, NULL, error_r);
	}
	if (ret > 0)
		seen_filter = TRUE;
	settings_apply_ctx_overrides_deinit(ctx);

	if (ret >= 0)
		ret = settings_mmap_apply_defaults(ctx, error_r);
	if (ret < 0) {
		pool_unref(&set_pool);
		return -1;
	}

	if (ctx->filter_key != NULL && !seen_filter &&
	    ctx->filter_name_required) {
		pool_unref(&set_pool);
		return 0;
	}

	if ((ctx->flags & SETTINGS_GET_FLAG_NO_CHECK) == 0) {
		if (!settings_check(ctx->event, ctx->info, *pool_p,
				    ctx->set_struct, error_r)) {
			*error_r = t_strdup_printf("Invalid %s settings: %s",
						   ctx->info->name, *error_r);
			pool_unref(&set_pool);
			return -1;
		}
	}

	*set_r = ctx->set_struct;
	return 1;
}

static int
settings_get_full(struct event *event,
		  const char *filter_key, const char *filter_value,
		  const struct setting_parser_info *info,
		  enum settings_get_flags flags,
		  const char *source_filename,
		  unsigned int source_linenum,
		  const void **set_r, const char **error_r)
{
	struct settings_root *scan_root, *root = NULL;
	struct settings_mmap *mmap = NULL;
	struct settings_instance *scan_instance, *instance = NULL;
	struct event *lookup_event, *scan_event = event;
	const char *str, *filter_name = NULL;
	bool filter_name_required = FALSE;

	lookup_event = event_create(event);
	if (filter_value != NULL) {
		filter_name = t_strdup_printf("%s/%s", filter_key,
			settings_section_escape(filter_value));
		/* the filter key=value field is needed by setting override
		   handling to incrementally generate the filter */
		event_add_str(lookup_event, filter_key, filter_value);
		event_strlist_append(lookup_event, SETTINGS_EVENT_FILTER_NAME,
				     filter_name);
		filter_name_required = TRUE;
	} else if (filter_key != NULL) {
		filter_name = filter_key;
		event_strlist_append(lookup_event, SETTINGS_EVENT_FILTER_NAME,
				     filter_name);
		filter_name_required = TRUE;
	}

	do {
		scan_root = event_get_ptr(scan_event, SETTINGS_EVENT_ROOT);
		scan_instance = event_get_ptr(scan_event,
					      SETTINGS_EVENT_INSTANCE);

		if (root == NULL)
			root = scan_root;
		else if ((scan_root != NULL && root != scan_root) ||
			 (scan_instance != NULL && root != scan_instance->root)) {
			/* settings root changed - ignore the rest of the
			   event hierarchy. */
			break;
		}
		if (instance == NULL && scan_instance != NULL) {
			instance = scan_instance;
			root = instance->root;
		}

		str = event_get_ptr(scan_event, SETTINGS_EVENT_FILTER_NAME);
		if (str != NULL) {
			event_strlist_append(lookup_event,
					     SETTINGS_EVENT_FILTER_NAME, str);
		}
		scan_event = event_get_parent(scan_event);
	} while (scan_event != NULL);

	if (root == NULL)
		i_panic("settings_get() - event has no SETTINGS_EVENT_ROOT");
	if (instance != NULL)
		mmap = instance->mmap;
	else
		mmap = root->mmap;

	/* no instance-specific settings */
	struct settings_instance empty_instance = {
		.mmap = mmap,
	};
	if (instance == NULL)
		instance = &empty_instance;

	struct settings_apply_ctx ctx = {
		.event = lookup_event,
		.root = root,
		.instance = instance,
		.info = info,
		.flags = flags,
		.filter_name = filter_name,
		.filter_key = filter_key,
		.filter_value = filter_value,
		.filter_name_required = filter_name_required,
	};

	int ret;
	T_BEGIN {
		ret = settings_instance_get(&ctx, source_filename, source_linenum,
					    set_r, error_r);
	} T_END_PASS_STR_IF(ret < 0, error_r);
	settings_parser_unref(&ctx.parser);
	event_unref(&lookup_event);
	array_free(&ctx.set_seen);
	str_free(&ctx.str);
	pool_unref(&ctx.temp_pool);
	return ret;
}

#undef settings_get
int settings_get(struct event *event,
		 const struct setting_parser_info *info,
		 enum settings_get_flags flags,
		 const char *source_filename,
		 unsigned int source_linenum,
		 const void **set_r, const char **error_r)
{
	int ret = settings_get_full(event, NULL, NULL, info, flags,
				    source_filename, source_linenum,
				    set_r, error_r);
	i_assert(ret != 0);
	return ret < 0 ? -1 : 0;
}

#undef settings_try_get
int settings_try_get(struct event *event, const char *filter_name,
		     const struct setting_parser_info *info,
		     enum settings_get_flags flags,
		     const char *source_filename,
		     unsigned int source_linenum,
		     const void **set_r, const char **error_r)
{
	return settings_get_full(event, filter_name, NULL, info, flags,
				 source_filename, source_linenum,
				 set_r, error_r);
}

#undef settings_get_filter
int settings_get_filter(struct event *event,
			const char *filter_key, const char *filter_value,
			const struct setting_parser_info *info,
			enum settings_get_flags flags,
			const char *source_filename,
			unsigned int source_linenum,
			const void **set_r, const char **error_r)
{
	i_assert(filter_key != NULL);
	i_assert(filter_value != NULL);

	int ret = settings_get_full(event, filter_key, filter_value, info,
				    flags, source_filename, source_linenum,
				    set_r, error_r);
	if (ret < 0)
		return -1;
	if (ret == 0) {
		/* e.g. namespace=foo was given but no namespace/foo/name */
		*error_r = t_strdup_printf(
			"Filter %s=%s unexpectedly not found "
			"(invalid userdb or -o override settings?)",
			filter_key, filter_value);
		return -1;
	}
	return 0;
}

#undef settings_try_get_filter
int settings_try_get_filter(struct event *event,
			    const char *filter_key, const char *filter_value,
			    const struct setting_parser_info *info,
			    enum settings_get_flags flags,
			    const char *source_filename,
			    unsigned int source_linenum,
			    const void **set_r, const char **error_r)
{
	i_assert(filter_key != NULL);
	i_assert(filter_value != NULL);

	return settings_get_full(event, filter_key, filter_value, info,
				 flags, source_filename, source_linenum,
				 set_r, error_r);
}

#undef settings_get_or_fatal
const void *
settings_get_or_fatal(struct event *event,
		      const struct setting_parser_info *info,
		      const char *source_filename,
		      unsigned int source_linenum)
{
	const void *set;
	const char *error;

	if (settings_get(event, info, 0, source_filename,
			 source_linenum, &set, &error) < 0)
		i_fatal("%s", error);
	return set;
}

void settings_override(struct settings_instance *instance,
		       const char *key, const char *value,
		       enum settings_override_type type)
{
	if (!array_is_created(&instance->overrides))
		p_array_init(&instance->overrides, instance->pool, 16);
	struct settings_override *set =
		array_append_space(&instance->overrides);
	set->pool = instance->pool;
	set->type = type;
	size_t len = strlen(key);
	T_BEGIN {
		if (len > 0 && key[len-1] == '+') {
			/* key+=value */
			set->append = TRUE;
			key = t_strndup(key, len-1);
		}
		set->key = set->orig_key = p_strdup(instance->pool, key);
		set->path_element_count = path_element_count(set->key);
		set->value = p_strdup(instance->pool, value);
	} T_END;
}

void settings_root_override(struct settings_root *root,
			    const char *key, const char *value,
			    enum settings_override_type type)
{
	if (!array_is_created(&root->overrides))
		p_array_init(&root->overrides, root->pool, 16);
	struct settings_override *set =
		array_append_space(&root->overrides);
	set->pool = root->pool;
	set->type = type;
	set->key = set->orig_key = p_strdup(root->pool, key);
	set->path_element_count = path_element_count(set->key);
	set->value = p_strdup(root->pool, value);
}

static struct settings_instance *
settings_instance_alloc(void)
{
	pool_t pool = pool_alloconly_create("settings instance", 1024);
	struct settings_instance *instance =
		p_new(pool, struct settings_instance, 1);
	instance->pool = pool;
	return instance;
}

struct settings_instance *
settings_instance_new(struct settings_root *root)
{
	struct settings_instance *instance = settings_instance_alloc();
	instance->root = root;
	instance->mmap = root->mmap;
	return instance;
}

struct settings_instance *
settings_instance_dup(const struct settings_instance *src)
{
	struct settings_instance *dest = settings_instance_alloc();
	dest->root = src->root;
	dest->mmap = src->mmap;

	if (!array_is_created(&src->overrides))
		return dest;

	p_array_init(&dest->overrides, dest->pool,
		     array_count(&src->overrides) + 8);
	const struct settings_override *src_set;
	array_foreach(&src->overrides, src_set) {
		struct settings_override *dest_set =
			array_append_space(&dest->overrides);
		/* regenerate filter later */
		dest_set->pool = dest->pool;
		dest_set->type = src_set->type;
		dest_set->append = src_set->append;
		dest_set->orig_key = p_strdup(dest->pool, src_set->orig_key);
		dest_set->key = dest_set->orig_key;
		dest_set->value = p_strdup(dest->pool, src_set->value);
	}
	return dest;
}

void settings_instance_free(struct settings_instance **_instance)
{
	struct settings_instance *instance = *_instance;
	struct settings_override *override;

	if (instance == NULL)
		return;

	*_instance = NULL;

	if (array_is_created(&instance->overrides)) {
		array_foreach_modifiable(&instance->overrides, override)
			settings_override_free(override);
	}
	pool_unref(&instance->pool);
}

struct settings_root *settings_root_init(void)
{
	pool_t pool = pool_alloconly_create("settings root", 128);
	struct settings_root *root = p_new(pool, struct settings_root, 1);
	root->pool = pool;
	return root;
}

void settings_root_deinit(struct settings_root **_root)
{
	struct settings_root *root = *_root;
	struct settings_override *override;
	struct settings_mmap_pool *mpool;

	if (root == NULL)
		return;
	*_root = NULL;

	if (array_is_created(&root->overrides)) {
		array_foreach_modifiable(&root->overrides, override)
			settings_override_free(override);
	}
	settings_mmap_unref(&root->mmap);

	for (mpool = root->settings_pools; mpool != NULL; mpool = mpool->next) {
		i_warning("Leaked settings: %s:%u",
			  mpool->source_filename, mpool->source_linenum);
	}
	pool_unref(&root->pool);
}

struct settings_root *settings_root_find(const struct event *event)
{
	struct settings_root *root;

	do {
		root = event_get_ptr(event, SETTINGS_EVENT_ROOT);
		if (root != NULL)
			return root;
		event = event_get_parent(event);
	} while (event != NULL);
	return NULL;
}

static void set_registered_infos_free(void)
{
	array_free(&set_registered_infos);
}

static int
setting_parser_info_cmp(const struct setting_parser_info *info,
			const struct setting_parser_info *const *info2)
{
	return *info2 == info ? 0 : -1;
}

void settings_info_register(const struct setting_parser_info *info)
{
	if (!array_is_created(&set_registered_infos)) {
		i_array_init(&set_registered_infos, 16);
		lib_atexit(set_registered_infos_free);
	}
	if (array_lsearch(&set_registered_infos, info,
			  setting_parser_info_cmp) == NULL)
		array_push_back(&set_registered_infos, &info);
}

struct settings_instance *settings_instance_find(const struct event *event)
{
	struct settings_instance *instance;

	do {
		instance = event_get_ptr(event, SETTINGS_EVENT_INSTANCE);
		if (instance != NULL)
			return instance;
		event = event_get_parent(event);
	} while (event != NULL);
	return NULL;
}

void settings_simple_init(struct settings_simple *set_r,
			  const char *const settings[])
{
	i_zero(set_r);
	set_r->root = settings_root_init();
	set_r->event = event_create(NULL);
	event_set_ptr(set_r->event, SETTINGS_EVENT_ROOT, set_r->root);
	if (settings != NULL)
		settings_simple_update(set_r, settings);
}

void settings_simple_deinit(struct settings_simple *set)
{
	settings_instance_free(&set->instance);
	settings_root_deinit(&set->root);
	event_unref(&set->event);
}

void settings_simple_update(struct settings_simple *set,
			    const char *const settings[])
{
	settings_instance_free(&set->instance);
	set->instance = settings_instance_new(set->root);
	for (unsigned int i = 0; settings[i] != NULL; i += 2) {
		settings_override(set->instance, settings[i], settings[i + 1],
				  SETTINGS_OVERRIDE_TYPE_CODE);
	}
	event_set_ptr(set->event, SETTINGS_EVENT_INSTANCE, set->instance);
}
