/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "str-parse.h"
#include "settings-parser.h"

struct boollist_removal {
	ARRAY_TYPE(const_string) *array;
	const char *key_suffix;
};

struct setting_parser_context {
	pool_t set_pool, parser_pool;
	int refcount;
        enum settings_parser_flags flags;

	const struct setting_parser_info *info;

	/* Pointer to structure containing the values */
	void *set_struct;
	ARRAY(struct boollist_removal) boollist_removals;

	char *error;
};

const char *set_value_unknown = "UNKNOWN_VALUE_WITH_VARIABLES";

#ifdef DEBUG
static const char *boollist_eol_sentry = "boollist-eol";
#endif

static void
setting_parser_copy_defaults(struct setting_parser_context *ctx,
			     const struct setting_parser_info *info)
{
	const struct setting_define *def;
	const char *p, **strp;

	if (info->defaults == NULL)
		return;

	memcpy(ctx->set_struct, info->defaults, info->struct_size);
	for (def = info->defines; def->key != NULL; def++) {
		switch (def->type) {
		case SET_ENUM: {
			/* fix enums by dropping everything after the
			   first ':' */
			strp = PTR_OFFSET(ctx->set_struct, def->offset);
			p = strchr(*strp, ':');
			if (p != NULL)
				*strp = p_strdup_until(ctx->set_pool, *strp, p);
			break;
		}
		default:
			break;
		}
	}
}

struct setting_parser_context *
settings_parser_init(pool_t set_pool, const struct setting_parser_info *root,
		     enum settings_parser_flags flags)
{
	struct setting_parser_context *ctx;
	pool_t parser_pool;

	parser_pool = pool_alloconly_create(MEMPOOL_GROWING"settings parser",
					    1024);
	ctx = p_new(parser_pool, struct setting_parser_context, 1);
	ctx->refcount = 1;
	ctx->set_pool = set_pool;
	ctx->parser_pool = parser_pool;
	ctx->flags = flags;

	ctx->info = root;
	if (root->struct_size > 0) {
		ctx->set_struct =
			p_malloc(ctx->set_pool, root->struct_size);
		setting_parser_copy_defaults(ctx, root);
	}

	pool_ref(ctx->set_pool);
	return ctx;
}

void settings_parser_ref(struct setting_parser_context *ctx)
{
	i_assert(ctx->refcount > 0);
	ctx->refcount++;
}

void settings_parser_unref(struct setting_parser_context **_ctx)
{
	struct setting_parser_context *ctx = *_ctx;

	if (ctx == NULL)
		return;
	*_ctx = NULL;

	i_assert(ctx->refcount > 0);
	if (--ctx->refcount > 0)
		return;
	i_free(ctx->error);
	pool_unref(&ctx->set_pool);
	pool_unref(&ctx->parser_pool);
}

unsigned int
setting_parser_info_get_define_count(const struct setting_parser_info *info)
{
	unsigned int count = 0;
	while (info->defines[count].key != NULL)
		count++;
	return count;
}

bool setting_parser_info_find_key(const struct setting_parser_info *info,
				  const char *key, unsigned int *idx_r)
{
	const char *suffix;

	for (unsigned int i = 0; info->defines[i].key != NULL; i++) {
		if (!str_begins(key, info->defines[i].key, &suffix))
			; /* mismatch */
		else if (suffix[0] == '\0') {
			/* full setting */
			while (i > 0 && info->defines[i].type == SET_ALIAS)
				i--;
			*idx_r = i;
			return TRUE;
		} else if (suffix[0] == '/' &&
			   (info->defines[i].type == SET_STRLIST ||
			    info->defines[i].type == SET_BOOLLIST)) {
			/* strlist key */
			*idx_r = i;
			return TRUE;
		}
	}
	return FALSE;
}

void *settings_parser_get_set(const struct setting_parser_context *ctx)
{
	return ctx->set_struct;
}

static void settings_parser_set_error(struct setting_parser_context *ctx,
				      const char *error)
{
	i_free(ctx->error);
	ctx->error = i_strdup(error);
}

const char *settings_parser_get_error(struct setting_parser_context *ctx)
{
	return ctx->error;
}

static const struct setting_define *
setting_define_find(const struct setting_parser_info *info, const char *key)
{
	const struct setting_define *list;

	for (list = info->defines; list->key != NULL; list++) {
		if (strcmp(list->key, key) == 0)
			return list;
	}
	return NULL;
}

static int
get_bool(struct setting_parser_context *ctx, const char *value, bool *result_r)
{
	const char *error;
	int ret;
	if ((ret = str_parse_get_bool(value, result_r, &error)) < 0)
		settings_parser_set_error(ctx, error);
	return ret;
}

static int
get_uint(struct setting_parser_context *ctx, const char *value,
	 unsigned int *result_r)
{
	if (settings_value_is_unlimited(value)) {
		*result_r = SET_UINT_UNLIMITED;
		return 0;
	}
	if (str_to_uint(value, result_r) < 0) {
		settings_parser_set_error(ctx, t_strdup_printf(
			"Invalid number %s: %s", value,
			str_num_error(value)));
		return -1;
	}
	return 0;
}

static int
get_octal(struct setting_parser_context *ctx, const char *value,
	  unsigned int *result_r)
{
	unsigned long long octal;

	if (*value != '0')
		return get_uint(ctx, value, result_r);

	if (str_to_ullong_oct(value, &octal) < 0) {
		settings_parser_set_error(ctx,
			t_strconcat("Invalid number: ", value, NULL));
		return -1;
	}
	*result_r = (unsigned int)octal;
	return 0;
}

static int get_enum(struct setting_parser_context *ctx, const char *value,
		    char **result_r, const char *allowed_values)
{
	const char *p;

	while (allowed_values != NULL) {
		p = strchr(allowed_values, ':');
		if (p == NULL) {
			if (strcmp(allowed_values, value) == 0)
				break;

			settings_parser_set_error(ctx,
				t_strconcat("Invalid value: ", value, NULL));
			return -1;
		}

		if (strncmp(allowed_values, value, p - allowed_values) == 0 &&
		    value[p - allowed_values] == '\0')
			break;

		allowed_values = p + 1;
	}

	*result_r = p_strdup(ctx->set_pool, value);
	return 0;
}

static int
get_in_port_zero(struct setting_parser_context *ctx, const char *value,
	 in_port_t *result_r)
{
	if (net_str2port_zero(value, result_r) < 0) {
		settings_parser_set_error(ctx, t_strdup_printf(
			"Invalid port number %s", value));
		return -1;
	}
	return 0;
}

static void
settings_parse_strlist(struct setting_parser_context *ctx,
		       ARRAY_TYPE(const_string) *array,
		       const char *key, const char *value)
{
	const char *const *items;
	const char *vkey, *vvalue;
	unsigned int i, count;

	key = strchr(key, SETTINGS_SEPARATOR);
	if (key == NULL)
		return;
	key = settings_section_unescape(key + 1);
	vvalue = p_strdup(ctx->set_pool, value);

	if (!array_is_created(array))
		p_array_init(array, ctx->set_pool, 4);

	/* replace if it already exists */
	items = array_get(array, &count);
	for (i = 0; i < count; i += 2) {
		if (strcmp(items[i], key) == 0) {
			array_idx_set(array, i + 1, &vvalue);
			return;
		}
	}

	vkey = p_strdup(ctx->set_pool, key);
	array_push_back(array, &vkey);
	array_push_back(array, &vvalue);
}

int settings_parse_boollist_string(const char *value, pool_t pool,
				   ARRAY_TYPE(const_string) *dest,
				   const char **error_r)
{
	string_t *elem = t_str_new(32);
	const char *elem_dup;
	bool quoted = FALSE, end_of_quote = FALSE;
	for (unsigned int i = 0; value[i] != '\0'; i++) {
		switch (value[i]) {
		case '"':
			if (!quoted) {
				/* beginning of a string */
				if (str_len(elem) != 0) {
					*error_r = "'\"' in the middle of a string";
					return -1;
				}
				quoted = TRUE;
			} else if (end_of_quote) {
				*error_r = "Expected ',' or ' ' after '\"'";
				return -1;
			} else {
				/* end of a string */
				end_of_quote = TRUE;
			}
			break;
		case ' ':
		case ',':
			if (quoted && !end_of_quote) {
				/* inside a "quoted string" */
				str_append_c(elem, value[i]);
				break;
			}

			if (quoted || str_len(elem) > 0) {
				elem_dup = p_strdup(pool,
					settings_section_unescape(str_c(elem)));
				array_push_back(dest, &elem_dup);
				str_truncate(elem, 0);
			}
			quoted = FALSE;
			end_of_quote = FALSE;
			break;
		case '\\':
			if (quoted) {
				i++;
				if (value[i] == '\0') {
					*error_r = "Value ends with '\\'";
					return -1;
				}
			}
			/* fall through */
		default:
			if (end_of_quote) {
				*error_r = "Expected ',' or ' ' after '\"'";
				return -1;
			}
			str_append_c(elem, value[i]);
			break;
		}
	}
	if (quoted && !end_of_quote) {
		*error_r = "Missing ending '\"'";
		return -1;
	}
	if (quoted || str_len(elem) > 0) {
		elem_dup = settings_section_unescape(p_strdup(pool, str_c(elem)));
		array_push_back(dest, &elem_dup);
	}
	return 0;
}

const char *const *settings_boollist_get(const ARRAY_TYPE(const_string) *array)
{
	const char *const *strings = empty_str_array;
	unsigned int count;

	if (array_not_empty(array)) {
		strings = array_get(array, &count);
		i_assert(strings[count] == NULL);
#ifdef DEBUG
		i_assert(strings[count+1] == boollist_eol_sentry);
#endif
	}
	return strings;

}

static void boollist_null_terminate(ARRAY_TYPE(const_string) *array)
{
	array_append_zero(array);
#ifdef DEBUG
	array_push_back(array, &boollist_eol_sentry);
	array_pop_back(array);
#endif
	array_pop_back(array);
}

static int
settings_parse_boollist(struct setting_parser_context *ctx,
			ARRAY_TYPE(const_string) *array,
			const char *key, const char *value)
{
	const char *const *elem, *error;

	if (!array_is_created(array))
		p_array_init(array, ctx->set_pool, 5);

	key = strrchr(key, SETTINGS_SEPARATOR);
	if (key == NULL) {
		/* replace the whole boollist */
		array_clear(array);
		if (settings_parse_boollist_string(value, ctx->set_pool,
						   array, &error) < 0) {
			settings_parser_set_error(ctx, error);
			return -1;
		}
		/* keep it NULL-terminated for each access */
		boollist_null_terminate(array);
		return 0;
	}
	key = settings_section_unescape(key + 1);

	bool value_bool;
	if (get_bool(ctx, value, &value_bool) < 0)
		return -1;

	elem = array_lsearch(array, &key, i_strcmp_p);
	if (elem == NULL && value_bool) {
		/* add missing element */
		key = p_strdup(ctx->set_pool, key);
		array_push_back(array, &key);
	} else if (!value_bool) {
		/* remove unwanted element */
		if (elem != NULL) {
			key = *elem;
			array_delete(array, array_ptr_to_idx(array, elem), 1);
		} else {
			key = p_strdup(ctx->parser_pool, key);
		}
		/* remember the removal for settings_parse_list_has_key() */
		if (!array_is_created(&ctx->boollist_removals))
			p_array_init(&ctx->boollist_removals, ctx->parser_pool, 2);
		struct boollist_removal *removal =
			array_append_space(&ctx->boollist_removals);
		removal->array = array;
		removal->key_suffix = key;
	}
	/* keep it NULL-terminated for each access */
	boollist_null_terminate(array);
	return 0;
}

static int
settings_parse(struct setting_parser_context *ctx,
	       const struct setting_define *def,
	       const char *key, const char *value, bool dup_value)
{
	void *ptr;
	const void *ptr2;
	const char *error;
	int ret;

	if (value == set_value_unknown) {
		/* setting value is unknown - preserve the exact pointer */
		dup_value = FALSE;
	}

	i_free(ctx->error);

	while (def->type == SET_ALIAS) {
		i_assert(def != ctx->info->defines);
		def--;
	}

	ptr = PTR_OFFSET(ctx->set_struct, def->offset);
	switch (def->type) {
	case SET_BOOL:
		if (get_bool(ctx, value, (bool *)ptr) < 0)
			return -1;
		break;
	case SET_UINT:
		if (get_uint(ctx, value, (unsigned int *)ptr) < 0)
			return -1;
		break;
	case SET_UINT_OCT:
		if (get_octal(ctx, value, (unsigned int *)ptr) < 0)
			return -1;
		break;
	case SET_TIME:
		if (settings_value_is_unlimited(value)) {
			*(unsigned int *)ptr = SET_TIME_INFINITE;
			return 0;
		}
		if (str_parse_get_interval(value, (unsigned int *)ptr, &error) < 0) {
			settings_parser_set_error(ctx, error);
			return -1;
		}
		break;
	case SET_TIME_MSECS:
		if (settings_value_is_unlimited(value)) {
			*(unsigned int *)ptr = SET_TIME_MSECS_INFINITE;
			return 0;
		}
		if (str_parse_get_interval_msecs(value, (unsigned int *)ptr, &error) < 0) {
			settings_parser_set_error(ctx, error);
			return -1;
		}
		break;
	case SET_SIZE:
		if (settings_value_is_unlimited(value)) {
			*(uoff_t *)ptr = SET_SIZE_UNLIMITED;
			return 0;
		}
		if (str_parse_get_size(value, (uoff_t *)ptr, &error) < 0) {
			settings_parser_set_error(ctx, error);
			return -1;
		}
		break;
	case SET_IN_PORT:
		if (get_in_port_zero(ctx, value, (in_port_t *)ptr) < 0)
			return -1;
		break;
	case SET_STR:
	case SET_STR_NOVARS:
		if (dup_value)
			value = p_strdup(ctx->set_pool, value);
		*((const char **)ptr) = value;
		break;
	case SET_ENUM:
		/* get the available values from default string */
		i_assert(ctx->info->defaults != NULL);
		ptr2 = CONST_PTR_OFFSET(ctx->info->defaults, def->offset);
		if (get_enum(ctx, value, (char **)ptr,
			     *(const char *const *)ptr2) < 0)
			return -1;
		break;
	case SET_STRLIST:
		T_BEGIN {
			settings_parse_strlist(ctx, ptr, key, value);
		} T_END;
		break;
	case SET_BOOLLIST:
		T_BEGIN {
			ret = settings_parse_boollist(ctx, ptr, key, value);
		} T_END;
		if (ret < 0)
			return -1;
		break;
	case SET_FILTER_ARRAY: {
		/* Add filter names to the array. Userdb can add more simply
		   by giving e.g. "namespace=newname" without it removing the
		   existing ones. */
		ARRAY_TYPE(const_string) *arr = ptr;
		const char *const *list = t_strsplit(value, ",\t ");
		unsigned int i, count = str_array_length(list);
		if (!array_is_created(arr))
			p_array_init(arr, ctx->set_pool, count);
		unsigned int insert_pos = 0;
		for (i = 0; i < count; i++) {
			const char *value = p_strdup(ctx->set_pool,
				settings_section_unescape(list[i]));
			if ((ctx->flags & SETTINGS_PARSER_FLAG_INSERT_FILTERS) != 0)
				array_insert(arr, insert_pos++, &value, 1);
			else
				array_push_back(arr, &value);
		}
		break;
	}
	case SET_FILTER_NAME:
	case SET_FILTER_HIERARCHY:
		settings_parser_set_error(ctx, t_strdup_printf(
			"Setting is a named filter, use '%s {'", key));
		return -1;
	case SET_ALIAS:
		i_unreached();
	}
	return 0;
}

static bool
settings_find_key(struct setting_parser_context *ctx, const char *key,
		  bool allow_filter_name, const struct setting_define **def_r)
{
	const struct setting_define *def;
	const char *end, *parent_key;

	/* try to find the exact key */
	def = setting_define_find(ctx->info, key);
	if (def != NULL && ((def->type != SET_FILTER_NAME &&
			     def->type != SET_FILTER_HIERARCHY) ||
			    allow_filter_name)) {
		*def_r = def;
		return TRUE;
	}

	/* try to find list/key prefix */
	end = strrchr(key, SETTINGS_SEPARATOR);
	if (end == NULL)
		return FALSE;

	parent_key = t_strdup_until(key, end);
	def = setting_define_find(ctx->info, parent_key);
	if (def != NULL && (def->type == SET_STRLIST ||
			    def->type == SET_BOOLLIST)) {
		*def_r = def;
		return TRUE;
	}
	return FALSE;
}

static int
settings_parse_keyvalue_real(struct setting_parser_context *ctx,
			     const char *key, const char *value, bool dup_value)
{
	const struct setting_define *def;

	if (!settings_find_key(ctx, key, FALSE, &def)) {
		settings_parser_set_error(ctx,
			t_strconcat("Unknown setting: ", key, NULL));
		return 0;
	}

	if (settings_parse(ctx, def, key, value, dup_value) < 0)
		return -1;
	return 1;
}

int settings_parse_keyvalue(struct setting_parser_context *ctx,
			    const char *key, const char *value)
{
	return settings_parse_keyvalue_real(ctx, key, value, TRUE);
}

int settings_parse_keyidx_value(struct setting_parser_context *ctx,
				unsigned int key_idx, const char *key,
				const char *value)
{
	return settings_parse(ctx, &ctx->info->defines[key_idx],
			      key, value, TRUE);
}

int settings_parse_keyvalue_nodup(struct setting_parser_context *ctx,
				  const char *key, const char *value)
{
	return settings_parse_keyvalue_real(ctx, key, value, FALSE);
}

int settings_parse_keyidx_value_nodup(struct setting_parser_context *ctx,
				      unsigned int key_idx, const char *key,
				      const char *value)
{
	return settings_parse(ctx, &ctx->info->defines[key_idx],
			      key, value, FALSE);
}

static int boollist_removal_cmp(const struct boollist_removal *r1,
				const struct boollist_removal *r2)
{
	if (r1->array != r2->array)
		return 1;
	return strcmp(r1->key_suffix, r2->key_suffix);
}

bool settings_parse_list_has_key(struct setting_parser_context *ctx,
				 unsigned int key_idx,
				 const char *key_suffix)
{
	const struct setting_define *def = &ctx->info->defines[key_idx];
	unsigned int skip = UINT_MAX;

	switch (def->type) {
	case SET_STRLIST:
		skip = 2;
		break;
	case SET_BOOLLIST:
		skip = 1;
		if (!array_is_created(&ctx->boollist_removals))
			break;

		struct boollist_removal lookup = {
			.array = PTR_OFFSET(ctx->set_struct, def->offset),
			.key_suffix = key_suffix,
		};
		if (array_lsearch(&ctx->boollist_removals, &lookup,
				  boollist_removal_cmp) != NULL)
			return TRUE;
		break;
	default:
		i_unreached();
	}

	ARRAY_TYPE(const_string) *array =
		PTR_OFFSET(ctx->set_struct, def->offset);
	if (!array_is_created(array))
		return FALSE;

	unsigned int i, count;
	const char *const *items = array_get(array, &count);
	for (i = 0; i < count; i += skip) {
		if (strcmp(items[i], key_suffix) == 0)
			return TRUE;
	}
	return FALSE;
}

const void *
settings_parse_get_value(struct setting_parser_context *ctx,
			 const char **key, enum setting_type *type_r)
{
	const struct setting_define *def;

	if (!settings_find_key(ctx, *key, TRUE, &def))
		return NULL;

	while (def->type == SET_ALIAS) {
		i_assert(def != ctx->info->defines);
		def--;
		/* Replace the key with the unaliased key. We assume here that
		   lists don't have aliases, because the key replacement
		   would only need to replace the key prefix then. */
		i_assert(def->type != SET_STRLIST && def->type != SET_BOOLLIST);
		*key = def->key;
	}
	*type_r = def->type;
	return PTR_OFFSET(ctx->set_struct, def->offset);
}

bool settings_check(struct event *event, const struct setting_parser_info *info,
		    pool_t pool, void *set, const char **error_r)
{
	bool valid;

	if (info->check_func != NULL) {
		T_BEGIN {
			valid = info->check_func(set, pool, error_r);
		} T_END_PASS_STR_IF(!valid, error_r);
		if (!valid)
			return FALSE;
	}
	if (info->ext_check_func != NULL) {
		T_BEGIN {
			valid = info->ext_check_func(event, set, pool, error_r);
		} T_END_PASS_STR_IF(!valid, error_r);
		if (!valid)
			return FALSE;
	}
	return TRUE;
}

bool settings_parser_check(struct setting_parser_context *ctx, pool_t pool,
			   struct event *event, const char **error_r)
{
	return settings_check(event, ctx->info, pool,
			      ctx->set_struct, error_r);
}

const char *settings_section_escape(const char *name)
{
#define CHAR_NEED_ESCAPE(c) \
	((c) == '=' || (c) == SETTINGS_SEPARATOR || (c) == '\\' || (c) == ' ' || (c) == ',')
	string_t *str;
	unsigned int i;

	for (i = 0; name[i] != '\0'; i++) {
		if (CHAR_NEED_ESCAPE(name[i]))
			break;
	}
	if (name[i] == '\0') {
		if (i == 0)
			return "\\.";
		return name;
	}

	str = t_str_new(i + strlen(name+i) + 8);
	str_append_data(str, name, i);
	for (; name[i] != '\0'; i++) {
		switch (name[i]) {
		case '=':
			str_append(str, "\\e");
			break;
		case SETTINGS_SEPARATOR:
			str_append(str, "\\s");
			break;
		case '\\':
			str_append(str, "\\\\");
			break;
		case ' ':
			str_append(str, "\\_");
			break;
		case ',':
			str_append(str, "\\+");
			break;
		default:
			str_append_c(str, name[i]);
			break;
		}
	}
	return str_c(str);
}

const char *settings_section_unescape(const char *name)
{
	const char *p = strchr(name, '\\');
	if (p == NULL)
		return name;

	string_t *str = t_str_new(strlen(name));
	str_append_data(str, name, p - name);
	while (p[1] != '\0') {
		switch (p[1]) {
		case 'e':
			str_append_c(str, '=');
			break;
		case 's':
			str_append_c(str, SETTINGS_SEPARATOR);
			break;
		case '\\':
			str_append_c(str, '\\');
			break;
		case '_':
			str_append_c(str, ' ');
			break;
		case '+':
			str_append_c(str, ',');
			break;
		case '.':
			/* empty string */
			break;
		default:
			/* not supposed to happen */
			str_append_c(str, '\\');
			str_append_c(str, p[1]);
			break;
		}
		name = p+2;
		p = strchr(name, '\\');
		if (p == NULL) {
			str_append(str, name);
			return str_c(str);
		}
		str_append_data(str, name, p - name);
	}
	/* ends with '\\' - not supposed to happen */
	str_append_c(str, '\\');
	return str_c(str);
}

static bool config_binary = FALSE;

bool is_config_binary(void)
{
	return config_binary;
}

void set_config_binary(bool value)
{
	config_binary = value;
}

