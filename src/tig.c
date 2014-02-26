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

#define WARN_MISSING_CURSES_CONFIGURATION

#include "tig.h"
#include "types.h"
#include "util.h"
#include "parse.h"
#include "io.h"
#include "refs.h"
#include "graph.h"
#include "git.h"
#include "request.h"
#include "line.h"
#include "keys.h"
#include "view.h"
#include "repo.h"
#include "options.h"
#include "draw.h"
#include "display.h"

static bool
forward_request_to_child(struct view *child, enum request request)
{
	return displayed_views() == 2 && view_is_displayed(child) &&
		!strcmp(child->vid, child->ops->id);
}

static enum request
view_request(struct view *view, enum request request)
{
	if (!view || !view->lines)
		return request;

	if (request == REQ_ENTER && !opt_focus_child &&
	    view_has_flags(view, VIEW_SEND_CHILD_ENTER)) {
		struct view *child = display[1];

	    	if (forward_request_to_child(child, request)) {
			view_request(child, request);
			return REQ_NONE;
		}
	}

	if (request == REQ_REFRESH && view->unrefreshable) {
		report("This view can not be refreshed");
		return REQ_NONE;
	}

	return view->ops->request(view, request, &view->line[view->pos.lineno]);
}

/*
 * Option management
 */

#define VIEW_FLAG_RESET_DISPLAY	((enum view_flag) -1)

#define TOGGLE_MENU_INFO(_) \
	_(LINENO,    '.', "line numbers",      &opt_show_line_numbers, NULL, VIEW_NO_FLAGS), \
	_(DATE,      'D', "dates",             &opt_show_date, date_map, VIEW_NO_FLAGS), \
	_(AUTHOR,    'A', "author",            &opt_show_author, author_map, VIEW_NO_FLAGS), \
	_(GRAPHIC,   '~', "graphics",          &opt_line_graphics, graphic_map, VIEW_NO_FLAGS), \
	_(REV_GRAPH, 'g', "revision graph",    &opt_show_rev_graph, NULL, VIEW_LOG_LIKE), \
	_(FILENAME,  '#', "file names",        &opt_show_filename, filename_map, VIEW_NO_FLAGS), \
	_(FILE_SIZE, '*', "file sizes",        &opt_show_file_size, file_size_map, VIEW_NO_FLAGS), \
	_(IGNORE_SPACE, 'W', "space changes",  &opt_ignore_space, ignore_space_map, VIEW_DIFF_LIKE), \
	_(COMMIT_ORDER, 'l', "commit order",   &opt_commit_order, commit_order_map, VIEW_LOG_LIKE), \
	_(REFS,      'F', "reference display", &opt_show_refs, NULL, VIEW_NO_FLAGS), \
	_(CHANGES,   'C', "local change display", &opt_show_changes, NULL, VIEW_NO_FLAGS), \
	_(ID,        'X', "commit ID display", &opt_show_id, NULL, VIEW_NO_FLAGS), \
	_(FILES,     '%', "file filtering",    &opt_file_filter, NULL, VIEW_DIFF_LIKE | VIEW_LOG_LIKE), \
	_(TITLE_OVERFLOW, '$', "commit title overflow display", &opt_title_overflow, NULL, VIEW_NO_FLAGS), \
	_(UNTRACKED_DIRS, 'd', "untracked directory info", &opt_status_untracked_dirs, NULL, VIEW_STATUS_LIKE), \
	_(VERTICAL_SPLIT, '|', "view split",   &opt_vertical_split, vertical_split_map, VIEW_FLAG_RESET_DISPLAY), \

static enum view_flag
toggle_option(struct view *view, enum request request, char msg[SIZEOF_STR])
{
	const struct {
		enum request request;
		const struct enum_map *map;
		enum view_flag reload_flags;
	} data[] = {
#define DEFINE_TOGGLE_DATA(id, key, help, value, map, vflags) { REQ_TOGGLE_ ## id, map, vflags  }
		TOGGLE_MENU_INFO(DEFINE_TOGGLE_DATA)
	};
	const struct menu_item menu[] = {
#define DEFINE_TOGGLE_MENU(id, key, help, value, map, vflags) { key, help, value }
		TOGGLE_MENU_INFO(DEFINE_TOGGLE_MENU)
		{ 0 }
	};
	int i = 0;

	if (request == REQ_OPTIONS) {
		if (!prompt_menu("Toggle option", menu, &i))
			return VIEW_NO_FLAGS;
	} else {
		while (i < ARRAY_SIZE(data) && data[i].request != request)
			i++;
		if (i >= ARRAY_SIZE(data))
			die("Invalid request (%d)", request);
	}

	if (data[i].map != NULL) {
		unsigned int *opt = menu[i].data;

		*opt = (*opt + 1) % data[i].map->size;
		if (data[i].map == ignore_space_map) {
			update_ignore_space_arg();
			string_format_size(msg, SIZEOF_STR,
				"Ignoring %s %s", enum_name(data[i].map->entries[*opt]), menu[i].text);

		} else if (data[i].map == commit_order_map) {
			update_commit_order_arg();
			string_format_size(msg, SIZEOF_STR,
				"Using %s %s", enum_name(data[i].map->entries[*opt]), menu[i].text);

		} else {
			string_format_size(msg, SIZEOF_STR,
				"Displaying %s %s", enum_name(data[i].map->entries[*opt]), menu[i].text);
		}

	} else if (menu[i].data == &opt_title_overflow) {
		int *option = menu[i].data;

		*option = *option ? -*option : 50;
		string_format_size(msg, SIZEOF_STR,
			"%sabling %s", *option > 0 ? "En" : "Dis", menu[i].text);

	} else {
		bool *option = menu[i].data;

		*option = !*option;
		string_format_size(msg, SIZEOF_STR,
			"%sabling %s", *option ? "En" : "Dis", menu[i].text);
	}

	return data[i].reload_flags;
}


/*
 * View opening
 */

static void
open_mergetool(const char *file)
{
	const char *mergetool_argv[] = { "git", "mergetool", file, NULL };

	open_external_viewer(mergetool_argv, repo.cdup, TRUE, "");
}

static enum request run_prompt_command(struct view *view, char *cmd);

static enum request
open_run_request(struct view *view, enum request request)
{
	struct run_request *req = get_run_request(request);
	const char **argv = NULL;
	bool confirmed = FALSE;

	request = REQ_NONE;

	if (!req) {
		report("Unknown run request");
		return request;
	}

	if (format_argv(view, &argv, req->argv, FALSE, TRUE)) {
		if (req->internal) {
			char cmd[SIZEOF_STR];

			if (argv_to_string(argv, cmd, sizeof(cmd), " ")) {
				request = run_prompt_command(view, cmd);
			}
		}
		else {
			confirmed = !req->confirm;

			if (req->confirm) {
				char cmd[SIZEOF_STR], prompt[SIZEOF_STR];
				const char *and_exit = req->exit ? " and exit" : "";

				if (argv_to_string(argv, cmd, sizeof(cmd), " ") &&
				    string_format(prompt, "Run `%s`%s?", cmd, and_exit) &&
				    prompt_yesno(prompt)) {
					confirmed = TRUE;
				}
			}

			if (confirmed && argv_remove_quotes(argv)) {
				if (req->silent)
					io_run_bg(argv);
				else
					open_external_viewer(argv, NULL, !req->exit, "");
			}
		}
	}

	if (argv)
		argv_free(argv);
	free(argv);

	if (request == REQ_NONE) {
		if (req->confirm && !confirmed)
			request = REQ_NONE;

		else if (req->exit)
			request = REQ_QUIT;

		else if (view_has_flags(view, VIEW_REFRESH) && !view->unrefreshable)
			request = REQ_REFRESH;
	}
	return request;
}

/*
 * User request switch noodle
 */

static int
view_driver(struct view *view, enum request request)
{
	int i;

	if (request == REQ_NONE)
		return TRUE;

	if (request >= REQ_RUN_REQUESTS) {
		request = open_run_request(view, request);

		// exit quickly rather than going through view_request and back
		if (request == REQ_QUIT)
			return FALSE;
	}

	request = view_request(view, request);
	if (request == REQ_NONE)
		return TRUE;

	switch (request) {
	case REQ_MOVE_UP:
	case REQ_MOVE_DOWN:
	case REQ_MOVE_PAGE_UP:
	case REQ_MOVE_PAGE_DOWN:
	case REQ_MOVE_FIRST_LINE:
	case REQ_MOVE_LAST_LINE:
		move_view(view, request);
		break;

	case REQ_SCROLL_FIRST_COL:
	case REQ_SCROLL_LEFT:
	case REQ_SCROLL_RIGHT:
	case REQ_SCROLL_LINE_DOWN:
	case REQ_SCROLL_LINE_UP:
	case REQ_SCROLL_PAGE_DOWN:
	case REQ_SCROLL_PAGE_UP:
	case REQ_SCROLL_WHEEL_DOWN:
	case REQ_SCROLL_WHEEL_UP:
		scroll_view(view, request);
		break;

	case REQ_VIEW_MAIN:
	case REQ_VIEW_DIFF:
	case REQ_VIEW_LOG:
	case REQ_VIEW_TREE:
	case REQ_VIEW_HELP:
	case REQ_VIEW_BRANCH:
	case REQ_VIEW_BLAME:
	case REQ_VIEW_BLOB:
	case REQ_VIEW_STATUS:
	case REQ_VIEW_STAGE:
	case REQ_VIEW_PAGER:
	case REQ_VIEW_STASH:
		open_view(view, request, OPEN_DEFAULT);
		break;

	case REQ_NEXT:
	case REQ_PREVIOUS:
		if (view->parent) {
			int line;

			view = view->parent;
			line = view->pos.lineno;
			view_request(view, request);
			move_view(view, request);
			if (view_is_displayed(view))
				update_view_title(view);
			if (line != view->pos.lineno)
				view_request(view, REQ_ENTER);
		} else {
			move_view(view, request);
		}
		break;

	case REQ_VIEW_NEXT:
	{
		int nviews = displayed_views();
		int next_view = (current_view + 1) % nviews;

		if (next_view == current_view) {
			report("Only one view is displayed");
			break;
		}

		current_view = next_view;
		/* Blur out the title of the previous view. */
		update_view_title(view);
		report_clear();
		break;
	}
	case REQ_REFRESH:
		report("Refreshing is not supported by the %s view", view->name);
		break;

	case REQ_PARENT:
		report("Moving to parent is not supported by the the %s view", view->name);
		break;

	case REQ_BACK:
		report("Going back is not supported for by %s view", view->name);
		break;

	case REQ_MAXIMIZE:
		if (displayed_views() == 2)
			maximize_view(view, TRUE);
		break;

	case REQ_OPTIONS:
	case REQ_TOGGLE_LINENO:
	case REQ_TOGGLE_DATE:
	case REQ_TOGGLE_AUTHOR:
	case REQ_TOGGLE_FILENAME:
	case REQ_TOGGLE_GRAPHIC:
	case REQ_TOGGLE_REV_GRAPH:
	case REQ_TOGGLE_REFS:
	case REQ_TOGGLE_CHANGES:
	case REQ_TOGGLE_IGNORE_SPACE:
	case REQ_TOGGLE_ID:
	case REQ_TOGGLE_FILES:
	case REQ_TOGGLE_TITLE_OVERFLOW:
	case REQ_TOGGLE_FILE_SIZE:
	case REQ_TOGGLE_UNTRACKED_DIRS:
	case REQ_TOGGLE_VERTICAL_SPLIT:
		{
			char action[SIZEOF_STR] = "";
			enum view_flag flags = toggle_option(view, request, action);
	
			if (flags == VIEW_FLAG_RESET_DISPLAY) {
				resize_display();
				redraw_display(TRUE);
			} else {
				foreach_displayed_view(view, i) {
					if (view_has_flags(view, flags) && !view->unrefreshable)
						reload_view(view);
					else
						redraw_view(view);
				}
			}

			if (*action)
				report("%s", action);
		}
		break;

	case REQ_TOGGLE_SORT_FIELD:
	case REQ_TOGGLE_SORT_ORDER:
		report("Sorting is not yet supported for the %s view", view->name);
		break;

	case REQ_DIFF_CONTEXT_UP:
	case REQ_DIFF_CONTEXT_DOWN:
		report("Changing the diff context is not yet supported for the %s view", view->name);
		break;

	case REQ_SEARCH:
	case REQ_SEARCH_BACK:
		search_view(view, request);
		break;

	case REQ_FIND_NEXT:
	case REQ_FIND_PREV:
		find_next(view, request);
		break;

	case REQ_STOP_LOADING:
		foreach_view(view, i) {
			if (view->pipe)
				report("Stopped loading the %s view", view->name),
			end_update(view, TRUE);
		}
		break;

	case REQ_SHOW_VERSION:
		report("tig-%s (built %s)", TIG_VERSION, __DATE__);
		return TRUE;

	case REQ_SCREEN_REDRAW:
		redraw_display(TRUE);
		break;

	case REQ_EDIT:
		report("Nothing to edit");
		break;

	case REQ_ENTER:
		report("Nothing to enter");
		break;

	case REQ_VIEW_CLOSE:
		/* XXX: Mark closed views by letting view->prev point to the
		 * view itself. Parents to closed view should never be
		 * followed. */
		if (view->prev && view->prev != view) {
			maximize_view(view->prev, TRUE);
			view->prev = view;
			break;
		}
		/* Fall-through */
	case REQ_QUIT:
		return FALSE;

	default:
		report("Unknown key, press %s for help",
		       get_view_key(view, REQ_VIEW_HELP));
		return TRUE;
	}

	return TRUE;
}


/*
 * View backend utilities
 */

#include "pager.h"
#include "diff.h"


/*
 * Tree backend
 */

/* The top of the path stack. */
static struct view_history tree_view_history = { sizeof(char *) };

static void
pop_tree_stack_entry(struct position *position)
{
	char *path_position = NULL;

	pop_view_history_state(&tree_view_history, position, &path_position);
	path_position[0] = 0;
}

static void
push_tree_stack_entry(struct view *view, const char *name, struct position *position)
{
	size_t pathlen = strlen(view->env->directory);
	char *path_position = view->env->directory + pathlen;
	struct view_state *state = push_view_history_state(&tree_view_history, position, &path_position);

	if (!state)
		return;

	if (!string_format_from(view->env->directory, &pathlen, "%s/", name)) {
		pop_tree_stack_entry(NULL);
		return;
	}

	clear_position(position);
}

/* Parse output from git-ls-tree(1):
 *
 * 100644 blob 95925677ca47beb0b8cce7c0e0011bcc3f61470f  213045	tig.c
 */

#define SIZEOF_TREE_ATTR \
	STRING_SIZE("100644 blob f931e1d229c3e185caad4449bf5b66ed72462657\t")

#define SIZEOF_TREE_MODE \
	STRING_SIZE("100644 ")

#define TREE_ID_OFFSET \
	STRING_SIZE("100644 blob ")

#define tree_path_is_parent(path)	(!strcmp("..", (path)))

struct tree_entry {
	char id[SIZEOF_REV];
	char commit[SIZEOF_REV];
	mode_t mode;
	struct time time;		/* Date from the author ident. */
	const struct ident *author;	/* Author of the commit. */
	unsigned long size;
	char name[1];
};

struct tree_state {
	char commit[SIZEOF_REV];
	const struct ident *author;
	struct time author_time;
	int size_width;
	bool read_date;
};

static const char *
tree_path(const struct line *line)
{
	return ((struct tree_entry *) line->data)->name;
}

static int
tree_compare_entry(const struct line *line1, const struct line *line2)
{
	if (line1->type != line2->type)
		return line1->type == LINE_TREE_DIR ? -1 : 1;
	return strcmp(tree_path(line1), tree_path(line2));
}

static const enum sort_field tree_sort_fields[] = {
	ORDERBY_NAME, ORDERBY_DATE, ORDERBY_AUTHOR
};
static struct sort_state tree_sort_state = SORT_STATE(tree_sort_fields);

static int
tree_compare(const void *l1, const void *l2)
{
	const struct line *line1 = (const struct line *) l1;
	const struct line *line2 = (const struct line *) l2;
	const struct tree_entry *entry1 = ((const struct line *) l1)->data;
	const struct tree_entry *entry2 = ((const struct line *) l2)->data;

	if (line1->type == LINE_TREE_HEAD)
		return -1;
	if (line2->type == LINE_TREE_HEAD)
		return 1;

	switch (get_sort_field(tree_sort_state)) {
	case ORDERBY_DATE:
		return sort_order(tree_sort_state, timecmp(&entry1->time, &entry2->time));

	case ORDERBY_AUTHOR:
		return sort_order(tree_sort_state, ident_compare(entry1->author, entry2->author));

	case ORDERBY_NAME:
	default:
		return sort_order(tree_sort_state, tree_compare_entry(line1, line2));
	}
}


static struct line *
tree_entry(struct view *view, enum line_type type, const char *path,
	   const char *mode, const char *id, unsigned long size)
{
	bool custom = type == LINE_TREE_HEAD || tree_path_is_parent(path);
	struct tree_entry *entry;
	struct line *line = add_line_alloc(view, &entry, type, strlen(path), custom);

	if (!line)
		return NULL;

	strncpy(entry->name, path, strlen(path));
	if (mode)
		entry->mode = strtoul(mode, NULL, 8);
	if (id)
		string_copy_rev(entry->id, id);
	entry->size = size;

	return line;
}

static bool
tree_read_date(struct view *view, char *text, struct tree_state *state)
{
	if (!text && state->read_date) {
		state->read_date = FALSE;
		return TRUE;

	} else if (!text) {
		/* Find next entry to process */
		const char *log_file[] = {
			"git", "log", encoding_arg, "--no-color", "--pretty=raw",
				"--cc", "--raw", view->ops->id, "--", "%(directory)", NULL
		};

		if (!view->lines) {
			tree_entry(view, LINE_TREE_HEAD, view->env->directory, NULL, NULL, 0);
			tree_entry(view, LINE_TREE_DIR, "..", "040000", view->ref, 0);
			report("Tree is empty");
			return TRUE;
		}

		if (!begin_update(view, repo.cdup, log_file, OPEN_EXTRA)) {
			report("Failed to load tree data");
			return TRUE;
		}

		state->read_date = TRUE;
		return FALSE;

	} else if (*text == 'c' && get_line_type(text) == LINE_COMMIT) {
		string_copy_rev_from_commit_line(state->commit, text);

	} else if (*text == 'a' && get_line_type(text) == LINE_AUTHOR) {
		parse_author_line(text + STRING_SIZE("author "),
				  &state->author, &state->author_time);

	} else if (*text == ':') {
		char *pos;
		size_t annotated = 1;
		size_t i;

		pos = strrchr(text, '\t');
		if (!pos)
			return TRUE;
		text = pos + 1;
		if (*view->env->directory && !strncmp(text, view->env->directory, strlen(view->env->directory)))
			text += strlen(view->env->directory);
		pos = strchr(text, '/');
		if (pos)
			*pos = 0;

		for (i = 1; i < view->lines; i++) {
			struct line *line = &view->line[i];
			struct tree_entry *entry = line->data;

			annotated += !!entry->author;
			if (entry->author || strcmp(entry->name, text))
				continue;

			string_copy_rev(entry->commit, state->commit);
			entry->author = state->author;
			entry->time = state->author_time;
			line->dirty = 1;
			break;
		}

		if (annotated == view->lines)
			io_kill(view->pipe);
	}
	return TRUE;
}

static bool
tree_read(struct view *view, char *text)
{
	struct tree_state *state = view->private;
	struct tree_entry *data;
	struct line *entry, *line;
	enum line_type type;
	size_t textlen = text ? strlen(text) : 0;
	const char *attr_offset = text + SIZEOF_TREE_ATTR;
	char *path;
	size_t size;

	if (state->read_date || !text)
		return tree_read_date(view, text, state);

	if (textlen <= SIZEOF_TREE_ATTR)
		return FALSE;
	if (view->lines == 0 &&
	    !tree_entry(view, LINE_TREE_HEAD, view->env->directory, NULL, NULL, 0))
		return FALSE;

	size = parse_size(attr_offset, &state->size_width);
	path = strchr(attr_offset, '\t');
	if (!path)
		return FALSE;
	path++;

	/* Strip the path part ... */
	if (*view->env->directory) {
		size_t pathlen = textlen - SIZEOF_TREE_ATTR;
		size_t striplen = strlen(view->env->directory);

		if (pathlen > striplen)
			memmove(path, path + striplen,
				pathlen - striplen + 1);

		/* Insert "link" to parent directory. */
		if (view->lines == 1 &&
		    !tree_entry(view, LINE_TREE_DIR, "..", "040000", view->ref, 0))
			return FALSE;
	}

	type = text[SIZEOF_TREE_MODE] == 't' ? LINE_TREE_DIR : LINE_TREE_FILE;
	entry = tree_entry(view, type, path, text, text + TREE_ID_OFFSET, size);
	if (!entry)
		return FALSE;
	data = entry->data;

	/* Skip "Directory ..." and ".." line. */
	for (line = &view->line[1 + !!*view->env->directory]; line < entry; line++) {
		if (tree_compare_entry(line, entry) <= 0)
			continue;

		memmove(line + 1, line, (entry - line) * sizeof(*entry));

		line->data = data;
		line->type = type;
		for (; line <= entry; line++)
			line->dirty = line->cleareol = 1;
		return TRUE;
	}

	/* Move the current line to the first tree entry. */
	if (!check_position(&view->prev_pos) && !check_position(&view->pos))
		goto_view_line(view, 0, 1);

	return TRUE;
}

static bool
tree_draw(struct view *view, struct line *line, unsigned int lineno)
{
	struct tree_state *state = view->private;
	struct tree_entry *entry = line->data;

	if (line->type == LINE_TREE_HEAD) {
		if (draw_text(view, line->type, "Directory path /"))
			return TRUE;
	} else {
		if (draw_mode(view, entry->mode))
			return TRUE;

		if (draw_author(view, entry->author))
			return TRUE;

		if (draw_file_size(view, entry->size, state->size_width,
				   line->type != LINE_TREE_FILE))
			return TRUE;

		if (draw_date(view, &entry->time))
			return TRUE;

		if (draw_id(view, entry->commit))
			return TRUE;
	}

	draw_text(view, line->type, entry->name);
	return TRUE;
}

static void
open_blob_editor(const char *id, const char *name, unsigned int lineno)
{
	const char *blob_argv[] = { "git", "cat-file", "blob", id, NULL };
	char file[SIZEOF_STR];
	int fd;

	if (!name)
		name = "unknown";

	if (!string_format(file, "%s/tigblob.XXXXXX.%s", get_temp_dir(), name)) {
		report("Temporary file name is too long");
		return;
	}

	fd = mkstemps(file, strlen(name) + 1);

	if (fd == -1)
		report("Failed to create temporary file");
	else if (!io_run_append(blob_argv, fd))
		report("Failed to save blob data to file");
	else
		open_editor(file, lineno);
	if (fd != -1)
		unlink(file);
}

static enum request
tree_request(struct view *view, enum request request, struct line *line)
{
	enum open_flags flags;
	struct tree_entry *entry = line->data;

	switch (request) {
	case REQ_VIEW_BLAME:
		if (line->type != LINE_TREE_FILE) {
			report("Blame only supported for files");
			return REQ_NONE;
		}

		string_copy(view->env->ref, view->vid);
		return request;

	case REQ_EDIT:
		if (line->type != LINE_TREE_FILE) {
			report("Edit only supported for files");
		} else if (!is_head_commit(view->vid)) {
			open_blob_editor(entry->id, entry->name, 0);
		} else {
			open_editor(view->env->file, 0);
		}
		return REQ_NONE;

	case REQ_TOGGLE_SORT_FIELD:
	case REQ_TOGGLE_SORT_ORDER:
		sort_view(view, request, &tree_sort_state, tree_compare);
		return REQ_NONE;

	case REQ_PARENT:
	case REQ_BACK:
		if (!*view->env->directory) {
			/* quit view if at top of tree */
			return REQ_VIEW_CLOSE;
		}
		/* fake 'cd  ..' */
		line = &view->line[1];
		break;

	case REQ_ENTER:
		break;

	default:
		return request;
	}

	/* Cleanup the stack if the tree view is at a different tree. */
	if (!*view->env->directory)
		reset_view_history(&tree_view_history);

	switch (line->type) {
	case LINE_TREE_DIR:
		/* Depending on whether it is a subdirectory or parent link
		 * mangle the path buffer. */
		if (line == &view->line[1] && *view->env->directory) {
			pop_tree_stack_entry(&view->pos);

		} else {
			const char *basename = tree_path(line);

			push_tree_stack_entry(view, basename, &view->pos);
		}

		/* Trees and subtrees share the same ID, so they are not not
		 * unique like blobs. */
		flags = OPEN_RELOAD;
		request = REQ_VIEW_TREE;
		break;

	case LINE_TREE_FILE:
		flags = view_is_displayed(view) ? OPEN_SPLIT : OPEN_DEFAULT;
		request = REQ_VIEW_BLOB;
		break;

	default:
		return REQ_NONE;
	}

	open_view(view, request, flags);

	return REQ_NONE;
}

static bool
tree_grep(struct view *view, struct line *line)
{
	struct tree_entry *entry = line->data;
	const char *text[] = {
		entry->name,
		mkauthor(entry->author, opt_author_width, opt_show_author),
		mkdate(&entry->time, opt_show_date),
		NULL
	};

	return grep_text(view, text);
}

static void
tree_select(struct view *view, struct line *line)
{
	struct tree_entry *entry = line->data;

	if (line->type == LINE_TREE_HEAD) {
		string_format(view->ref, "Files in /%s", view->env->directory);
		return;
	}

	if (line->type == LINE_TREE_DIR && tree_path_is_parent(entry->name)) {
		string_copy(view->ref, "Open parent directory");
		view->env->blob[0] = 0;
		return;
	}

	if (line->type == LINE_TREE_FILE) {
		string_copy_rev(view->env->blob, entry->id);
		string_format(view->env->file, "%s%s", view->env->directory, tree_path(line));
	}

	string_copy_rev(view->ref, entry->id);
}

static bool
tree_open(struct view *view, enum open_flags flags)
{
	static const char *tree_argv[] = {
		"git", "ls-tree", "-l", "%(commit)", "%(directory)", NULL
	};

	if (string_rev_is_null(view->env->commit)) {
		report("No tree exists for this commit");
		return FALSE;
	}

	if (view->lines == 0 && repo.prefix[0]) {
		char *pos = repo.prefix;

		while (pos && *pos) {
			char *end = strchr(pos, '/');

			if (end)
				*end = 0;
			push_tree_stack_entry(view, pos, &view->pos);
			pos = end;
			if (end) {
				*end = '/';
				pos++;
			}
		}

	} else if (strcmp(view->vid, view->ops->id)) {
		view->env->directory[0] = 0;
	}

	return begin_update(view, repo.cdup, tree_argv, flags);
}

struct view_ops tree_ops = {
	"file",
	{ "tree" },
	view_env.commit,
	VIEW_SEND_CHILD_ENTER,
	sizeof(struct tree_state),
	tree_open,
	tree_read,
	tree_draw,
	tree_request,
	tree_grep,
	tree_select,
};

static bool
blob_open(struct view *view, enum open_flags flags)
{
	static const char *blob_argv[] = {
		"git", "cat-file", "blob", "%(blob)", NULL
	};

	if (!view->env->blob[0] && view->env->file[0]) {
		const char *commit = view->env->commit[0] ? view->env->commit : "HEAD";
		char blob_spec[SIZEOF_STR];
		const char *rev_parse_argv[] = {
			"git", "rev-parse", blob_spec, NULL
		};

		if (!string_format(blob_spec, "%s:%s", commit, view->env->file) ||
		    !io_run_buf(rev_parse_argv, view->env->blob, sizeof(view->env->blob))) {
			report("Failed to resolve blob from file name");
			return FALSE;
		}
	}

	if (!view->env->blob[0]) {
		report("No file chosen, press %s to open tree view",
			get_view_key(view, REQ_VIEW_TREE));
		return FALSE;
	}

	view->encoding = get_path_encoding(view->env->file, default_encoding);

	return begin_update(view, NULL, blob_argv, flags);
}

static bool
blob_read(struct view *view, char *line)
{
	if (!line)
		return TRUE;
	return add_line_text(view, line, LINE_DEFAULT) != NULL;
}

static enum request
blob_request(struct view *view, enum request request, struct line *line)
{
	switch (request) {
	case REQ_VIEW_BLAME:
		if (view->parent)
			string_copy(view->env->ref, view->parent->vid);
		return request;

	case REQ_EDIT:
		open_blob_editor(view->vid, NULL, (line - view->line) + 1);
		return REQ_NONE;
	default:
		return pager_request(view, request, line);
	}
}

struct view_ops blob_ops = {
	"line",
	{ "blob" },
	view_env.blob,
	VIEW_NO_FLAGS,
	0,
	blob_open,
	blob_read,
	pager_draw,
	blob_request,
	pager_grep,
	pager_select,
};

/*
 * Blame backend
 *
 * Loading the blame view is a two phase job:
 *
 *  1. File content is read either using view_env.file from the
 *     filesystem or using git-cat-file.
 *  2. Then blame information is incrementally added by
 *     reading output from git-blame.
 */

struct blame_history_state {
	char id[SIZEOF_REV];		/* SHA1 ID. */
	const char *filename;		/* Name of file. */
};

static struct view_history blame_view_history = { sizeof(struct blame_history_state) };

struct blame {
	struct blame_commit *commit;
	unsigned long lineno;
	char text[1];
};

struct blame_state {
	struct blame_commit *commit;
	int blamed;
	bool done_reading;
	bool auto_filename_display;
	/* The history state for the current view is cached in the view
	 * state so it always matches what was used to load the current blame
	 * view. */
	struct blame_history_state history_state;
};

static bool
blame_detect_filename_display(struct view *view)
{
	bool show_filenames = FALSE;
	const char *filename = NULL;
	int i;

	if (opt_blame_options) {
		for (i = 0; opt_blame_options[i]; i++) {
			if (prefixcmp(opt_blame_options[i], "-C"))
				continue;

			show_filenames = TRUE;
		}
	}

	for (i = 0; i < view->lines; i++) {
		struct blame *blame = view->line[i].data;

		if (blame->commit && blame->commit->id[0]) {
			if (!filename)
				filename = blame->commit->filename;
			else if (strcmp(filename, blame->commit->filename))
				show_filenames = TRUE;
		}
	}

	return show_filenames;
}

static bool
blame_open(struct view *view, enum open_flags flags)
{
	struct blame_state *state = view->private;
	const char *file_argv[] = { repo.cdup, view->env->file , NULL };
	char path[SIZEOF_STR];
	size_t i;

	if (!view->env->file[0]) {
		report("No file chosen, press %s to open tree view",
			get_view_key(view, REQ_VIEW_TREE));
		return FALSE;
	}

	if (!view->prev && *repo.prefix && !(flags & (OPEN_RELOAD | OPEN_REFRESH))) {
		string_copy(path, view->env->file);
		if (!string_format(view->env->file, "%s%s", repo.prefix, path)) {
			report("Failed to setup the blame view");
			return FALSE;
		}
	}

	if (*view->env->ref || !begin_update(view, repo.cdup, file_argv, flags)) {
		const char *blame_cat_file_argv[] = {
			"git", "cat-file", "blob", "%(ref):%(file)", NULL
		};

		if (!begin_update(view, repo.cdup, blame_cat_file_argv, flags))
			return FALSE;
	}

	/* First pass: remove multiple references to the same commit. */
	for (i = 0; i < view->lines; i++) {
		struct blame *blame = view->line[i].data;

		if (blame->commit && blame->commit->id[0])
			blame->commit->id[0] = 0;
		else
			blame->commit = NULL;
	}

	/* Second pass: free existing references. */
	for (i = 0; i < view->lines; i++) {
		struct blame *blame = view->line[i].data;

		if (blame->commit)
			free(blame->commit);
	}

	if (!(flags & OPEN_RELOAD))
		reset_view_history(&blame_view_history);
	string_copy_rev(state->history_state.id, view->env->ref);
	state->history_state.filename = get_path(view->env->file);
	if (!state->history_state.filename)
		return FALSE;
	string_format(view->vid, "%s", view->env->file);
	string_format(view->ref, "%s ...", view->env->file);

	return TRUE;
}

static struct blame_commit *
get_blame_commit(struct view *view, const char *id)
{
	size_t i;

	for (i = 0; i < view->lines; i++) {
		struct blame *blame = view->line[i].data;

		if (!blame->commit)
			continue;

		if (!strncmp(blame->commit->id, id, SIZEOF_REV - 1))
			return blame->commit;
	}

	{
		struct blame_commit *commit = calloc(1, sizeof(*commit));

		if (commit)
			string_ncopy(commit->id, id, SIZEOF_REV);
		return commit;
	}
}

static struct blame_commit *
read_blame_commit(struct view *view, const char *text, struct blame_state *state)
{
	struct blame_header header;
	struct blame_commit *commit;
	struct blame *blame;

	if (!parse_blame_header(&header, text, view->lines))
		return NULL;

	commit = get_blame_commit(view, text);
	if (!commit)
		return NULL;

	state->blamed += header.group;
	while (header.group--) {
		struct line *line = &view->line[header.lineno + header.group - 1];

		blame = line->data;
		blame->commit = commit;
		blame->lineno = header.orig_lineno + header.group - 1;
		line->dirty = 1;
	}

	return commit;
}

static bool
blame_read_file(struct view *view, const char *text, struct blame_state *state)
{
	if (!text) {
		const char *blame_argv[] = {
			"git", "blame", encoding_arg, "%(blameargs)", "--incremental",
				*view->env->ref ? view->env->ref : "--incremental", "--", view->env->file, NULL
		};

		if (view->lines == 0 && !view->prev)
			die("No blame exist for %s", view->vid);

		if (view->lines == 0 || !begin_update(view, repo.cdup, blame_argv, OPEN_EXTRA)) {
			report("Failed to load blame data");
			return TRUE;
		}

		if (view->env->lineno > 0) {
			select_view_line(view, view->env->lineno);
			view->env->lineno = 0;
		}

		state->done_reading = TRUE;
		return FALSE;

	} else {
		size_t textlen = strlen(text);
		struct blame *blame;

		if (!add_line_alloc(view, &blame, LINE_ID, textlen, FALSE))
			return FALSE;

		blame->commit = NULL;
		strncpy(blame->text, text, textlen);
		blame->text[textlen] = 0;
		return TRUE;
	}
}

static bool
blame_read(struct view *view, char *line)
{
	struct blame_state *state = view->private;

	if (!state->done_reading)
		return blame_read_file(view, line, state);

	if (!line) {
		state->auto_filename_display = blame_detect_filename_display(view);
		string_format(view->ref, "%s", view->vid);
		if (view_is_displayed(view)) {
			update_view_title(view);
			redraw_view_from(view, 0);
		}
		return TRUE;
	}

	if (!state->commit) {
		state->commit = read_blame_commit(view, line, state);
		string_format(view->ref, "%s %2zd%%", view->vid,
			      view->lines ? state->blamed * 100 / view->lines : 0);

	} else if (parse_blame_info(state->commit, line)) {
		if (!state->commit->filename)
			return FALSE;
		state->commit = NULL;
	}

	return TRUE;
}

static bool
blame_draw(struct view *view, struct line *line, unsigned int lineno)
{
	struct blame_state *state = view->private;
	struct blame *blame = line->data;
	struct time *time = NULL;
	const char *id = NULL, *filename = NULL;
	const struct ident *author = NULL;
	enum line_type id_type = LINE_ID;
	static const enum line_type blame_colors[] = {
		LINE_PALETTE_0,
		LINE_PALETTE_1,
		LINE_PALETTE_2,
		LINE_PALETTE_3,
		LINE_PALETTE_4,
		LINE_PALETTE_5,
		LINE_PALETTE_6,
	};

#define BLAME_COLOR(i) \
	(blame_colors[(i) % ARRAY_SIZE(blame_colors)])

	if (blame->commit && blame->commit->filename) {
		id = blame->commit->id;
		author = blame->commit->author;
		filename = blame->commit->filename;
		time = &blame->commit->time;
		id_type = BLAME_COLOR((long) blame->commit);
	}

	if (draw_date(view, time))
		return TRUE;

	if (draw_author(view, author))
		return TRUE;

	if (draw_filename(view, filename, state->auto_filename_display))
		return TRUE;

	if (draw_id_custom(view, id_type, id, opt_id_width))
		return TRUE;

	if (draw_lineno(view, lineno))
		return TRUE;

	draw_text(view, LINE_DEFAULT, blame->text);
	return TRUE;
}

static bool
check_blame_commit(struct blame *blame, bool check_null_id)
{
	if (!blame->commit)
		report("Commit data not loaded yet");
	else if (check_null_id && string_rev_is_null(blame->commit->id))
		report("No commit exist for the selected line");
	else
		return TRUE;
	return FALSE;
}

static void
setup_blame_parent_line(struct view *view, struct blame *blame)
{
	char from[SIZEOF_REF + SIZEOF_STR];
	char to[SIZEOF_REF + SIZEOF_STR];
	const char *diff_tree_argv[] = {
		"git", "diff", encoding_arg, "--no-textconv", "--no-extdiff",
			"--no-color", "-U0", from, to, "--", NULL
	};
	struct io io;
	int parent_lineno = -1;
	int blamed_lineno = -1;
	char *line;

	if (!string_format(from, "%s:%s", view->env->ref, view->env->file) ||
	    !string_format(to, "%s:%s", blame->commit->id, blame->commit->filename) ||
	    !io_run(&io, IO_RD, NULL, opt_env, diff_tree_argv))
		return;

	while ((line = io_get(&io, '\n', TRUE))) {
		if (*line == '@') {
			char *pos = strchr(line, '+');

			parent_lineno = atoi(line + 4);
			if (pos)
				blamed_lineno = atoi(pos + 1);

		} else if (*line == '+' && parent_lineno != -1) {
			if (blame->lineno == blamed_lineno - 1 &&
			    !strcmp(blame->text, line + 1)) {
				view->pos.lineno = parent_lineno ? parent_lineno - 1 : 0;
				break;
			}
			blamed_lineno++;
		}
	}

	io_done(&io);
}

static void
blame_go_forward(struct view *view, struct blame *blame, bool parent)
{
	struct blame_state *state = view->private;
	struct blame_history_state *history_state = &state->history_state;
	struct blame_commit *commit = blame->commit;
	const char *id = parent ? commit->parent_id : commit->id;
	const char *filename = parent ? commit->parent_filename : commit->filename;

	if (!*id && parent) {
		report("The selected commit has no parents");
		return;
	}

	if (!strcmp(history_state->id, id) && !strcmp(history_state->filename, filename)) {
		report("The selected commit is already displayed");
		return;
	}

	if (!push_view_history_state(&blame_view_history, &view->pos, history_state)) {
		report("Failed to save current view state");
		return;
	}

	string_ncopy(view->env->ref, id, sizeof(commit->id));
	string_ncopy(view->env->file, filename, strlen(filename));
	if (parent)
		setup_blame_parent_line(view, blame);
	view->env->lineno = blame->lineno;
	reload_view(view);
}

static void
blame_go_back(struct view *view)
{
	struct blame_history_state history_state;

	if (!pop_view_history_state(&blame_view_history, &view->pos, &history_state)) {
		report("Already at start of history");
		return;
	}

	string_copy(view->env->ref, history_state.id);
	string_ncopy(view->env->file, history_state.filename, strlen(history_state.filename));
	view->env->lineno = view->pos.lineno;
	reload_view(view);
}

static enum request
blame_request(struct view *view, enum request request, struct line *line)
{
	enum open_flags flags = view_is_displayed(view) ? OPEN_SPLIT : OPEN_DEFAULT;
	struct blame *blame = line->data;

	switch (request) {
	case REQ_VIEW_BLAME:
	case REQ_PARENT:
		if (!check_blame_commit(blame, TRUE))
			break;
		blame_go_forward(view, blame, request == REQ_PARENT);
		break;

	case REQ_BACK:
		blame_go_back(view);
		break;

	case REQ_ENTER:
		if (!check_blame_commit(blame, FALSE))
			break;

		if (view_is_displayed(VIEW(REQ_VIEW_DIFF)) &&
		    !strcmp(blame->commit->id, VIEW(REQ_VIEW_DIFF)->ref))
			break;

		if (string_rev_is_null(blame->commit->id)) {
			struct view *diff = VIEW(REQ_VIEW_DIFF);
			const char *diff_parent_argv[] = {
				GIT_DIFF_BLAME(encoding_arg,
					opt_diff_context_arg,
					opt_ignore_space_arg, view->vid)
			};
			const char *diff_no_parent_argv[] = {
				GIT_DIFF_BLAME_NO_PARENT(encoding_arg,
					opt_diff_context_arg,
					opt_ignore_space_arg, view->vid)
			};
			const char **diff_index_argv = *blame->commit->parent_id
				? diff_parent_argv : diff_no_parent_argv;

			open_argv(view, diff, diff_index_argv, NULL, flags);
			if (diff->pipe)
				string_copy_rev(diff->ref, NULL_ID);
		} else {
			open_view(view, REQ_VIEW_DIFF, flags);
		}
		break;

	default:
		return request;
	}

	return REQ_NONE;
}

static bool
blame_grep(struct view *view, struct line *line)
{
	struct blame *blame = line->data;
	struct blame_commit *commit = blame->commit;
	const char *text[] = {
		blame->text,
		commit ? commit->title : "",
		commit ? commit->id : "",
		commit ? mkauthor(commit->author, opt_author_width, opt_show_author) : "",
		commit ? mkdate(&commit->time, opt_show_date) : "",
		NULL
	};

	return grep_text(view, text);
}

static void
blame_select(struct view *view, struct line *line)
{
	struct blame *blame = line->data;
	struct blame_commit *commit = blame->commit;

	if (!commit)
		return;

	if (string_rev_is_null(commit->id))
		string_ncopy(view->env->commit, "HEAD", 4);
	else
		string_copy_rev(view->env->commit, commit->id);
}

struct view_ops blame_ops = {
	"line",
	{ "blame" },
	view_env.commit,
	VIEW_ALWAYS_LINENO | VIEW_SEND_CHILD_ENTER,
	sizeof(struct blame_state),
	blame_open,
	blame_read,
	blame_draw,
	blame_request,
	blame_grep,
	blame_select,
};

/*
 * Branch backend
 */

struct branch {
	const struct ident *author;	/* Author of the last commit. */
	struct time time;		/* Date of the last activity. */
	char title[128];		/* First line of the commit message. */
	const struct ref *ref;		/* Name and commit ID information. */
};

static const struct ref branch_all;
#define BRANCH_ALL_NAME	"All branches"
#define branch_is_all(branch) ((branch)->ref == &branch_all)

static const enum sort_field branch_sort_fields[] = {
	ORDERBY_NAME, ORDERBY_DATE, ORDERBY_AUTHOR
};
static struct sort_state branch_sort_state = SORT_STATE(branch_sort_fields);

struct branch_state {
	char id[SIZEOF_REV];
	size_t max_ref_length;
};

static int
branch_compare(const void *l1, const void *l2)
{
	const struct branch *branch1 = ((const struct line *) l1)->data;
	const struct branch *branch2 = ((const struct line *) l2)->data;

	if (branch_is_all(branch1))
		return -1;
	else if (branch_is_all(branch2))
		return 1;

	switch (get_sort_field(branch_sort_state)) {
	case ORDERBY_DATE:
		return sort_order(branch_sort_state, timecmp(&branch1->time, &branch2->time));

	case ORDERBY_AUTHOR:
		return sort_order(branch_sort_state, ident_compare(branch1->author, branch2->author));

	case ORDERBY_NAME:
	default:
		return sort_order(branch_sort_state, strcmp(branch1->ref->name, branch2->ref->name));
	}
}

static bool
branch_draw(struct view *view, struct line *line, unsigned int lineno)
{
	struct branch_state *state = view->private;
	struct branch *branch = line->data;
	enum line_type type = branch_is_all(branch) ? LINE_DEFAULT : get_line_type_from_ref(branch->ref);
	const char *branch_name = branch_is_all(branch) ? BRANCH_ALL_NAME : branch->ref->name;

	if (draw_lineno(view, lineno))
		return TRUE;

	if (draw_date(view, &branch->time))
		return TRUE;

	if (draw_author(view, branch->author))
		return TRUE;

	if (draw_field(view, type, branch_name, state->max_ref_length, ALIGN_LEFT, FALSE))
		return TRUE;

	if (draw_id(view, branch->ref->id))
		return TRUE;

	draw_text(view, LINE_DEFAULT, branch->title);
	return TRUE;
}

static enum request
branch_request(struct view *view, enum request request, struct line *line)
{
	struct branch *branch = line->data;

	switch (request) {
	case REQ_REFRESH:
		load_refs(TRUE);
		refresh_view(view);
		return REQ_NONE;

	case REQ_TOGGLE_SORT_FIELD:
	case REQ_TOGGLE_SORT_ORDER:
		sort_view(view, request, &branch_sort_state, branch_compare);
		return REQ_NONE;

	case REQ_ENTER:
	{
		const struct ref *ref = branch->ref;
		const char *all_branches_argv[] = {
			GIT_MAIN_LOG(encoding_arg, "", branch_is_all(branch) ? "--all" : ref->name, "")
		};
		struct view *main_view = VIEW(REQ_VIEW_MAIN);

		open_argv(view, main_view, all_branches_argv, NULL, OPEN_SPLIT);
		return REQ_NONE;
	}
	case REQ_JUMP_COMMIT:
	{
		int lineno;

		for (lineno = 0; lineno < view->lines; lineno++) {
			struct branch *branch = view->line[lineno].data;

			if (!strncasecmp(branch->ref->id, view->env->search, strlen(view->env->search))) {
				select_view_line(view, lineno);
				report_clear();
				return REQ_NONE;
			}
		}
	}
	default:
		return request;
	}
}

static bool
branch_read(struct view *view, char *line)
{
	struct branch_state *state = view->private;
	const char *title = NULL;
	const struct ident *author = NULL;
	struct time time = {};
	size_t i;

	if (!line)
		return TRUE;

	switch (get_line_type(line)) {
	case LINE_COMMIT:
		string_copy_rev_from_commit_line(state->id, line);
		return TRUE;

	case LINE_AUTHOR:
		parse_author_line(line + STRING_SIZE("author "), &author, &time);
		break;

	default:
		title = line + STRING_SIZE("title ");
	}

	for (i = 0; i < view->lines; i++) {
		struct branch *branch = view->line[i].data;

		if (strcmp(branch->ref->id, state->id))
			continue;

		if (author) {
			branch->author = author;
			branch->time = time;
		}

		if (title)
			string_expand(branch->title, sizeof(branch->title), title, 1);

		view->line[i].dirty = TRUE;
	}

	return TRUE;
}

static bool
branch_open_visitor(void *data, const struct ref *ref)
{
	struct view *view = data;
	struct branch_state *state = view->private;
	struct branch *branch;
	bool is_all = ref == &branch_all;
	size_t ref_length;

	if (ref->tag || ref->ltag)
		return TRUE;

	if (!add_line_alloc(view, &branch, LINE_DEFAULT, 0, is_all))
		return FALSE;

	ref_length = is_all ? STRING_SIZE(BRANCH_ALL_NAME) : strlen(ref->name);
	if (ref_length > state->max_ref_length)
		state->max_ref_length = ref_length;

	branch->ref = ref;
	return TRUE;
}

static bool
branch_open(struct view *view, enum open_flags flags)
{
	const char *branch_log[] = {
		"git", "log", encoding_arg, "--no-color", "--date=raw",
			"--pretty=format:commit %H%nauthor %an <%ae> %ad%ntitle %s",
			"--all", "--simplify-by-decoration", NULL
	};

	if (!begin_update(view, NULL, branch_log, OPEN_RELOAD)) {
		report("Failed to load branch data");
		return FALSE;
	}

	branch_open_visitor(view, &branch_all);
	foreach_ref(branch_open_visitor, view);

	return TRUE;
}

static bool
branch_grep(struct view *view, struct line *line)
{
	struct branch *branch = line->data;
	const char *text[] = {
		branch->ref->name,
		mkauthor(branch->author, opt_author_width, opt_show_author),
		NULL
	};

	return grep_text(view, text);
}

static void
branch_select(struct view *view, struct line *line)
{
	struct branch *branch = line->data;

	if (branch_is_all(branch)) {
		string_copy(view->ref, BRANCH_ALL_NAME);
		return;
	}
	string_copy_rev(view->ref, branch->ref->id);
	string_copy_rev(view->env->commit, branch->ref->id);
	string_copy_rev(view->env->head, branch->ref->id);
	string_copy_rev(view->env->branch, branch->ref->name);
}

struct view_ops branch_ops = {
	"branch",
	{ "branch" },
	view_env.head,
	VIEW_REFRESH,
	sizeof(struct branch_state),
	branch_open,
	branch_read,
	branch_draw,
	branch_request,
	branch_grep,
	branch_select,
};

/*
 * Status backend
 */

struct status {
	char status;
	struct {
		mode_t mode;
		char rev[SIZEOF_REV];
		char name[SIZEOF_STR];
	} old;
	struct {
		mode_t mode;
		char rev[SIZEOF_REV];
		char name[SIZEOF_STR];
	} new;
};

static char status_onbranch[SIZEOF_STR];
static struct status stage_status;
static enum line_type stage_line_type;

DEFINE_ALLOCATOR(realloc_ints, int, 32)

/* This should work even for the "On branch" line. */
static inline bool
status_has_none(struct view *view, struct line *line)
{
	return view_has_line(view, line) && !line[1].data;
}

/* Get fields from the diff line:
 * :100644 100644 06a5d6ae9eca55be2e0e585a152e6b1336f2b20e 0000000000000000000000000000000000000000 M
 */
static inline bool
status_get_diff(struct status *file, const char *buf, size_t bufsize)
{
	const char *old_mode = buf +  1;
	const char *new_mode = buf +  8;
	const char *old_rev  = buf + 15;
	const char *new_rev  = buf + 56;
	const char *status   = buf + 97;

	if (bufsize < 98 ||
	    old_mode[-1] != ':' ||
	    new_mode[-1] != ' ' ||
	    old_rev[-1]  != ' ' ||
	    new_rev[-1]  != ' ' ||
	    status[-1]   != ' ')
		return FALSE;

	file->status = *status;

	string_copy_rev(file->old.rev, old_rev);
	string_copy_rev(file->new.rev, new_rev);

	file->old.mode = strtoul(old_mode, NULL, 8);
	file->new.mode = strtoul(new_mode, NULL, 8);

	file->old.name[0] = file->new.name[0] = 0;

	return TRUE;
}

static bool
status_run(struct view *view, const char *argv[], char status, enum line_type type)
{
	struct status *unmerged = NULL;
	char *buf;
	struct io io;

	if (!io_run(&io, IO_RD, repo.cdup, opt_env, argv))
		return FALSE;

	add_line_nodata(view, type);

	while ((buf = io_get(&io, 0, TRUE))) {
		struct status *file = unmerged;

		if (!file) {
			if (!add_line_alloc(view, &file, type, 0, FALSE))
				goto error_out;
		}

		/* Parse diff info part. */
		if (status) {
			file->status = status;
			if (status == 'A')
				string_copy(file->old.rev, NULL_ID);

		} else if (!file->status || file == unmerged) {
			if (!status_get_diff(file, buf, strlen(buf)))
				goto error_out;

			buf = io_get(&io, 0, TRUE);
			if (!buf)
				break;

			/* Collapse all modified entries that follow an
			 * associated unmerged entry. */
			if (unmerged == file) {
				unmerged->status = 'U';
				unmerged = NULL;
			} else if (file->status == 'U') {
				unmerged = file;
			}
		}

		/* Grab the old name for rename/copy. */
		if (!*file->old.name &&
		    (file->status == 'R' || file->status == 'C')) {
			string_ncopy(file->old.name, buf, strlen(buf));

			buf = io_get(&io, 0, TRUE);
			if (!buf)
				break;
		}

		/* git-ls-files just delivers a NUL separated list of
		 * file names similar to the second half of the
		 * git-diff-* output. */
		string_ncopy(file->new.name, buf, strlen(buf));
		if (!*file->old.name)
			string_copy(file->old.name, file->new.name);
		file = NULL;
	}

	if (io_error(&io)) {
error_out:
		io_done(&io);
		return FALSE;
	}

	if (!view->line[view->lines - 1].data)
		add_line_nodata(view, LINE_STAT_NONE);

	io_done(&io);
	return TRUE;
}

static const char *status_diff_index_argv[] = { GIT_DIFF_STAGED_FILES("-z") };
static const char *status_diff_files_argv[] = { GIT_DIFF_UNSTAGED_FILES("-z") };

static const char *status_list_other_argv[] = {
	"git", "ls-files", "-z", "--others", "--exclude-standard", repo.prefix, NULL, NULL,
};

static const char *status_list_no_head_argv[] = {
	"git", "ls-files", "-z", "--cached", "--exclude-standard", NULL
};

static const char *update_index_argv[] = {
	"git", "update-index", "-q", "--unmerged", "--refresh", NULL
};

/* Restore the previous line number to stay in the context or select a
 * line with something that can be updated. */
static void
status_restore(struct view *view)
{
	if (!check_position(&view->prev_pos))
		return;

	if (view->prev_pos.lineno >= view->lines)
		view->prev_pos.lineno = view->lines - 1;
	while (view->prev_pos.lineno < view->lines && !view->line[view->prev_pos.lineno].data)
		view->prev_pos.lineno++;
	while (view->prev_pos.lineno > 0 && !view->line[view->prev_pos.lineno].data)
		view->prev_pos.lineno--;

	/* If the above fails, always skip the "On branch" line. */
	if (view->prev_pos.lineno < view->lines)
		view->pos.lineno = view->prev_pos.lineno;
	else
		view->pos.lineno = 1;

	if (view->prev_pos.offset > view->pos.lineno)
		view->pos.offset = view->pos.lineno;
	else if (view->prev_pos.offset < view->lines)
		view->pos.offset = view->prev_pos.offset;

	clear_position(&view->prev_pos);
}

static void
status_update_onbranch(void)
{
	static const char *paths[][2] = {
		{ "rebase-apply/rebasing",	"Rebasing" },
		{ "rebase-apply/applying",	"Applying mailbox" },
		{ "rebase-apply/",		"Rebasing mailbox" },
		{ "rebase-merge/interactive",	"Interactive rebase" },
		{ "rebase-merge/",		"Rebase merge" },
		{ "MERGE_HEAD",			"Merging" },
		{ "BISECT_LOG",			"Bisecting" },
		{ "HEAD",			"On branch" },
	};
	char buf[SIZEOF_STR];
	struct stat stat;
	int i;

	if (is_initial_commit()) {
		string_copy(status_onbranch, "Initial commit");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(paths); i++) {
		char *head = repo.head;

		if (!string_format(buf, "%s/%s", repo.git_dir, paths[i][0]) ||
		    lstat(buf, &stat) < 0)
			continue;

		if (!*repo.head) {
			struct io io;

			if (io_open(&io, "%s/rebase-merge/head-name", repo.git_dir) &&
			    io_read_buf(&io, buf, sizeof(buf))) {
				head = buf;
				if (!prefixcmp(head, "refs/heads/"))
					head += STRING_SIZE("refs/heads/");
			}
		}

		if (!string_format(status_onbranch, "%s %s", paths[i][1], head))
			string_copy(status_onbranch, repo.head);
		return;
	}

	string_copy(status_onbranch, "Not currently on any branch");
}

/* First parse staged info using git-diff-index(1), then parse unstaged
 * info using git-diff-files(1), and finally untracked files using
 * git-ls-files(1). */
static bool
status_open(struct view *view, enum open_flags flags)
{
	const char **staged_argv = is_initial_commit() ?
		status_list_no_head_argv : status_diff_index_argv;
	char staged_status = staged_argv == status_list_no_head_argv ? 'A' : 0;

	if (repo.is_inside_work_tree == FALSE) {
		report("The status view requires a working tree");
		return FALSE;
	}

	reset_view(view);

	add_line_nodata(view, LINE_STAT_HEAD);
	status_update_onbranch();

	io_run_bg(update_index_argv);

	status_list_other_argv[ARRAY_SIZE(status_list_other_argv) - 2] =
		opt_status_untracked_dirs ? NULL : "--directory";

	if (!status_run(view, staged_argv, staged_status, LINE_STAT_STAGED) ||
	    !status_run(view, status_diff_files_argv, 0, LINE_STAT_UNSTAGED) ||
	    !status_run(view, status_list_other_argv, '?', LINE_STAT_UNTRACKED)) {
		report("Failed to load status data");
		return FALSE;
	}

	/* Restore the exact position or use the specialized restore
	 * mode? */
	status_restore(view);
	return TRUE;
}

static bool
status_draw(struct view *view, struct line *line, unsigned int lineno)
{
	struct status *status = line->data;
	enum line_type type;
	const char *text;

	if (!status) {
		switch (line->type) {
		case LINE_STAT_STAGED:
			type = LINE_STAT_SECTION;
			text = "Changes to be committed:";
			break;

		case LINE_STAT_UNSTAGED:
			type = LINE_STAT_SECTION;
			text = "Changed but not updated:";
			break;

		case LINE_STAT_UNTRACKED:
			type = LINE_STAT_SECTION;
			text = "Untracked files:";
			break;

		case LINE_STAT_NONE:
			type = LINE_DEFAULT;
			text = "  (no files)";
			break;

		case LINE_STAT_HEAD:
			type = LINE_STAT_HEAD;
			text = status_onbranch;
			break;

		default:
			return FALSE;
		}
	} else {
		static char buf[] = { '?', ' ', ' ', ' ', 0 };

		buf[0] = status->status;
		if (draw_text(view, line->type, buf))
			return TRUE;
		type = LINE_DEFAULT;
		text = status->new.name;
	}

	draw_text(view, type, text);
	return TRUE;
}

static enum request
status_enter(struct view *view, struct line *line)
{
	struct status *status = line->data;
	enum open_flags flags = view_is_displayed(view) ? OPEN_SPLIT : OPEN_DEFAULT;

	if (line->type == LINE_STAT_NONE ||
	    (!status && line[1].type == LINE_STAT_NONE)) {
		report("No file to diff");
		return REQ_NONE;
	}

	switch (line->type) {
	case LINE_STAT_STAGED:
	case LINE_STAT_UNSTAGED:
		break;

	case LINE_STAT_UNTRACKED:
		if (!status) {
			report("No file to show");
			return REQ_NONE;
		}

	    	if (!suffixcmp(status->new.name, -1, "/")) {
			report("Cannot display a directory");
			return REQ_NONE;
		}
		break;

	case LINE_STAT_HEAD:
		return REQ_NONE;

	default:
		die("line type %d not handled in switch", line->type);
	}

	if (status) {
		stage_status = *status;
	} else {
		memset(&stage_status, 0, sizeof(stage_status));
	}

	stage_line_type = line->type;

	open_view(view, REQ_VIEW_STAGE, flags);
	return REQ_NONE;
}

static bool
status_exists(struct view *view, struct status *status, enum line_type type)
{
	unsigned long lineno;

	for (lineno = 0; lineno < view->lines; lineno++) {
		struct line *line = &view->line[lineno];
		struct status *pos = line->data;

		if (line->type != type)
			continue;
		if (!pos && (!status || !status->status) && line[1].data) {
			select_view_line(view, lineno);
			return TRUE;
		}
		if (pos && !strcmp(status->new.name, pos->new.name)) {
			select_view_line(view, lineno);
			return TRUE;
		}
	}

	return FALSE;
}


static bool
status_update_prepare(struct io *io, enum line_type type)
{
	const char *staged_argv[] = {
		"git", "update-index", "-z", "--index-info", NULL
	};
	const char *others_argv[] = {
		"git", "update-index", "-z", "--add", "--remove", "--stdin", NULL
	};

	switch (type) {
	case LINE_STAT_STAGED:
		return io_run(io, IO_WR, repo.cdup, opt_env, staged_argv);

	case LINE_STAT_UNSTAGED:
	case LINE_STAT_UNTRACKED:
		return io_run(io, IO_WR, repo.cdup, opt_env, others_argv);

	default:
		die("line type %d not handled in switch", type);
		return FALSE;
	}
}

static bool
status_update_write(struct io *io, struct status *status, enum line_type type)
{
	switch (type) {
	case LINE_STAT_STAGED:
		return io_printf(io, "%06o %s\t%s%c", status->old.mode,
				 status->old.rev, status->old.name, 0);

	case LINE_STAT_UNSTAGED:
	case LINE_STAT_UNTRACKED:
		return io_printf(io, "%s%c", status->new.name, 0);

	default:
		die("line type %d not handled in switch", type);
		return FALSE;
	}
}

static bool
status_update_file(struct status *status, enum line_type type)
{
	struct io io;
	bool result;

	if (!status_update_prepare(&io, type))
		return FALSE;

	result = status_update_write(&io, status, type);
	return io_done(&io) && result;
}

static bool
status_update_files(struct view *view, struct line *line)
{
	char buf[sizeof(view->ref)];
	struct io io;
	bool result = TRUE;
	struct line *pos;
	int files = 0;
	int file, done;
	int cursor_y = -1, cursor_x = -1;

	if (!status_update_prepare(&io, line->type))
		return FALSE;

	for (pos = line; view_has_line(view, pos) && pos->data; pos++)
		files++;

	string_copy(buf, view->ref);
	getsyx(cursor_y, cursor_x);
	for (file = 0, done = 5; result && file < files; line++, file++) {
		int almost_done = file * 100 / files;

		if (almost_done > done) {
			done = almost_done;
			string_format(view->ref, "updating file %u of %u (%d%% done)",
				      file, files, done);
			update_view_title(view);
			setsyx(cursor_y, cursor_x);
			doupdate();
		}
		result = status_update_write(&io, line->data, line->type);
	}
	string_copy(view->ref, buf);

	return io_done(&io) && result;
}

static bool
status_update(struct view *view)
{
	struct line *line = &view->line[view->pos.lineno];

	assert(view->lines);

	if (!line->data) {
		if (status_has_none(view, line)) {
			report("Nothing to update");
			return FALSE;
		}

		if (!status_update_files(view, line + 1)) {
			report("Failed to update file status");
			return FALSE;
		}

	} else if (!status_update_file(line->data, line->type)) {
		report("Failed to update file status");
		return FALSE;
	}

	return TRUE;
}

static bool
status_revert(struct status *status, enum line_type type, bool has_none)
{
	if (!status || type != LINE_STAT_UNSTAGED) {
		if (type == LINE_STAT_STAGED) {
			report("Cannot revert changes to staged files");
		} else if (type == LINE_STAT_UNTRACKED) {
			report("Cannot revert changes to untracked files");
		} else if (has_none) {
			report("Nothing to revert");
		} else {
			report("Cannot revert changes to multiple files");
		}

	} else if (prompt_yesno("Are you sure you want to revert changes?")) {
		char mode[10] = "100644";
		const char *reset_argv[] = {
			"git", "update-index", "--cacheinfo", mode,
				status->old.rev, status->old.name, NULL
		};
		const char *checkout_argv[] = {
			"git", "checkout", "--", status->old.name, NULL
		};

		if (status->status == 'U') {
			string_format(mode, "%5o", status->old.mode);

			if (status->old.mode == 0 && status->new.mode == 0) {
				reset_argv[2] = "--force-remove";
				reset_argv[3] = status->old.name;
				reset_argv[4] = NULL;
			}

			if (!io_run_fg(reset_argv, repo.cdup))
				return FALSE;
			if (status->old.mode == 0 && status->new.mode == 0)
				return TRUE;
		}

		return io_run_fg(checkout_argv, repo.cdup);
	}

	return FALSE;
}

static enum request
status_request(struct view *view, enum request request, struct line *line)
{
	struct status *status = line->data;

	switch (request) {
	case REQ_STATUS_UPDATE:
		if (!status_update(view))
			return REQ_NONE;
		break;

	case REQ_STATUS_REVERT:
		if (!status_revert(status, line->type, status_has_none(view, line)))
			return REQ_NONE;
		break;

	case REQ_STATUS_MERGE:
		if (!status || status->status != 'U') {
			report("Merging only possible for files with unmerged status ('U').");
			return REQ_NONE;
		}
		open_mergetool(status->new.name);
		break;

	case REQ_EDIT:
		if (!status)
			return request;
		if (status->status == 'D') {
			report("File has been deleted.");
			return REQ_NONE;
		}

		open_editor(status->new.name, 0);
		break;

	case REQ_VIEW_BLAME:
		if (line->type == LINE_STAT_UNTRACKED || !status) {
			report("Nothing to blame here");
			return REQ_NONE;
		}
		if (status)
			view->env->ref[0] = 0;
		return request;

	case REQ_ENTER:
		/* After returning the status view has been split to
		 * show the stage view. No further reloading is
		 * necessary. */
		return status_enter(view, line);

	case REQ_REFRESH:
		/* Load the current branch information and then the view. */
		load_refs(TRUE);
		break;

	default:
		return request;
	}

	refresh_view(view);

	return REQ_NONE;
}

static bool
status_stage_info_(char *buf, size_t bufsize,
		   enum line_type type, struct status *status)
{
	const char *file = status ? status->new.name : "";
	const char *info;

	switch (type) {
	case LINE_STAT_STAGED:
		if (status && status->status)
			info = "Staged changes to %s";
		else
			info = "Staged changes";
		break;

	case LINE_STAT_UNSTAGED:
		if (status && status->status)
			info = "Unstaged changes to %s";
		else
			info = "Unstaged changes";
		break;

	case LINE_STAT_UNTRACKED:
		info = "Untracked file %s";
		break;

	case LINE_STAT_HEAD:
	default:
		info = "";
	}

	return string_nformat(buf, bufsize, NULL, info, file);
}
#define status_stage_info(buf, type, status) \
	status_stage_info_(buf, sizeof(buf), type, status)

static void
status_select(struct view *view, struct line *line)
{
	struct status *status = line->data;
	char file[SIZEOF_STR] = "all files";
	const char *text;
	const char *key;

	if (status && !string_format(file, "'%s'", status->new.name))
		return;

	if (!status && line[1].type == LINE_STAT_NONE)
		line++;

	switch (line->type) {
	case LINE_STAT_STAGED:
		text = "Press %s to unstage %s for commit";
		break;

	case LINE_STAT_UNSTAGED:
		text = "Press %s to stage %s for commit";
		break;

	case LINE_STAT_UNTRACKED:
		text = "Press %s to stage %s for addition";
		break;

	case LINE_STAT_HEAD:
	case LINE_STAT_NONE:
		text = "Nothing to update";
		break;

	default:
		die("line type %d not handled in switch", line->type);
	}

	if (status && status->status == 'U') {
		text = "Press %s to resolve conflict in %s";
		key = get_view_key(view, REQ_STATUS_MERGE);

	} else {
		key = get_view_key(view, REQ_STATUS_UPDATE);
	}

	string_format(view->ref, text, key, file);
	status_stage_info(view->env->status, line->type, status);
	if (status)
		string_copy(view->env->file, status->new.name);
}

static bool
status_grep(struct view *view, struct line *line)
{
	struct status *status = line->data;

	if (status) {
		const char buf[2] = { status->status, 0 };
		const char *text[] = { status->new.name, buf, NULL };

		return grep_text(view, text);
	}

	return FALSE;
}

struct view_ops status_ops = {
	"file",
	{ "status" },
	"status",
	VIEW_CUSTOM_STATUS | VIEW_SEND_CHILD_ENTER | VIEW_STATUS_LIKE | VIEW_REFRESH,
	0,
	status_open,
	NULL,
	status_draw,
	status_request,
	status_grep,
	status_select,
};


struct stage_state {
	struct diff_state diff;
	size_t chunks;
	int *chunk;
};

static bool
stage_diff_write(struct io *io, struct line *line, struct line *end)
{
	while (line < end) {
		if (!io_write(io, line->data, strlen(line->data)) ||
		    !io_write(io, "\n", 1))
			return FALSE;
		line++;
		if (line->type == LINE_DIFF_CHUNK ||
		    line->type == LINE_DIFF_HEADER)
			break;
	}

	return TRUE;
}

static bool
stage_apply_chunk(struct view *view, struct line *chunk, struct line *line, bool revert)
{
	const char *apply_argv[SIZEOF_ARG] = {
		"git", "apply", "--whitespace=nowarn", NULL
	};
	struct line *diff_hdr;
	struct io io;
	int argc = 3;

	diff_hdr = find_prev_line_by_type(view, chunk, LINE_DIFF_HEADER);
	if (!diff_hdr)
		return FALSE;

	if (!revert)
		apply_argv[argc++] = "--cached";
	if (line != NULL)
		apply_argv[argc++] = "--unidiff-zero";
	if (revert || stage_line_type == LINE_STAT_STAGED)
		apply_argv[argc++] = "-R";
	apply_argv[argc++] = "-";
	apply_argv[argc++] = NULL;
	if (!io_run(&io, IO_WR, repo.cdup, opt_env, apply_argv))
		return FALSE;

	if (line != NULL) {
		unsigned long lineno = 0;
		struct line *context = chunk + 1;
		const char *markers[] = {
			line->type == LINE_DIFF_DEL ? ""   : ",0",
			line->type == LINE_DIFF_DEL ? ",0" : "",
		};

		parse_chunk_lineno(&lineno, chunk->data, line->type == LINE_DIFF_DEL ? '+' : '-');

		while (context < line) {
			if (context->type == LINE_DIFF_CHUNK || context->type == LINE_DIFF_HEADER) {
				break;
			} else if (context->type != LINE_DIFF_DEL && context->type != LINE_DIFF_ADD) {
				lineno++;
			}
			context++;
		}

		if (!stage_diff_write(&io, diff_hdr, chunk) ||
		    !io_printf(&io, "@@ -%lu%s +%lu%s @@\n",
			       lineno, markers[0], lineno, markers[1]) ||
		    !stage_diff_write(&io, line, line + 1)) {
			chunk = NULL;
		}
	} else {
		if (!stage_diff_write(&io, diff_hdr, chunk) ||
		    !stage_diff_write(&io, chunk, view->line + view->lines))
			chunk = NULL;
	}

	io_done(&io);

	return chunk ? TRUE : FALSE;
}

static bool
stage_update(struct view *view, struct line *line, bool single)
{
	struct line *chunk = NULL;

	if (!is_initial_commit() && stage_line_type != LINE_STAT_UNTRACKED)
		chunk = find_prev_line_by_type(view, line, LINE_DIFF_CHUNK);

	if (chunk) {
		if (!stage_apply_chunk(view, chunk, single ? line : NULL, FALSE)) {
			report("Failed to apply chunk");
			return FALSE;
		}

	} else if (!stage_status.status) {
		view = view->parent;

		for (line = view->line; view_has_line(view, line); line++)
			if (line->type == stage_line_type)
				break;

		if (!status_update_files(view, line + 1)) {
			report("Failed to update files");
			return FALSE;
		}

	} else if (!status_update_file(&stage_status, stage_line_type)) {
		report("Failed to update file");
		return FALSE;
	}

	return TRUE;
}

static bool
stage_revert(struct view *view, struct line *line)
{
	struct line *chunk = NULL;

	if (!is_initial_commit() && stage_line_type == LINE_STAT_UNSTAGED)
		chunk = find_prev_line_by_type(view, line, LINE_DIFF_CHUNK);

	if (chunk) {
		if (!prompt_yesno("Are you sure you want to revert changes?"))
			return FALSE;

		if (!stage_apply_chunk(view, chunk, NULL, TRUE)) {
			report("Failed to revert chunk");
			return FALSE;
		}
		return TRUE;

	} else {
		return status_revert(stage_status.status ? &stage_status : NULL,
				     stage_line_type, FALSE);
	}
}


static void
stage_next(struct view *view, struct line *line)
{
	struct stage_state *state = view->private;
	int i;

	if (!state->chunks) {
		for (line = view->line; view_has_line(view, line); line++) {
			if (line->type != LINE_DIFF_CHUNK)
				continue;

			if (!realloc_ints(&state->chunk, state->chunks, 1)) {
				report("Allocation failure");
				return;
			}

			state->chunk[state->chunks++] = line - view->line;
		}
	}

	for (i = 0; i < state->chunks; i++) {
		if (state->chunk[i] > view->pos.lineno) {
			do_scroll_view(view, state->chunk[i] - view->pos.lineno);
			report("Chunk %d of %zd", i + 1, state->chunks);
			return;
		}
	}

	report("No next chunk found");
}

static struct line *
stage_insert_chunk(struct view *view, struct chunk_header *header,
		   struct line *from, struct line *to, struct line *last_unchanged_line)
{
	char buf[SIZEOF_STR];
	char *chunk_line;
	unsigned long from_lineno = last_unchanged_line - view->line;
	unsigned long to_lineno = to - view->line;
	unsigned long after_lineno = to_lineno;

	if (!string_format(buf, "@@ -%lu,%lu +%lu,%lu @@",
			header->old.position, header->old.lines,
			header->new.position, header->new.lines))
		return NULL;

	chunk_line = strdup(buf);
	if (!chunk_line)
		return NULL;

	free(from->data);
	from->data = chunk_line;

	if (!to)
		return from;

	if (!add_line_at(view, after_lineno++, buf, LINE_DIFF_CHUNK, strlen(buf) + 1, FALSE))
		return NULL;

	while (from_lineno < to_lineno) {
		struct line *line = &view->line[from_lineno++];

		if (!add_line_at(view, after_lineno++, line->data, line->type, strlen(line->data) + 1, FALSE))
			return FALSE;
	}

	return view->line + after_lineno;
}

static void
stage_split_chunk(struct view *view, struct line *chunk_start)
{
	struct chunk_header header;
	struct line *last_changed_line = NULL, *last_unchanged_line = NULL, *pos;
	int chunks = 0;

	if (!chunk_start || !parse_chunk_header(&header, chunk_start->data)) {
		report("Failed to parse chunk header");
		return;
	}

	header.old.lines = header.new.lines = 0;

	for (pos = chunk_start + 1; view_has_line(view, pos); pos++) {
		const char *chunk_line = pos->data;

		if (*chunk_line == '@' || *chunk_line == '\\')
			break;

		if (*chunk_line == ' ') {
			header.old.lines++;
			header.new.lines++;
			if (last_unchanged_line < last_changed_line)
				last_unchanged_line = pos;
			continue;
		}

		if (last_changed_line && last_changed_line < last_unchanged_line) {
			unsigned long chunk_start_lineno = pos - view->line;
			unsigned long diff = pos - last_unchanged_line;

			pos = stage_insert_chunk(view, &header, chunk_start, pos, last_unchanged_line);

			header.old.position += header.old.lines - diff;
			header.new.position += header.new.lines - diff;
			header.old.lines = header.new.lines = diff;

			chunk_start = view->line + chunk_start_lineno;
			last_changed_line = last_unchanged_line = NULL;
			chunks++;
		}

		if (*chunk_line == '-') {
			header.old.lines++;
			last_changed_line = pos;
		} else if (*chunk_line == '+') {
			header.new.lines++;
			last_changed_line = pos;
		}
	}

	if (chunks) {
		stage_insert_chunk(view, &header, chunk_start, NULL, NULL);
		redraw_view(view);
		report("Split the chunk in %d", chunks + 1);
	} else {
		report("The chunk cannot be split");
	}
}

static enum request
stage_request(struct view *view, enum request request, struct line *line)
{
	switch (request) {
	case REQ_STATUS_UPDATE:
		if (!stage_update(view, line, FALSE))
			return REQ_NONE;
		break;

	case REQ_STATUS_REVERT:
		if (!stage_revert(view, line))
			return REQ_NONE;
		break;

	case REQ_STAGE_UPDATE_LINE:
		if (stage_line_type == LINE_STAT_UNTRACKED ||
		    stage_status.status == 'A') {
			report("Staging single lines is not supported for new files");
			return REQ_NONE;
		}
		if (line->type != LINE_DIFF_DEL && line->type != LINE_DIFF_ADD) {
			report("Please select a change to stage");
			return REQ_NONE;
		}
		if (!stage_update(view, line, TRUE))
			return REQ_NONE;
		break;

	case REQ_STAGE_NEXT:
		if (stage_line_type == LINE_STAT_UNTRACKED) {
			report("File is untracked; press %s to add",
			       get_view_key(view, REQ_STATUS_UPDATE));
			return REQ_NONE;
		}
		stage_next(view, line);
		return REQ_NONE;

	case REQ_STAGE_SPLIT_CHUNK:
		if (stage_line_type == LINE_STAT_UNTRACKED ||
		    !(line = find_prev_line_by_type(view, line, LINE_DIFF_CHUNK))) {
			report("No chunks to split in sight");
			return REQ_NONE;
		}
		stage_split_chunk(view, line);
		return REQ_NONE;

	case REQ_EDIT:
		if (!stage_status.new.name[0])
			return diff_common_edit(view, request, line);

		if (stage_status.status == 'D') {
			report("File has been deleted.");
			return REQ_NONE;
		}

		if (stage_line_type == LINE_STAT_UNTRACKED) {
			open_editor(stage_status.new.name, (line - view->line) + 1);
		} else {
			open_editor(stage_status.new.name, diff_get_lineno(view, line));
		}
		break;

	case REQ_REFRESH:
		/* Reload everything(including current branch information) ... */
		load_refs(TRUE);
		break;

	case REQ_VIEW_BLAME:
		if (stage_line_type == LINE_STAT_UNTRACKED) {
			report("Nothing to blame here");
			return REQ_NONE;
		}

		if (stage_status.new.name[0]) {
			string_copy(view->env->file, stage_status.new.name);
		} else {
			const char *file = diff_get_pathname(view, line);

			if (file)
				string_copy(view->env->file, file);
		}

		view->env->ref[0] = 0;
		view->env->lineno = diff_get_lineno(view, line);
		if (view->env->lineno > 0)
			view->env->lineno--;
		return request;

	case REQ_ENTER:
		return diff_common_enter(view, request, line);

	case REQ_DIFF_CONTEXT_UP:
	case REQ_DIFF_CONTEXT_DOWN:
		if (!update_diff_context(request))
			return REQ_NONE;
		break;

	default:
		return request;
	}

	refresh_view(view->parent);

	/* Check whether the staged entry still exists, and close the
	 * stage view if it doesn't. */
	if (!status_exists(view->parent, &stage_status, stage_line_type)) {
		status_restore(view->parent);
		return REQ_VIEW_CLOSE;
	}

	refresh_view(view);

	return REQ_NONE;
}

static bool
stage_open(struct view *view, enum open_flags flags)
{
	static const char *no_head_diff_argv[] = {
		GIT_DIFF_STAGED_INITIAL(encoding_arg, opt_diff_context_arg, opt_ignore_space_arg,
			stage_status.new.name)
	};
	static const char *index_show_argv[] = {
		GIT_DIFF_STAGED(encoding_arg, opt_diff_context_arg, opt_ignore_space_arg,
			stage_status.old.name, stage_status.new.name)
	};
	static const char *files_show_argv[] = {
		GIT_DIFF_UNSTAGED(encoding_arg, opt_diff_context_arg, opt_ignore_space_arg,
			stage_status.old.name, stage_status.new.name)
	};
	/* Diffs for unmerged entries are empty when passing the new
	 * path, so leave out the new path. */
	static const char *files_unmerged_argv[] = {
		"git", "diff-files", encoding_arg, "--root", "--patch-with-stat",
			opt_diff_context_arg, opt_ignore_space_arg, "--",
			stage_status.old.name, NULL
	};
	static const char *file_argv[] = { repo.cdup, stage_status.new.name, NULL };
	const char **argv = NULL;

	if (!stage_line_type) {
		report("No stage content, press %s to open the status view and choose file",
			get_view_key(view, REQ_VIEW_STATUS));
		return FALSE;
	}

	view->encoding = NULL;

	switch (stage_line_type) {
	case LINE_STAT_STAGED:
		if (is_initial_commit()) {
			argv = no_head_diff_argv;
		} else {
			argv = index_show_argv;
		}
		break;

	case LINE_STAT_UNSTAGED:
		if (stage_status.status != 'U')
			argv = files_show_argv;
		else
			argv = files_unmerged_argv;
		break;

	case LINE_STAT_UNTRACKED:
		argv = file_argv;
		view->encoding = get_path_encoding(stage_status.old.name, default_encoding);
		break;

	case LINE_STAT_HEAD:
	default:
		die("line type %d not handled in switch", stage_line_type);
	}

	if (!status_stage_info(view->ref, stage_line_type, &stage_status)
		|| !argv_copy(&view->argv, argv)) {
		report("Failed to open staged view");
		return FALSE;
	}

	view->vid[0] = 0;
	view->dir = repo.cdup;
	return begin_update(view, NULL, NULL, flags);
}

static bool
stage_read(struct view *view, char *data)
{
	struct stage_state *state = view->private;

	if (stage_line_type == LINE_STAT_UNTRACKED)
		return pager_common_read(view, data, LINE_DEFAULT);

	if (data && diff_common_read(view, data, &state->diff))
		return TRUE;

	return pager_read(view, data);
}

struct view_ops stage_ops = {
	"line",
	{ "stage" },
	view_env.status,
	VIEW_DIFF_LIKE | VIEW_REFRESH,
	sizeof(struct stage_state),
	stage_open,
	stage_read,
	diff_common_draw,
	stage_request,
	pager_grep,
	pager_select,
};


/*
 * Revision graph
 */

static const enum line_type graph_colors[] = {
	LINE_PALETTE_0,
	LINE_PALETTE_1,
	LINE_PALETTE_2,
	LINE_PALETTE_3,
	LINE_PALETTE_4,
	LINE_PALETTE_5,
	LINE_PALETTE_6,
};

static enum line_type get_graph_color(struct graph_symbol *symbol)
{
	if (symbol->commit)
		return LINE_GRAPH_COMMIT;
	assert(symbol->color < ARRAY_SIZE(graph_colors));
	return graph_colors[symbol->color];
}

static bool
draw_graph_utf8(struct view *view, struct graph_symbol *symbol, enum line_type color, bool first)
{
	const char *chars = graph_symbol_to_utf8(symbol);

	return draw_text(view, color, chars + !!first);
}

static bool
draw_graph_ascii(struct view *view, struct graph_symbol *symbol, enum line_type color, bool first)
{
	const char *chars = graph_symbol_to_ascii(symbol);

	return draw_text(view, color, chars + !!first);
}

static bool
draw_graph_chtype(struct view *view, struct graph_symbol *symbol, enum line_type color, bool first)
{
	const chtype *chars = graph_symbol_to_chtype(symbol);

	return draw_graphic(view, color, chars + !!first, 2 - !!first, FALSE);
}

typedef bool (*draw_graph_fn)(struct view *, struct graph_symbol *, enum line_type, bool);

static bool draw_graph(struct view *view, struct graph_canvas *canvas)
{
	static const draw_graph_fn fns[] = {
		draw_graph_ascii,
		draw_graph_chtype,
		draw_graph_utf8
	};
	draw_graph_fn fn = fns[opt_line_graphics];
	int i;

	for (i = 0; i < canvas->size; i++) {
		struct graph_symbol *symbol = &canvas->symbols[i];
		enum line_type color = get_graph_color(symbol);

		if (fn(view, symbol, color, i == 0))
			return TRUE;
	}

	return draw_text(view, LINE_MAIN_REVGRAPH, " ");
}

/*
 * Main view backend
 */

DEFINE_ALLOCATOR(realloc_reflogs, char *, 32)

struct commit {
	char id[SIZEOF_REV];		/* SHA1 ID. */
	const struct ident *author;	/* Author of the commit. */
	struct time time;		/* Date from the author ident. */
	struct graph_canvas graph;	/* Ancestry chain graphics. */
	char title[1];			/* First line of the commit message. */
};

struct main_state {
	struct graph graph;
	struct commit current;
	char **reflog;
	size_t reflogs;
	int reflog_width;
	char reflogmsg[SIZEOF_STR / 2];
	bool in_header;
	bool added_changes_commits;
	bool with_graph;
};

static void
main_register_commit(struct view *view, struct commit *commit, const char *ids, bool is_boundary)
{
	struct main_state *state = view->private;

	string_copy_rev(commit->id, ids);
	if (state->with_graph)
		graph_add_commit(&state->graph, &commit->graph, commit->id, ids, is_boundary);
}

static struct commit *
main_add_commit(struct view *view, enum line_type type, struct commit *template,
		const char *title, bool custom)
{
	struct main_state *state = view->private;
	size_t titlelen = strlen(title);
	struct commit *commit;
	char buf[SIZEOF_STR / 2];

	/* FIXME: More graceful handling of titles; append "..." to
	 * shortened titles, etc. */
	string_expand(buf, sizeof(buf), title, 1);
	title = buf;
	titlelen = strlen(title);

	if (!add_line_alloc(view, &commit, type, titlelen, custom))
		return NULL;

	*commit = *template;
	strncpy(commit->title, title, titlelen);
	state->graph.canvas = &commit->graph;
	memset(template, 0, sizeof(*template));
	state->reflogmsg[0] = 0;
	return commit;
}

static inline void
main_flush_commit(struct view *view, struct commit *commit)
{
	if (*commit->id)
		main_add_commit(view, LINE_MAIN_COMMIT, commit, "", FALSE);
}

static bool
main_has_changes(const char *argv[])
{
	struct io io;

	if (!io_run(&io, IO_BG, NULL, opt_env, argv, -1))
		return FALSE;
	io_done(&io);
	return io.status == 1;
}

static void
main_add_changes_commit(struct view *view, enum line_type type, const char *parent, const char *title)
{
	char ids[SIZEOF_STR] = NULL_ID " ";
	struct main_state *state = view->private;
	struct commit commit = {};
	struct timeval now;
	struct timezone tz;

	if (!parent)
		return;

	string_copy_rev(ids + STRING_SIZE(NULL_ID " "), parent);

	if (!gettimeofday(&now, &tz)) {
		commit.time.tz = tz.tz_minuteswest * 60;
		commit.time.sec = now.tv_sec - commit.time.tz;
	}

	commit.author = &unknown_ident;
	main_register_commit(view, &commit, ids, FALSE);
	if (main_add_commit(view, type, &commit, title, TRUE) && state->with_graph)
		graph_render_parents(&state->graph);
}

static void
main_add_changes_commits(struct view *view, struct main_state *state, const char *parent)
{
	const char *staged_argv[] = { GIT_DIFF_STAGED_FILES("--quiet") };
	const char *unstaged_argv[] = { GIT_DIFF_UNSTAGED_FILES("--quiet") };
	const char *staged_parent = NULL_ID;
	const char *unstaged_parent = parent;

	if (!is_head_commit(parent))
		return;

	state->added_changes_commits = TRUE;

	io_run_bg(update_index_argv);

	if (!main_has_changes(unstaged_argv)) {
		unstaged_parent = NULL;
		staged_parent = parent;
	}

	if (!main_has_changes(staged_argv)) {
		staged_parent = NULL;
	}

	main_add_changes_commit(view, LINE_STAT_STAGED, staged_parent, "Staged changes");
	main_add_changes_commit(view, LINE_STAT_UNSTAGED, unstaged_parent, "Unstaged changes");
}

static bool
main_open(struct view *view, enum open_flags flags)
{
	static const char *main_argv[] = {
		GIT_MAIN_LOG(encoding_arg, "%(cmdlineargs)", "%(revargs)", "%(fileargs)")
	};
	struct main_state *state = view->private;

	state->with_graph = opt_show_rev_graph &&
			    opt_commit_order != COMMIT_ORDER_REVERSE;

	if (flags & OPEN_PAGER_MODE) {
		state->added_changes_commits = TRUE;
		state->with_graph = FALSE;
	}

	return begin_update(view, NULL, main_argv, flags);
}

static void
main_done(struct view *view)
{
	struct main_state *state = view->private;
	int i;

	for (i = 0; i < view->lines; i++) {
		struct commit *commit = view->line[i].data;

		free(commit->graph.symbols);
	}

	for (i = 0; i < state->reflogs; i++)
		free(state->reflog[i]);
	free(state->reflog);
}

#define MAIN_NO_COMMIT_REFS 1
#define main_check_commit_refs(line)	!((line)->user_flags & MAIN_NO_COMMIT_REFS)
#define main_mark_no_commit_refs(line)	((line)->user_flags |= MAIN_NO_COMMIT_REFS)

static inline struct ref_list *
main_get_commit_refs(struct line *line, struct commit *commit)
{
	struct ref_list *refs = NULL;

	if (main_check_commit_refs(line) && !(refs = get_ref_list(commit->id)))
		main_mark_no_commit_refs(line);

	return refs;
}

static bool
main_draw(struct view *view, struct line *line, unsigned int lineno)
{
	struct main_state *state = view->private;
	struct commit *commit = line->data;
	struct ref_list *refs = NULL;

	if (!commit->author)
		return FALSE;

	if (draw_lineno(view, lineno))
		return TRUE;

	if (opt_show_id) {
		if (state->reflogs) {
			const char *id = state->reflog[line->lineno - 1];

			if (draw_id_custom(view, LINE_ID, id, state->reflog_width))
				return TRUE;
		} else if (draw_id(view, commit->id)) {
			return TRUE;
		}
	}

	if (draw_date(view, &commit->time))
		return TRUE;

	if (draw_author(view, commit->author))
		return TRUE;

	if (state->with_graph && draw_graph(view, &commit->graph))
		return TRUE;

	if ((refs = main_get_commit_refs(line, commit)) && draw_refs(view, refs))
		return TRUE;

	if (commit->title)
		draw_commit_title(view, commit->title, 0);
	return TRUE;
}

static bool
main_add_reflog(struct view *view, struct main_state *state, char *reflog)
{
	char *end = strchr(reflog, ' ');
	int id_width;

	if (!end)
		return FALSE;
	*end = 0;

	if (!realloc_reflogs(&state->reflog, state->reflogs, 1)
	    || !(reflog = strdup(reflog)))
		return FALSE;

	state->reflog[state->reflogs++] = reflog;
	id_width = strlen(reflog);
	if (state->reflog_width < id_width) {
		state->reflog_width = id_width;
		if (opt_show_id)
			view->force_redraw = TRUE;
	}

	return TRUE;
}

/* Reads git log --pretty=raw output and parses it into the commit struct. */
static bool
main_read(struct view *view, char *line)
{
	struct main_state *state = view->private;
	struct graph *graph = &state->graph;
	enum line_type type;
	struct commit *commit = &state->current;

	if (!line) {
		main_flush_commit(view, commit);

		if (!view->lines && !view->prev)
			die("No revisions match the given arguments.");
		if (view->lines > 0) {
			struct commit *last = view->line[view->lines - 1].data;

			view->line[view->lines - 1].dirty = 1;
			if (!last->author) {
				view->lines--;
				free(last);
			}
		}

		if (state->with_graph)
			done_graph(graph);
		return TRUE;
	}

	type = get_line_type(line);
	if (type == LINE_COMMIT) {
		bool is_boundary;

		state->in_header = TRUE;
		line += STRING_SIZE("commit ");
		is_boundary = *line == '-';
		while (*line && !isalnum(*line))
			line++;

		if (!state->added_changes_commits && opt_show_changes && repo.is_inside_work_tree)
			main_add_changes_commits(view, state, line);
		else
			main_flush_commit(view, commit);

		main_register_commit(view, &state->current, line, is_boundary);
		return TRUE;
	}

	if (!*commit->id)
		return TRUE;

	/* Empty line separates the commit header from the log itself. */
	if (*line == '\0')
		state->in_header = FALSE;

	switch (type) {
	case LINE_PP_REFLOG:
		if (!main_add_reflog(view, state, line + STRING_SIZE("Reflog: ")))
			return FALSE;
		break;

	case LINE_PP_REFLOGMSG:
		line += STRING_SIZE("Reflog message: ");
		string_ncopy(state->reflogmsg, line, strlen(line));
		break;

	case LINE_PARENT:
		if (state->with_graph && !graph->has_parents)
			graph_add_parent(graph, line + STRING_SIZE("parent "));
		break;

	case LINE_AUTHOR:
		parse_author_line(line + STRING_SIZE("author "),
				  &commit->author, &commit->time);
		if (state->with_graph)
			graph_render_parents(graph);
		break;

	default:
		/* Fill in the commit title if it has not already been set. */
		if (*commit->title)
			break;

		/* Skip lines in the commit header. */
		if (state->in_header)
			break;

		/* Require titles to start with a non-space character at the
		 * offset used by git log. */
		if (strncmp(line, "    ", 4))
			break;
		line += 4;
		/* Well, if the title starts with a whitespace character,
		 * try to be forgiving.  Otherwise we end up with no title. */
		while (isspace(*line))
			line++;
		if (*line == '\0')
			break;
		if (*state->reflogmsg)
			line = state->reflogmsg;
		main_add_commit(view, LINE_MAIN_COMMIT, commit, line, FALSE);
	}

	return TRUE;
}

static enum request
main_request(struct view *view, enum request request, struct line *line)
{
	enum open_flags flags = (view_is_displayed(view) && request != REQ_VIEW_DIFF)
				? OPEN_SPLIT : OPEN_DEFAULT;

	switch (request) {
	case REQ_NEXT:
	case REQ_PREVIOUS:
		if (view_is_displayed(view) && display[0] != view)
			return request;
		/* Do not pass navigation requests to the branch view
		 * when the main view is maximized. (GH #38) */
		return request == REQ_NEXT ? REQ_MOVE_DOWN : REQ_MOVE_UP;

	case REQ_VIEW_DIFF:
	case REQ_ENTER:
		if (view_is_displayed(view) && display[0] != view)
			maximize_view(view, TRUE);

		if (line->type == LINE_STAT_UNSTAGED
		    || line->type == LINE_STAT_STAGED) {
			struct view *diff = VIEW(REQ_VIEW_DIFF);
			const char *diff_staged_argv[] = {
				GIT_DIFF_STAGED(encoding_arg,
					opt_diff_context_arg,
					opt_ignore_space_arg, NULL, NULL)
			};
			const char *diff_unstaged_argv[] = {
				GIT_DIFF_UNSTAGED(encoding_arg,
					opt_diff_context_arg,
					opt_ignore_space_arg, NULL, NULL)
			};
			const char **diff_argv = line->type == LINE_STAT_STAGED
				? diff_staged_argv : diff_unstaged_argv;

			open_argv(view, diff, diff_argv, NULL, flags);
			break;
		}

		open_view(view, REQ_VIEW_DIFF, flags);
		break;

	case REQ_REFRESH:
		load_refs(TRUE);
		refresh_view(view);
		break;

	case REQ_JUMP_COMMIT:
	{
		int lineno;

		for (lineno = 0; lineno < view->lines; lineno++) {
			struct commit *commit = view->line[lineno].data;

			if (!strncasecmp(commit->id, view->env->search, strlen(view->env->search))) {
				select_view_line(view, lineno);
				report_clear();
				return REQ_NONE;
			}
		}

		report("Unable to find commit '%s'", view->env->search);
		break;
	}
	default:
		return request;
	}

	return REQ_NONE;
}

static bool
grep_refs(struct line *line, struct commit *commit, regex_t *regex)
{
	struct ref_list *list;
	regmatch_t pmatch;
	size_t i;

	if (!opt_show_refs || !(list = main_get_commit_refs(line, commit)))
		return FALSE;

	for (i = 0; i < list->size; i++) {
		if (!regexec(regex, list->refs[i]->name, 1, &pmatch, 0))
			return TRUE;
	}

	return FALSE;
}

static bool
main_grep(struct view *view, struct line *line)
{
	struct commit *commit = line->data;
	const char *text[] = {
		commit->id,
		commit->title,
		mkauthor(commit->author, opt_author_width, opt_show_author),
		mkdate(&commit->time, opt_show_date),
		NULL
	};

	return grep_text(view, text) || grep_refs(line, commit, view->regex);
}

static struct ref *
main_get_commit_branch(struct line *line, struct commit *commit)
{
	struct ref_list *list = main_get_commit_refs(line, commit);
	struct ref *branch = NULL;
	size_t i;

	for (i = 0; list && i < list->size; i++) {
		struct ref *ref = list->refs[i];

		switch (get_line_type_from_ref(ref)) {
		case LINE_MAIN_HEAD:
		case LINE_MAIN_REF:
			/* Always prefer local branches. */
			return ref;

		default:
			branch = ref;
		}
	}

	return branch;
}

static void
main_select(struct view *view, struct line *line)
{
	struct commit *commit = line->data;

	if (line->type == LINE_STAT_STAGED || line->type == LINE_STAT_UNSTAGED) {
		string_ncopy(view->ref, commit->title, strlen(commit->title));
	} else {
		struct ref *branch = main_get_commit_branch(line, commit);

		if (branch)
			string_copy_rev(view->env->branch, branch->name);
		string_copy_rev(view->ref, commit->id);
	}
	string_copy_rev(view->env->commit, commit->id);
}

struct view_ops main_ops = {
	"commit",
	{ "main" },
	view_env.head,
	VIEW_SEND_CHILD_ENTER | VIEW_FILE_FILTER | VIEW_LOG_LIKE | VIEW_REFRESH,
	sizeof(struct main_state),
	main_open,
	main_read,
	main_draw,
	main_request,
	main_grep,
	main_select,
	main_done,
};

static bool
stash_open(struct view *view, enum open_flags flags)
{
	static const char *stash_argv[] = { "git", "stash", "list",
		encoding_arg, "--no-color", "--pretty=raw", NULL };
	struct main_state *state = view->private;

	state->added_changes_commits = TRUE;
	state->with_graph = FALSE;
	return begin_update(view, NULL, stash_argv, flags | OPEN_RELOAD);
}

static void
stash_select(struct view *view, struct line *line)
{
	main_select(view, line);
	string_format(view->env->stash, "stash@{%d}", line->lineno - 1);
	string_copy(view->ref, view->env->stash);
}

struct view_ops stash_ops = {
	"stash",
	{ "stash" },
	view_env.stash,
	VIEW_SEND_CHILD_ENTER | VIEW_REFRESH,
	sizeof(struct main_state),
	stash_open,
	main_read,
	main_draw,
	main_request,
	main_grep,
	stash_select,
};

/*
 * Main
 */

static const char usage[] =
"tig " TIG_VERSION " (" __DATE__ ")\n"
"\n"
"Usage: tig        [options] [revs] [--] [paths]\n"
"   or: tig log    [options] [revs] [--] [paths]\n"
"   or: tig show   [options] [revs] [--] [paths]\n"
"   or: tig blame  [options] [rev] [--] path\n"
"   or: tig stash\n"
"   or: tig status\n"
"   or: tig <      [git command output]\n"
"\n"
"Options:\n"
"  +<number>       Select line <number> in the first view\n"
"  -v, --version   Show version and exit\n"
"  -h, --help      Show help message and exit";

static void TIG_NORETURN
quit(int sig)
{
	if (sig)
		signal(sig, SIG_DFL);

	/* XXX: Restore tty modes and let the OS cleanup the rest! */
	if (die_callback)
		die_callback();
	exit(0);
}

static int
read_filter_args(char *name, size_t namelen, char *value, size_t valuelen, void *data)
{
	const char ***filter_args = data;

	return argv_append(filter_args, name) ? OK : ERR;
}

static void
filter_rev_parse(const char ***args, const char *arg1, const char *arg2, const char *argv[])
{
	const char *rev_parse_argv[SIZEOF_ARG] = { "git", "rev-parse", arg1, arg2 };
	const char **all_argv = NULL;

	if (!argv_append_array(&all_argv, rev_parse_argv) ||
	    !argv_append_array(&all_argv, argv) ||
	    io_run_load(all_argv, "\n", read_filter_args, args) == ERR)
		die("Failed to split arguments");
	argv_free(all_argv);
	free(all_argv);
}

static bool
is_rev_flag(const char *flag)
{
	static const char *rev_flags[] = { GIT_REV_FLAGS };
	int i;

	for (i = 0; i < ARRAY_SIZE(rev_flags); i++)
		if (!strcmp(flag, rev_flags[i]))
			return TRUE;

	return FALSE;
}

static void
filter_options(const char *argv[], bool blame)
{
	const char **flags = NULL;
	int next, flags_pos;

	update_options_from_argv(argv);

	filter_rev_parse(&opt_file_argv, "--no-revs", "--no-flags", argv);
	filter_rev_parse(&flags, "--flags", "--no-revs", argv);

	if (flags) {
		for (next = flags_pos = 0; flags && flags[next]; next++) {
			const char *flag = flags[next];

			if (is_rev_flag(flag))
				argv_append(&opt_rev_argv, flag);
			else
				flags[flags_pos++] = flag;
		}

		flags[flags_pos] = NULL;

		if (blame)
			opt_blame_options = flags;
		else
			opt_cmdline_argv = flags;
	}

	filter_rev_parse(&opt_rev_argv, "--symbolic", "--revs-only", argv);
}

static enum request
parse_options(int argc, const char *argv[], bool pager_mode)
{
	enum request request;
	const char *subcommand;
	bool seen_dashdash = FALSE;
	const char **filter_argv = NULL;
	int i;

	request = pager_mode ? REQ_VIEW_PAGER : REQ_VIEW_MAIN;

	if (argc <= 1)
		return request;

	subcommand = argv[1];
	if (!strcmp(subcommand, "status")) {
		request = REQ_VIEW_STATUS;

	} else if (!strcmp(subcommand, "blame")) {
		request = REQ_VIEW_BLAME;

	} else if (!strcmp(subcommand, "show")) {
		request = REQ_VIEW_DIFF;

	} else if (!strcmp(subcommand, "log")) {
		request = REQ_VIEW_LOG;

	} else if (!strcmp(subcommand, "stash")) {
		request = REQ_VIEW_STASH;

	} else {
		subcommand = NULL;
	}

	for (i = 1 + !!subcommand; i < argc; i++) {
		const char *opt = argv[i];

		// stop parsing our options after -- and let rev-parse handle the rest
		if (!seen_dashdash) {
			if (!strcmp(opt, "--")) {
				seen_dashdash = TRUE;
				continue;

			} else if (!strcmp(opt, "-v") || !strcmp(opt, "--version")) {
				printf("tig version %s\n", TIG_VERSION);
				quit(0);

			} else if (!strcmp(opt, "-h") || !strcmp(opt, "--help")) {
				printf("%s\n", usage);
				quit(0);

			} else if (strlen(opt) >= 2 && *opt == '+' && string_isnumber(opt + 1)) {
				int lineno = atoi(opt + 1);

				view_env.lineno = lineno > 0 ? lineno - 1 : 0;
				continue;

			}
		}

		if (!argv_append(&filter_argv, opt))
			die("command too long");
	}

	if (filter_argv)
		filter_options(filter_argv, request == REQ_VIEW_BLAME);

	/* Finish validating and setting up blame options */
	if (request == REQ_VIEW_BLAME) {
		if (!opt_file_argv || opt_file_argv[1] || (opt_rev_argv && opt_rev_argv[1]))
			die("invalid number of options to blame\n\n%s", usage);

		if (opt_rev_argv) {
			string_ncopy(view_env.ref, opt_rev_argv[0], strlen(opt_rev_argv[0]));
		}

		string_ncopy(view_env.file, opt_file_argv[0], strlen(opt_file_argv[0]));
	}

	return request;
}

static enum request
open_pager_mode(enum request request)
{
	enum open_flags flags = OPEN_DEFAULT;

	if (request == REQ_VIEW_PAGER) {
		/* Detect if the user requested the main view. */
		if (argv_contains(opt_rev_argv, "--stdin")) {
			request = REQ_VIEW_MAIN;
			flags |= OPEN_FORWARD_STDIN;
		} else if (argv_contains(opt_cmdline_argv, "--pretty=raw")) {
			request = REQ_VIEW_MAIN;
			flags |= OPEN_STDIN;
		} else {
			flags |= OPEN_STDIN;
		}

	} else if (request == REQ_VIEW_DIFF) {
		if (argv_contains(opt_rev_argv, "--stdin"))
			flags |= OPEN_FORWARD_STDIN;
	}

	/* Open the requested view even if the pager mode is enabled so
	 * the warning message below is displayed correctly. */
	open_view(NULL, request, flags);

	if (!open_in_pager_mode(flags)) {
		close(STDIN_FILENO);
		report("Ignoring stdin.");
	}

	return REQ_NONE;
}

static enum request
run_prompt_command(struct view *view, char *cmd)
{
	enum request request;

	if (cmd && string_isnumber(cmd)) {
		int lineno = view->pos.lineno + 1;

		if (parse_int(&lineno, cmd, 1, view->lines + 1) == SUCCESS) {
			select_view_line(view, lineno - 1);
			report_clear();
		} else {
			report("Unable to parse '%s' as a line number", cmd);
		}
	} else if (cmd && iscommit(cmd)) {
		string_ncopy(view->env->search, cmd, strlen(cmd));

		request = view_request(view, REQ_JUMP_COMMIT);
		if (request == REQ_JUMP_COMMIT) {
			report("Jumping to commits is not supported by the '%s' view", view->name);
		}

	} else if (cmd && strlen(cmd) == 1) {
		request = get_keybinding(&view->ops->keymap, cmd[0]);
		return request;

	} else if (cmd && cmd[0] == '!') {
		struct view *next = VIEW(REQ_VIEW_PAGER);
		const char *argv[SIZEOF_ARG];
		int argc = 0;

		cmd++;
		/* When running random commands, initially show the
		 * command in the title. However, it maybe later be
		 * overwritten if a commit line is selected. */
		string_ncopy(next->ref, cmd, strlen(cmd));

		if (!argv_from_string(argv, &argc, cmd)) {
			report("Too many arguments");
		} else if (!format_argv(view, &next->argv, argv, FALSE, TRUE)) {
			report("Argument formatting failed");
		} else {
			next->dir = NULL;
			open_view(view, REQ_VIEW_PAGER, OPEN_PREPARED);
		}

	} else if (cmd) {
		request = get_request(cmd);
		if (request != REQ_UNKNOWN)
			return request;

		char *args = strchr(cmd, ' ');
		if (args) {
			*args++ = 0;
			if (set_option(cmd, args) == SUCCESS) {
				request = !view->unrefreshable ? REQ_REFRESH : REQ_SCREEN_REDRAW;
				if (!strcmp(cmd, "color"))
					init_colors();
			}
		}
		return request;
	}
	return REQ_NONE;
}

#ifdef NCURSES_MOUSE_VERSION
static struct view *
find_clicked_view(MEVENT *event)
{
	struct view *view;
	int i;

	foreach_displayed_view (view, i) {
		int beg_y = 0, beg_x = 0;

		getbegyx(view->win, beg_y, beg_x);

		if (beg_y <= event->y && event->y < beg_y + view->height
		    && beg_x <= event->x && event->x < beg_x + view->width) {
			if (i != current_view) {
				current_view = i;
			}
			return view;
		}
	}

	return NULL;
}

static enum request
handle_mouse_event(void)
{
	MEVENT event;
	struct view *view;

	if (getmouse(&event) != OK)
		return REQ_NONE;

	view = find_clicked_view(&event);
	if (!view)
		return REQ_NONE;

	if (event.bstate & BUTTON2_PRESSED)
		return REQ_SCROLL_WHEEL_DOWN;

	if (event.bstate & BUTTON4_PRESSED)
		return REQ_SCROLL_WHEEL_UP;

	if (event.bstate & BUTTON1_PRESSED) {
		if (event.y == view->pos.lineno - view->pos.offset) {
			/* Click is on the same line, perform an "ENTER" */
			return REQ_ENTER;

		} else {
			int y = getbegy(view->win);
			unsigned long lineno = (event.y - y) + view->pos.offset;

			select_view_line(view, lineno);
			update_view_title(view);
			report_clear();
		}
	}

	return REQ_NONE;
}
#endif

int
main(int argc, const char *argv[])
{
	const char *codeset = ENCODING_UTF8;
	bool pager_mode = !isatty(STDIN_FILENO);
	enum request request = parse_options(argc, argv, pager_mode);
	struct view *view;
	int i;

	signal(SIGINT, quit);
	signal(SIGQUIT, quit);
	signal(SIGPIPE, SIG_IGN);

	if (setlocale(LC_ALL, "")) {
		codeset = nl_langinfo(CODESET);
	}

	foreach_view(view, i) {
		add_keymap(&view->ops->keymap);
	}

	if (load_repo_info() == ERR)
		die("Failed to load repo info.");

	if (load_options() == ERR)
		die("Failed to load user config.");

	if (load_git_config() == ERR)
		die("Failed to load repo config.");

	/* Require a git repository unless when running in pager mode. */
	if (!repo.git_dir[0] && request != REQ_VIEW_PAGER)
		die("Not a git repository");

	if (codeset && strcmp(codeset, ENCODING_UTF8)) {
		char translit[SIZEOF_STR];

		if (string_format(translit, "%s%s", codeset, ICONV_TRANSLIT))
			opt_iconv_out = iconv_open(translit, ENCODING_UTF8);
		else
			opt_iconv_out = iconv_open(codeset, ENCODING_UTF8);
		if (opt_iconv_out == ICONV_NONE)
			die("Failed to initialize character set conversion");
	}

	if (load_refs(FALSE) == ERR)
		die("Failed to load refs.");

	init_display();

	if (pager_mode)
		request = open_pager_mode(request);

	while (view_driver(display[current_view], request)) {
		int key = get_input(0);

#ifdef NCURSES_MOUSE_VERSION
		if (key == KEY_MOUSE) {
			request = handle_mouse_event();
			continue;
		}
#endif

		if (key == KEY_ESC)
			key  = get_input(0) + 0x80;

		view = display[current_view];
		request = get_keybinding(&view->ops->keymap, key);

		/* Some low-level request handling. This keeps access to
		 * status_win restricted. */
		switch (request) {
		case REQ_NONE:
			report("Unknown key, press %s for help",
			       get_view_key(view, REQ_VIEW_HELP));
			break;
		case REQ_PROMPT:
		{
			char *cmd = read_prompt(":");
			request = run_prompt_command(view, cmd);
			break;
		}
		case REQ_SEARCH:
		case REQ_SEARCH_BACK:
		{
			const char *prompt = request == REQ_SEARCH ? "/" : "?";
			char *search = read_prompt(prompt);

			if (search)
				string_ncopy(view_env.search, search, strlen(search));
			else if (*view_env.search)
				request = request == REQ_SEARCH ?
					REQ_FIND_NEXT :
					REQ_FIND_PREV;
			else
				request = REQ_NONE;
			break;
		}
		default:
			break;
		}
	}

	quit(0);

	return 0;
}

/* vim: set ts=8 sw=8 noexpandtab: */
