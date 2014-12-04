/* Copyright (c) 2006-2014 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/tig.h"
#include "tig/map.h"
#include "tig/argv.h"
#include "tig/io.h"
#include "tig/watch.h"
#include "tig/options.h"
#include "tig/repo.h"
#include "tig/refdb.h"

static struct ref *refs_head = NULL;

DEFINE_STRING_MAP(refs_by_name, struct ref *, name, 32)
DEFINE_STRING_MAP(refs_by_id, struct ref *, id, 16)

int
ref_compare(const struct ref *ref1, const struct ref *ref2)
{
	if (ref1->type != ref2->type)
		return ref1->type - ref2->type;
	return strcmp_numeric(ref1->name, ref2->name);
}

static int
ref_canonical_compare(const struct ref *ref1, const struct ref *ref2)
{
	int tag_diff = !!ref_is_tag(ref2) - !!ref_is_tag(ref1);

	if (tag_diff)
		return tag_diff;
	if (ref1->type != ref2->type)
		return !tag_diff ? ref1->type - ref2->type : ref2->type - ref1->type;
	return strcmp_numeric(ref1->name, ref2->name);
}

void
foreach_ref(bool (*visitor)(void *data, const struct ref *ref), void *data)
{
	string_map_foreach(&refs_by_name, (string_map_iterator_fn) visitor, data);
}

const struct ref *
get_ref_head()
{
	return refs_head;
}

const struct ref *
get_ref_list(const char *id)
{
	return string_map_get(&refs_by_id, id);
}

const struct ref *
get_canonical_ref(const char *id)
{
	const struct ref *ref = NULL;
	const struct ref *pos;

	foreach_ref_list(pos, id)
		if (!ref || ref_canonical_compare(pos, ref) < 0)
			ref = pos;

	return ref;
}

struct ref_opt {
	const char *remote;
	const char *head;
	enum watch_trigger changed;
};

static int
add_to_refs(const char *id, size_t idlen, char *name, size_t namelen, struct ref_opt *opt)
{
	struct ref *ref = NULL;
	enum reference_type type = REFERENCE_BRANCH;
	void **ref_lists_slot;
	void **ref_slot;

	if (!prefixcmp(name, "refs/tags/")) {
		type = REFERENCE_TAG;
		if (!suffixcmp(name, namelen, "^{}")) {
			namelen -= 3;
			name[namelen] = 0;
		} else {
			type = REFERENCE_LOCAL_TAG;
		}

		namelen -= STRING_SIZE("refs/tags/");
		name	+= STRING_SIZE("refs/tags/");

	} else if (!prefixcmp(name, "refs/remotes/")) {
		type = REFERENCE_REMOTE;
		namelen -= STRING_SIZE("refs/remotes/");
		name	+= STRING_SIZE("refs/remotes/");
		if (!strcmp(opt->remote, name))
			type = REFERENCE_TRACKED_REMOTE;

	} else if (!prefixcmp(name, "refs/replace/")) {
		type = REFERENCE_REPLACE;
		id	= name + strlen("refs/replace/");
		idlen	= namelen - strlen("refs/replace/");
		name	= "replaced";
		namelen	= strlen(name);

	} else if (!prefixcmp(name, "refs/heads/")) {
		namelen -= STRING_SIZE("refs/heads/");
		name	+= STRING_SIZE("refs/heads/");
		if (strlen(opt->head) == namelen &&
		    !strncmp(opt->head, name, namelen))
			type = REFERENCE_HEAD;

	} else if (!strcmp(name, "HEAD")) {
		/* Handle the case of HEAD not being a symbolic ref,
		 * i.e. during a rebase. */
		if (*opt->head)
			return OK;
		type = REFERENCE_HEAD;
	}

	/* If we are reloading or it's an annotated tag, replace the
	 * previous SHA1 with the resolved commit id; relies on the fact
	 * git-ls-remote lists the commit id of an annotated tag right
	 * before the commit id it points to. */
	if (type == REFERENCE_REPLACE) {
		ref_slot = string_map_put_to(&refs_by_id, id);
		if (!ref_slot)
			return ERR;
		if (*ref_slot)
			ref = string_map_remove(&refs_by_id, ref_slot);

	} else {
		ref_slot = string_map_put_to(&refs_by_name, name);
		if (!ref_slot)
			return ERR;
		ref = *ref_slot;
	}

	if (!ref) {
		ref = calloc(1, sizeof(*ref) + namelen);
		if (!ref)
			return ERR;
		strncpy(ref->name, name, namelen);
		*ref_slot = ref;
	}

	if (strncmp(ref->id, id, idlen))
		opt->changed |= WATCH_REFS;

	ref->valid = TRUE;
	ref->type = type;
	string_ncopy_do(ref->id, SIZEOF_REV, id, idlen);

	if (type == REFERENCE_HEAD) {
		if (!refs_head ||
		    (refs_head != ref && memcmp(refs_head, ref, sizeof(*ref))))
			opt->changed |= WATCH_HEAD;
		refs_head = ref;
	}

	ref_lists_slot = string_map_put_to(&refs_by_id, id);
	if (!ref_lists_slot)
		return OK;

	ref->next = *ref_lists_slot;
	*ref_lists_slot = ref;

	while (ref->next) {
		struct ref *head = ref->next;

		if (head == ref || ref_compare(ref, head) <= 0)
			break;

		if (*ref_lists_slot == ref)
			*ref_lists_slot = head;
		ref->next = head->next;
		head->next = ref;
	}

	return OK;
}

static int
read_ref(char *id, size_t idlen, char *name, size_t namelen, void *data)
{
	return add_to_refs(id, idlen, name, namelen, data);
}

static bool
invalidate_refs(void *data, void *ref_)
{
	struct ref *ref = ref_;

	ref->valid = 0;
	ref->next = NULL;
	return TRUE;
}

static bool
cleanup_refs(void *data, void *ref_)
{
	struct ref_opt *opt = data;
	struct ref *ref = ref_;

	if (!ref->valid) {
		ref->id[0] = 0;
		opt->changed |= WATCH_REFS;
	}

	return TRUE;
}

static int
reload_refs(bool force)
{
	const char *ls_remote_argv[SIZEOF_ARG] = {
		"git", "ls-remote", repo.git_dir, NULL
	};
	static bool init = FALSE;
	struct ref_opt opt = { repo.remote, repo.head, WATCH_NONE };
	struct repo_info old_repo = repo;

	if (!init) {
		if (!argv_from_env(ls_remote_argv, "TIG_LS_REMOTE"))
			return ERR;
		init = TRUE;
	}

	if (!*repo.git_dir)
		return OK;

	if (force || !*repo.head)
		load_repo_head();

	if (strcmp(old_repo.head, repo.head))
		opt.changed |= WATCH_HEAD;

	refs_head = NULL;
	string_map_clear(&refs_by_id);
	string_map_foreach(&refs_by_name, invalidate_refs, NULL);

	if (io_run_load(ls_remote_argv, "\t", read_ref, &opt) == ERR)
		return ERR;

	string_map_foreach(&refs_by_name, cleanup_refs, &opt);

	if (opt.changed)
		watch_apply(NULL, opt.changed);

	return OK;
}

int
load_refs(bool force)
{
	static bool loaded = FALSE;

	if (!force && loaded)
		return OK;

	loaded = TRUE;
	return reload_refs(force);
}

int
add_ref(const char *id, char *name, const char *remote_name, const char *head)
{
	struct ref_opt opt = { remote_name, head };

	return add_to_refs(id, strlen(id), name, strlen(name), &opt);
}

void
ref_update_env(struct argv_env *env, const struct ref *ref, bool clear)
{
	if (clear)
		env->tag[0] = env->remote[0] = env->branch[0] = 0;

	string_copy_rev(env->commit, ref->id);

	if (ref_is_tag(ref)) {
		string_ncopy(env->tag, ref->name, strlen(ref->name));

	} else if (ref_is_remote(ref)) {
		const char *sep = strchr(ref->name, '/');

		if (!sep)
			return;
		string_ncopy(env->remote, ref->name, sep - ref->name);
		string_ncopy(env->branch, sep + 1, strlen(sep + 1));

	} else if (ref->type == REFERENCE_BRANCH) {
		string_ncopy(env->branch, ref->name, strlen(ref->name));
	}
}

static struct ref_format **ref_formats;

const struct ref_format *
get_ref_format(const struct ref *ref)
{
	static const struct ref_format default_format = { "", "" };

	if (ref_formats) {
		struct ref_format *format = ref_formats[ref->type];

		if (!format && ref_is_tag(ref))
			format = ref_formats[REFERENCE_TAG];
		if (!format && ref_is_remote(ref))
			format = ref_formats[REFERENCE_REMOTE];
		if (!format)
			format = ref_formats[REFERENCE_BRANCH];
		if (format)
			return format;
	}

	return &default_format;
}

static enum status_code
parse_ref_format_arg(const char *arg, const struct enum_map *map)
{
	size_t arglen = strlen(arg);
	const char *pos;

	for (pos = arg; *pos && arglen > 0; pos++, arglen--) {
		enum reference_type type;

		for (type = 0; type < map->size; type++) {
			const struct enum_map_entry *entry = &map->entries[type];
			struct ref_format *format;

			if (arglen < entry->namelen ||
			    string_enum_compare(pos, entry->name, entry->namelen))
				continue;

			format = malloc(sizeof(*format));
			if (!format)
				return ERROR_OUT_OF_MEMORY;
			format->start = strndup(arg, pos - arg);
			format->end = strdup(pos + entry->namelen);
			if (!format->start || !format->end) {
				free((void *) format->start);
				free((void *) format->end);
				free(format);
				return ERROR_OUT_OF_MEMORY;
			}

			ref_formats[type] = format;
			return SUCCESS;
		}
	}

	return error("Unknown ref format: %s", arg);
}

enum status_code
parse_ref_formats(const char *argv[])
{
	const struct enum_map *map = reference_type_map;
	int argc;

	if (!ref_formats) {
		ref_formats = calloc(reference_type_map->size, sizeof(struct ref_format *));
		if (!ref_formats)
			return ERROR_OUT_OF_MEMORY;
	}

	for (argc = 0; argv[argc]; argc++) {
		enum status_code code = parse_ref_format_arg(argv[argc], map);
		if (code != SUCCESS)
			return code;
	}

	return SUCCESS;
}

/* vim: set ts=8 sw=8 noexpandtab: */
