/* Copyright (c) 2006-2009 Jonas Fonseca <fonseca@diku.dk>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef TIG_VERSION
#define TIG_VERSION "unknown-version"
#endif

#ifndef DEBUG
#define NDEBUG
#endif

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>

#include <regex.h>

#include <locale.h>
#include <langinfo.h>
#include <iconv.h>

/* ncurses(3): Must be defined to have extended wide-character functions. */
#define _XOPEN_SOURCE_EXTENDED

#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#else
#ifdef HAVE_NCURSES_NCURSES_H
#include <ncurses/ncurses.h>
#else
#include <ncurses.h>
#endif
#endif

#if __GNUC__ >= 3
#define __NORETURN __attribute__((__noreturn__))
#else
#define __NORETURN
#endif

static void __NORETURN die(const char *err, ...);
static void warn(const char *msg, ...);
static void report(const char *msg, ...);
static void set_nonblocking_input(bool loading);
static int load_refs(void);
static size_t utf8_length(const char **string, size_t col, int *width, size_t max_width, int *trimmed, bool reserve);

#define ABS(x)		((x) >= 0  ? (x) : -(x))
#define MIN(x, y)	((x) < (y) ? (x) :  (y))

#define ARRAY_SIZE(x)	(sizeof(x) / sizeof(x[0]))
#define STRING_SIZE(x)	(sizeof(x) - 1)

#define SIZEOF_STR	1024	/* Default string size. */
#define SIZEOF_REF	256	/* Size of symbolic or SHA1 ID. */
#define SIZEOF_REV	41	/* Holds a SHA-1 and an ending NUL. */
#define SIZEOF_ARG	32	/* Default argument array size. */

/* Revision graph */

#define REVGRAPH_INIT	'I'
#define REVGRAPH_MERGE	'M'
#define REVGRAPH_BRANCH	'+'
#define REVGRAPH_COMMIT	'*'
#define REVGRAPH_BOUND	'^'

#define SIZEOF_REVGRAPH	19	/* Size of revision ancestry graphics. */

/* This color name can be used to refer to the default term colors. */
#define COLOR_DEFAULT	(-1)

#define ICONV_NONE	((iconv_t) -1)
#ifndef ICONV_CONST
#define ICONV_CONST	/* nothing */
#endif

/* The format and size of the date column in the main view. */
#define DATE_FORMAT	"%Y-%m-%d %H:%M"
#define DATE_COLS	STRING_SIZE("2006-04-29 14:21 ")

#define AUTHOR_COLS	20
#define ID_COLS		8

/* The default interval between line numbers. */
#define NUMBER_INTERVAL	5
#define SCROLL_INTERVAL	1

#define TAB_SIZE	8

#define	SCALE_SPLIT_VIEW(height)	((height) * 2 / 3)

#define NULL_ID		"0000000000000000000000000000000000000000"

#ifndef GIT_CONFIG
#define GIT_CONFIG "config"
#endif

/* Some ascii-shorthands fitted into the ncurses namespace. */
#define KEY_TAB		'\t'
#define KEY_RETURN	'\r'
#define KEY_ESC		27


struct ref {
	char *name;		/* Ref name; tag or head names are shortened. */
	char id[SIZEOF_REV];	/* Commit SHA1 ID */
	unsigned int head:1;	/* Is it the current HEAD? */
	unsigned int tag:1;	/* Is it a tag? */
	unsigned int ltag:1;	/* If so, is the tag local? */
	unsigned int remote:1;	/* Is it a remote ref? */
	unsigned int tracked:1;	/* Is it the remote for the current HEAD? */
	unsigned int next:1;	/* For ref lists: are there more refs? */
};

static struct ref **get_refs(const char *id);

enum format_flags {
	FORMAT_ALL,		/* Perform replacement in all arguments. */
	FORMAT_DASH,		/* Perform replacement up until "--". */
	FORMAT_NONE		/* No replacement should be performed. */
};

static bool format_argv(const char *dst[], const char *src[], enum format_flags flags);

struct int_map {
	const char *name;
	int namelen;
	int value;
};

static int
set_from_int_map(struct int_map *map, size_t map_size,
		 int *value, const char *name, int namelen)
{

	int i;

	for (i = 0; i < map_size; i++)
		if (namelen == map[i].namelen &&
		    !strncasecmp(name, map[i].name, namelen)) {
			*value = map[i].value;
			return OK;
		}

	return ERR;
}

enum input_status {
	INPUT_OK,
	INPUT_SKIP,
	INPUT_STOP,
	INPUT_CANCEL
};

typedef enum input_status (*input_handler)(void *data, char *buf, int c);

static char *prompt_input(const char *prompt, input_handler handler, void *data);
static bool prompt_yesno(const char *prompt);

/*
 * String helpers
 */

static inline void
string_ncopy_do(char *dst, size_t dstlen, const char *src, size_t srclen)
{
	if (srclen > dstlen - 1)
		srclen = dstlen - 1;

	strncpy(dst, src, srclen);
	dst[srclen] = 0;
}

/* Shorthands for safely copying into a fixed buffer. */

#define string_copy(dst, src) \
	string_ncopy_do(dst, sizeof(dst), src, sizeof(src))

#define string_ncopy(dst, src, srclen) \
	string_ncopy_do(dst, sizeof(dst), src, srclen)

#define string_copy_rev(dst, src) \
	string_ncopy_do(dst, SIZEOF_REV, src, SIZEOF_REV - 1)

#define string_add(dst, from, src) \
	string_ncopy_do(dst + (from), sizeof(dst) - (from), src, sizeof(src))

static size_t
string_expand_length(const char *line, int tabsize)
{
	size_t size, pos;

	for (pos = 0; line[pos]; pos++) {
		if (line[pos] == '\t' && tabsize > 0)
			size += tabsize - (size % tabsize);
		else
			size++;
	}
	return size;
}

static void
string_expand(char *dst, size_t dstlen, const char *src, int tabsize)
{
	size_t size, pos;

	for (size = pos = 0; size < dstlen - 1 && src[pos]; pos++) {
		if (src[pos] == '\t') {
			size_t expanded = tabsize - (size % tabsize);

			if (expanded + size >= dstlen - 1)
				expanded = dstlen - size - 1;
			memcpy(dst + size, "        ", expanded);
			size += expanded;
		} else {
			dst[size++] = src[pos];
		}
	}

	dst[size] = 0;
}

static char *
chomp_string(char *name)
{
	int namelen;

	while (isspace(*name))
		name++;

	namelen = strlen(name) - 1;
	while (namelen > 0 && isspace(name[namelen]))
		name[namelen--] = 0;

	return name;
}

static bool
string_nformat(char *buf, size_t bufsize, size_t *bufpos, const char *fmt, ...)
{
	va_list args;
	size_t pos = bufpos ? *bufpos : 0;

	va_start(args, fmt);
	pos += vsnprintf(buf + pos, bufsize - pos, fmt, args);
	va_end(args);

	if (bufpos)
		*bufpos = pos;

	return pos >= bufsize ? FALSE : TRUE;
}

#define string_format(buf, fmt, args...) \
	string_nformat(buf, sizeof(buf), NULL, fmt, args)

#define string_format_from(buf, from, fmt, args...) \
	string_nformat(buf, sizeof(buf), from, fmt, args)

static int
string_enum_compare(const char *str1, const char *str2, int len)
{
	size_t i;

#define string_enum_sep(x) ((x) == '-' || (x) == '_' || (x) == '.')

	/* Diff-Header == DIFF_HEADER */
	for (i = 0; i < len; i++) {
		if (toupper(str1[i]) == toupper(str2[i]))
			continue;

		if (string_enum_sep(str1[i]) &&
		    string_enum_sep(str2[i]))
			continue;

		return str1[i] - str2[i];
	}

	return 0;
}

#define prefixcmp(str1, str2) \
	strncmp(str1, str2, STRING_SIZE(str2))

static inline int
suffixcmp(const char *str, int slen, const char *suffix)
{
	size_t len = slen >= 0 ? slen : strlen(str);
	size_t suffixlen = strlen(suffix);

	return suffixlen < len ? strcmp(str + len - suffixlen, suffix) : -1;
}


static bool
argv_from_string(const char *argv[SIZEOF_ARG], int *argc, char *cmd)
{
	int valuelen;

	while (*cmd && *argc < SIZEOF_ARG && (valuelen = strcspn(cmd, " \t"))) {
		bool advance = cmd[valuelen] != 0;

		cmd[valuelen] = 0;
		argv[(*argc)++] = chomp_string(cmd);
		cmd = chomp_string(cmd + valuelen + advance);
	}

	if (*argc < SIZEOF_ARG)
		argv[*argc] = NULL;
	return *argc < SIZEOF_ARG;
}

static void
argv_from_env(const char **argv, const char *name)
{
	char *env = argv ? getenv(name) : NULL;
	int argc = 0;

	if (env && *env)
		env = strdup(env);
	if (env && !argv_from_string(argv, &argc, env))
		die("Too many arguments in the `%s` environment variable", name);
}


/*
 * Executing external commands.
 */

enum io_type {
	IO_FD,			/* File descriptor based IO. */
	IO_BG,			/* Execute command in the background. */
	IO_FG,			/* Execute command with same std{in,out,err}. */
	IO_RD,			/* Read only fork+exec IO. */
	IO_WR,			/* Write only fork+exec IO. */
	IO_AP,			/* Append fork+exec output to file. */
};

struct io {
	enum io_type type;	/* The requested type of pipe. */
	const char *dir;	/* Directory from which to execute. */
	pid_t pid;		/* Pipe for reading or writing. */
	int pipe;		/* Pipe end for reading or writing. */
	int error;		/* Error status. */
	const char *argv[SIZEOF_ARG];	/* Shell command arguments. */
	char *buf;		/* Read buffer. */
	size_t bufalloc;	/* Allocated buffer size. */
	size_t bufsize;		/* Buffer content size. */
	char *bufpos;		/* Current buffer position. */
	unsigned int eof:1;	/* Has end of file been reached. */
};

static void
reset_io(struct io *io)
{
	io->pipe = -1;
	io->pid = 0;
	io->buf = io->bufpos = NULL;
	io->bufalloc = io->bufsize = 0;
	io->error = 0;
	io->eof = 0;
}

static void
init_io(struct io *io, const char *dir, enum io_type type)
{
	reset_io(io);
	io->type = type;
	io->dir = dir;
}

static bool
init_io_rd(struct io *io, const char *argv[], const char *dir,
		enum format_flags flags)
{
	init_io(io, dir, IO_RD);
	return format_argv(io->argv, argv, flags);
}

static bool
io_open(struct io *io, const char *name)
{
	init_io(io, NULL, IO_FD);
	io->pipe = *name ? open(name, O_RDONLY) : STDIN_FILENO;
	return io->pipe != -1;
}

static bool
kill_io(struct io *io)
{
	return io->pid == 0 || kill(io->pid, SIGKILL) != -1;
}

static bool
done_io(struct io *io)
{
	pid_t pid = io->pid;

	if (io->pipe != -1)
		close(io->pipe);
	free(io->buf);
	reset_io(io);

	while (pid > 0) {
		int status;
		pid_t waiting = waitpid(pid, &status, 0);

		if (waiting < 0) {
			if (errno == EINTR)
				continue;
			report("waitpid failed (%s)", strerror(errno));
			return FALSE;
		}

		return waiting == pid &&
		       !WIFSIGNALED(status) &&
		       WIFEXITED(status) &&
		       !WEXITSTATUS(status);
	}

	return TRUE;
}

static bool
start_io(struct io *io)
{
	int pipefds[2] = { -1, -1 };

	if (io->type == IO_FD)
		return TRUE;

	if ((io->type == IO_RD || io->type == IO_WR) &&
	    pipe(pipefds) < 0)
		return FALSE;
	else if (io->type == IO_AP)
		pipefds[1] = io->pipe;

	if ((io->pid = fork())) {
		if (pipefds[!(io->type == IO_WR)] != -1)
			close(pipefds[!(io->type == IO_WR)]);
		if (io->pid != -1) {
			io->pipe = pipefds[!!(io->type == IO_WR)];
			return TRUE;
		}

	} else {
		if (io->type != IO_FG) {
			int devnull = open("/dev/null", O_RDWR);
			int readfd  = io->type == IO_WR ? pipefds[0] : devnull;
			int writefd = (io->type == IO_RD || io->type == IO_AP)
							? pipefds[1] : devnull;

			dup2(readfd,  STDIN_FILENO);
			dup2(writefd, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);

			close(devnull);
			if (pipefds[0] != -1)
				close(pipefds[0]);
			if (pipefds[1] != -1)
				close(pipefds[1]);
		}

		if (io->dir && *io->dir && chdir(io->dir) == -1)
			die("Failed to change directory: %s", strerror(errno));

		execvp(io->argv[0], (char *const*) io->argv);
		die("Failed to execute program: %s", strerror(errno));
	}

	if (pipefds[!!(io->type == IO_WR)] != -1)
		close(pipefds[!!(io->type == IO_WR)]);
	return FALSE;
}

static bool
run_io(struct io *io, const char **argv, const char *dir, enum io_type type)
{
	init_io(io, dir, type);
	if (!format_argv(io->argv, argv, FORMAT_NONE))
		return FALSE;
	return start_io(io);
}

static int
run_io_do(struct io *io)
{
	return start_io(io) && done_io(io);
}

static int
run_io_bg(const char **argv)
{
	struct io io = {};

	init_io(&io, NULL, IO_BG);
	if (!format_argv(io.argv, argv, FORMAT_NONE))
		return FALSE;
	return run_io_do(&io);
}

static bool
run_io_fg(const char **argv, const char *dir)
{
	struct io io = {};

	init_io(&io, dir, IO_FG);
	if (!format_argv(io.argv, argv, FORMAT_NONE))
		return FALSE;
	return run_io_do(&io);
}

static bool
run_io_append(const char **argv, enum format_flags flags, int fd)
{
	struct io io = {};

	init_io(&io, NULL, IO_AP);
	io.pipe = fd;
	if (format_argv(io.argv, argv, flags))
		return run_io_do(&io);
	close(fd);
	return FALSE;
}

static bool
run_io_rd(struct io *io, const char **argv, enum format_flags flags)
{
	return init_io_rd(io, argv, NULL, flags) && start_io(io);
}

static bool
io_eof(struct io *io)
{
	return io->eof;
}

static int
io_error(struct io *io)
{
	return io->error;
}

static bool
io_strerror(struct io *io)
{
	return strerror(io->error);
}

static bool
io_can_read(struct io *io)
{
	struct timeval tv = { 0, 500 };
	fd_set fds;

	FD_ZERO(&fds);
	FD_SET(io->pipe, &fds);

	return select(io->pipe + 1, &fds, NULL, NULL, &tv) > 0;
}

static ssize_t
io_read(struct io *io, void *buf, size_t bufsize)
{
	do {
		ssize_t readsize = read(io->pipe, buf, bufsize);

		if (readsize < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		else if (readsize == -1)
			io->error = errno;
		else if (readsize == 0)
			io->eof = 1;
		return readsize;
	} while (1);
}

static char *
io_get(struct io *io, int c, bool can_read)
{
	char *eol;
	ssize_t readsize;

	if (!io->buf) {
		io->buf = io->bufpos = malloc(BUFSIZ);
		if (!io->buf)
			return NULL;
		io->bufalloc = BUFSIZ;
		io->bufsize = 0;
	}

	while (TRUE) {
		if (io->bufsize > 0) {
			eol = memchr(io->bufpos, c, io->bufsize);
			if (eol) {
				char *line = io->bufpos;

				*eol = 0;
				io->bufpos = eol + 1;
				io->bufsize -= io->bufpos - line;
				return line;
			}
		}

		if (io_eof(io)) {
			if (io->bufsize) {
				io->bufpos[io->bufsize] = 0;
				io->bufsize = 0;
				return io->bufpos;
			}
			return NULL;
		}

		if (!can_read)
			return NULL;

		if (io->bufsize > 0 && io->bufpos > io->buf)
			memmove(io->buf, io->bufpos, io->bufsize);

		io->bufpos = io->buf;
		readsize = io_read(io, io->buf + io->bufsize, io->bufalloc - io->bufsize);
		if (io_error(io))
			return NULL;
		io->bufsize += readsize;
	}
}

static bool
io_write(struct io *io, const void *buf, size_t bufsize)
{
	size_t written = 0;

	while (!io_error(io) && written < bufsize) {
		ssize_t size;

		size = write(io->pipe, buf + written, bufsize - written);
		if (size < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		else if (size == -1)
			io->error = errno;
		else
			written += size;
	}

	return written == bufsize;
}

static bool
run_io_buf(const char **argv, char buf[], size_t bufsize)
{
	struct io io = {};
	bool error;

	if (!run_io_rd(&io, argv, FORMAT_NONE))
		return FALSE;

	io.buf = io.bufpos = buf;
	io.bufalloc = bufsize;
	error = !io_get(&io, '\n', TRUE) && io_error(&io);
	io.buf = NULL;

	return done_io(&io) || error;
}

static int
io_load(struct io *io, const char *separators,
	int (*read_property)(char *, size_t, char *, size_t))
{
	char *name;
	int state = OK;

	if (!start_io(io))
		return ERR;

	while (state == OK && (name = io_get(io, '\n', TRUE))) {
		char *value;
		size_t namelen;
		size_t valuelen;

		name = chomp_string(name);
		namelen = strcspn(name, separators);

		if (name[namelen]) {
			name[namelen] = 0;
			value = chomp_string(name + namelen + 1);
			valuelen = strlen(value);

		} else {
			value = "";
			valuelen = 0;
		}

		state = read_property(name, namelen, value, valuelen);
	}

	if (state != ERR && io_error(io))
		state = ERR;
	done_io(io);

	return state;
}

static int
run_io_load(const char **argv, const char *separators,
	    int (*read_property)(char *, size_t, char *, size_t))
{
	struct io io = {};

	return init_io_rd(&io, argv, NULL, FORMAT_NONE)
		? io_load(&io, separators, read_property) : ERR;
}


/*
 * User requests
 */

#define REQ_INFO \
	/* XXX: Keep the view request first and in sync with views[]. */ \
	REQ_GROUP("View switching") \
	REQ_(VIEW_MAIN,		"Show main view"), \
	REQ_(VIEW_DIFF,		"Show diff view"), \
	REQ_(VIEW_LOG,		"Show log view"), \
	REQ_(VIEW_TREE,		"Show tree view"), \
	REQ_(VIEW_BLOB,		"Show blob view"), \
	REQ_(VIEW_BLAME,	"Show blame view"), \
	REQ_(VIEW_HELP,		"Show help page"), \
	REQ_(VIEW_PAGER,	"Show pager view"), \
	REQ_(VIEW_STATUS,	"Show status view"), \
	REQ_(VIEW_STAGE,	"Show stage view"), \
	\
	REQ_GROUP("View manipulation") \
	REQ_(ENTER,		"Enter current line and scroll"), \
	REQ_(NEXT,		"Move to next"), \
	REQ_(PREVIOUS,		"Move to previous"), \
	REQ_(PARENT,		"Move to parent"), \
	REQ_(VIEW_NEXT,		"Move focus to next view"), \
	REQ_(REFRESH,		"Reload and refresh"), \
	REQ_(MAXIMIZE,		"Maximize the current view"), \
	REQ_(VIEW_CLOSE,	"Close the current view"), \
	REQ_(QUIT,		"Close all views and quit"), \
	\
	REQ_GROUP("View specific requests") \
	REQ_(STATUS_UPDATE,	"Update file status"), \
	REQ_(STATUS_REVERT,	"Revert file changes"), \
	REQ_(STATUS_MERGE,	"Merge file using external tool"), \
	REQ_(STAGE_NEXT,	"Find next chunk to stage"), \
	\
	REQ_GROUP("Cursor navigation") \
	REQ_(MOVE_UP,		"Move cursor one line up"), \
	REQ_(MOVE_DOWN,		"Move cursor one line down"), \
	REQ_(MOVE_PAGE_DOWN,	"Move cursor one page down"), \
	REQ_(MOVE_PAGE_UP,	"Move cursor one page up"), \
	REQ_(MOVE_FIRST_LINE,	"Move cursor to first line"), \
	REQ_(MOVE_LAST_LINE,	"Move cursor to last line"), \
	\
	REQ_GROUP("Scrolling") \
	REQ_(SCROLL_LEFT,	"Scroll two columns left"), \
	REQ_(SCROLL_RIGHT,	"Scroll two columns right"), \
	REQ_(SCROLL_LINE_UP,	"Scroll one line up"), \
	REQ_(SCROLL_LINE_DOWN,	"Scroll one line down"), \
	REQ_(SCROLL_PAGE_UP,	"Scroll one page up"), \
	REQ_(SCROLL_PAGE_DOWN,	"Scroll one page down"), \
	\
	REQ_GROUP("Searching") \
	REQ_(SEARCH,		"Search the view"), \
	REQ_(SEARCH_BACK,	"Search backwards in the view"), \
	REQ_(FIND_NEXT,		"Find next search match"), \
	REQ_(FIND_PREV,		"Find previous search match"), \
	\
	REQ_GROUP("Option manipulation") \
	REQ_(TOGGLE_LINENO,	"Toggle line numbers"), \
	REQ_(TOGGLE_DATE,	"Toggle date display"), \
	REQ_(TOGGLE_AUTHOR,	"Toggle author display"), \
	REQ_(TOGGLE_REV_GRAPH,	"Toggle revision graph visualization"), \
	REQ_(TOGGLE_REFS,	"Toggle reference display (tags/branches)"), \
	\
	REQ_GROUP("Misc") \
	REQ_(PROMPT,		"Bring up the prompt"), \
	REQ_(SCREEN_REDRAW,	"Redraw the screen"), \
	REQ_(SHOW_VERSION,	"Show version information"), \
	REQ_(STOP_LOADING,	"Stop all loading views"), \
	REQ_(EDIT,		"Open in editor"), \
	REQ_(NONE,		"Do nothing")


/* User action requests. */
enum request {
#define REQ_GROUP(help)
#define REQ_(req, help) REQ_##req

	/* Offset all requests to avoid conflicts with ncurses getch values. */
	REQ_OFFSET = KEY_MAX + 1,
	REQ_INFO

#undef	REQ_GROUP
#undef	REQ_
};

struct request_info {
	enum request request;
	const char *name;
	int namelen;
	const char *help;
};

static struct request_info req_info[] = {
#define REQ_GROUP(help)	{ 0, NULL, 0, (help) },
#define REQ_(req, help)	{ REQ_##req, (#req), STRING_SIZE(#req), (help) }
	REQ_INFO
#undef	REQ_GROUP
#undef	REQ_
};

static enum request
get_request(const char *name)
{
	int namelen = strlen(name);
	int i;

	for (i = 0; i < ARRAY_SIZE(req_info); i++)
		if (req_info[i].namelen == namelen &&
		    !string_enum_compare(req_info[i].name, name, namelen))
			return req_info[i].request;

	return REQ_NONE;
}


/*
 * Options
 */

/* Option and state variables. */
static bool opt_date			= TRUE;
static bool opt_author			= TRUE;
static bool opt_line_number		= FALSE;
static bool opt_line_graphics		= TRUE;
static bool opt_rev_graph		= FALSE;
static bool opt_show_refs		= TRUE;
static int opt_num_interval		= NUMBER_INTERVAL;
static int opt_tab_size			= TAB_SIZE;
static int opt_author_cols		= AUTHOR_COLS-1;
static char opt_path[SIZEOF_STR]	= "";
static char opt_file[SIZEOF_STR]	= "";
static char opt_ref[SIZEOF_REF]		= "";
static char opt_head[SIZEOF_REF]	= "";
static char opt_head_rev[SIZEOF_REV]	= "";
static char opt_remote[SIZEOF_REF]	= "";
static char opt_encoding[20]		= "UTF-8";
static bool opt_utf8			= TRUE;
static char opt_codeset[20]		= "UTF-8";
static iconv_t opt_iconv		= ICONV_NONE;
static char opt_search[SIZEOF_STR]	= "";
static char opt_cdup[SIZEOF_STR]	= "";
static char opt_prefix[SIZEOF_STR]	= "";
static char opt_git_dir[SIZEOF_STR]	= "";
static signed char opt_is_inside_work_tree	= -1; /* set to TRUE or FALSE */
static char opt_editor[SIZEOF_STR]	= "";
static FILE *opt_tty			= NULL;

#define is_initial_commit()	(!*opt_head_rev)
#define is_head_commit(rev)	(!strcmp((rev), "HEAD") || !strcmp(opt_head_rev, (rev)))


/*
 * Line-oriented content detection.
 */

#define LINE_INFO \
LINE(DIFF_HEADER,  "diff --git ",	COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_CHUNK,   "@@",		COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(DIFF_ADD,	   "+",			COLOR_GREEN,	COLOR_DEFAULT,	0), \
LINE(DIFF_DEL,	   "-",			COLOR_RED,	COLOR_DEFAULT,	0), \
LINE(DIFF_INDEX,	"index ",	  COLOR_BLUE,	COLOR_DEFAULT,	0), \
LINE(DIFF_OLDMODE,	"old file mode ", COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_NEWMODE,	"new file mode ", COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_COPY_FROM,	"copy from",	  COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_COPY_TO,	"copy to",	  COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_RENAME_FROM,	"rename from",	  COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_RENAME_TO,	"rename to",	  COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_SIMILARITY,   "similarity ",	  COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_DISSIMILARITY,"dissimilarity ", COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DIFF_TREE,		"diff-tree ",	  COLOR_BLUE,	COLOR_DEFAULT,	0), \
LINE(PP_AUTHOR,	   "Author: ",		COLOR_CYAN,	COLOR_DEFAULT,	0), \
LINE(PP_COMMIT,	   "Commit: ",		COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(PP_MERGE,	   "Merge: ",		COLOR_BLUE,	COLOR_DEFAULT,	0), \
LINE(PP_DATE,	   "Date:   ",		COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(PP_ADATE,	   "AuthorDate: ",	COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(PP_CDATE,	   "CommitDate: ",	COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(PP_REFS,	   "Refs: ",		COLOR_RED,	COLOR_DEFAULT,	0), \
LINE(COMMIT,	   "commit ",		COLOR_GREEN,	COLOR_DEFAULT,	0), \
LINE(PARENT,	   "parent ",		COLOR_BLUE,	COLOR_DEFAULT,	0), \
LINE(TREE,	   "tree ",		COLOR_BLUE,	COLOR_DEFAULT,	0), \
LINE(AUTHOR,	   "author ",		COLOR_GREEN,	COLOR_DEFAULT,	0), \
LINE(COMMITTER,	   "committer ",	COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(SIGNOFF,	   "    Signed-off-by", COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(ACKED,	   "    Acked-by",	COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(DEFAULT,	   "",			COLOR_DEFAULT,	COLOR_DEFAULT,	A_NORMAL), \
LINE(CURSOR,	   "",			COLOR_WHITE,	COLOR_GREEN,	A_BOLD), \
LINE(STATUS,	   "",			COLOR_GREEN,	COLOR_DEFAULT,	0), \
LINE(DELIMITER,	   "",			COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(DATE,         "",			COLOR_BLUE,	COLOR_DEFAULT,	0), \
LINE(MODE,         "",			COLOR_CYAN,	COLOR_DEFAULT,	0), \
LINE(LINE_NUMBER,  "",			COLOR_CYAN,	COLOR_DEFAULT,	0), \
LINE(TITLE_BLUR,   "",			COLOR_WHITE,	COLOR_BLUE,	0), \
LINE(TITLE_FOCUS,  "",			COLOR_WHITE,	COLOR_BLUE,	A_BOLD), \
LINE(MAIN_COMMIT,  "",			COLOR_DEFAULT,	COLOR_DEFAULT,	0), \
LINE(MAIN_TAG,     "",			COLOR_MAGENTA,	COLOR_DEFAULT,	A_BOLD), \
LINE(MAIN_LOCAL_TAG,"",			COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(MAIN_REMOTE,  "",			COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(MAIN_TRACKED, "",			COLOR_YELLOW,	COLOR_DEFAULT,	A_BOLD), \
LINE(MAIN_REF,     "",			COLOR_CYAN,	COLOR_DEFAULT,	0), \
LINE(MAIN_HEAD,    "",			COLOR_CYAN,	COLOR_DEFAULT,	A_BOLD), \
LINE(MAIN_REVGRAPH,"",			COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(TREE_PARENT,  "",			COLOR_DEFAULT,	COLOR_DEFAULT,	A_BOLD), \
LINE(TREE_DIR,     "",			COLOR_YELLOW,	COLOR_DEFAULT,	A_NORMAL), \
LINE(TREE_FILE,    "",			COLOR_DEFAULT,	COLOR_DEFAULT,	A_NORMAL), \
LINE(STAT_HEAD,    "",			COLOR_YELLOW,	COLOR_DEFAULT,	0), \
LINE(STAT_SECTION, "",			COLOR_CYAN,	COLOR_DEFAULT,	0), \
LINE(STAT_NONE,    "",			COLOR_DEFAULT,	COLOR_DEFAULT,	0), \
LINE(STAT_STAGED,  "",			COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(STAT_UNSTAGED,"",			COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(STAT_UNTRACKED,"",			COLOR_MAGENTA,	COLOR_DEFAULT,	0), \
LINE(BLAME_ID,     "",			COLOR_MAGENTA,	COLOR_DEFAULT,	0)

enum line_type {
#define LINE(type, line, fg, bg, attr) \
	LINE_##type
	LINE_INFO,
	LINE_NONE
#undef	LINE
};

struct line_info {
	const char *name;	/* Option name. */
	int namelen;		/* Size of option name. */
	const char *line;	/* The start of line to match. */
	int linelen;		/* Size of string to match. */
	int fg, bg, attr;	/* Color and text attributes for the lines. */
};

static struct line_info line_info[] = {
#define LINE(type, line, fg, bg, attr) \
	{ #type, STRING_SIZE(#type), (line), STRING_SIZE(line), (fg), (bg), (attr) }
	LINE_INFO
#undef	LINE
};

static enum line_type
get_line_type(const char *line)
{
	int linelen = strlen(line);
	enum line_type type;

	for (type = 0; type < ARRAY_SIZE(line_info); type++)
		/* Case insensitive search matches Signed-off-by lines better. */
		if (linelen >= line_info[type].linelen &&
		    !strncasecmp(line_info[type].line, line, line_info[type].linelen))
			return type;

	return LINE_DEFAULT;
}

static inline int
get_line_attr(enum line_type type)
{
	assert(type < ARRAY_SIZE(line_info));
	return COLOR_PAIR(type) | line_info[type].attr;
}

static struct line_info *
get_line_info(const char *name)
{
	size_t namelen = strlen(name);
	enum line_type type;

	for (type = 0; type < ARRAY_SIZE(line_info); type++)
		if (namelen == line_info[type].namelen &&
		    !string_enum_compare(line_info[type].name, name, namelen))
			return &line_info[type];

	return NULL;
}

static void
init_colors(void)
{
	int default_bg = line_info[LINE_DEFAULT].bg;
	int default_fg = line_info[LINE_DEFAULT].fg;
	enum line_type type;

	start_color();

	if (assume_default_colors(default_fg, default_bg) == ERR) {
		default_bg = COLOR_BLACK;
		default_fg = COLOR_WHITE;
	}

	for (type = 0; type < ARRAY_SIZE(line_info); type++) {
		struct line_info *info = &line_info[type];
		int bg = info->bg == COLOR_DEFAULT ? default_bg : info->bg;
		int fg = info->fg == COLOR_DEFAULT ? default_fg : info->fg;

		init_pair(type, fg, bg);
	}
}

struct line {
	enum line_type type;

	/* State flags */
	unsigned int selected:1;
	unsigned int dirty:1;
	unsigned int cleareol:1;

	void *data;		/* User data */
};


/*
 * Keys
 */

struct keybinding {
	int alias;
	enum request request;
};

static struct keybinding default_keybindings[] = {
	/* View switching */
	{ 'm',		REQ_VIEW_MAIN },
	{ 'd',		REQ_VIEW_DIFF },
	{ 'l',		REQ_VIEW_LOG },
	{ 't',		REQ_VIEW_TREE },
	{ 'f',		REQ_VIEW_BLOB },
	{ 'B',		REQ_VIEW_BLAME },
	{ 'p',		REQ_VIEW_PAGER },
	{ 'h',		REQ_VIEW_HELP },
	{ 'S',		REQ_VIEW_STATUS },
	{ 'c',		REQ_VIEW_STAGE },

	/* View manipulation */
	{ 'q',		REQ_VIEW_CLOSE },
	{ KEY_TAB,	REQ_VIEW_NEXT },
	{ KEY_RETURN,	REQ_ENTER },
	{ KEY_UP,	REQ_PREVIOUS },
	{ KEY_DOWN,	REQ_NEXT },
	{ 'R',		REQ_REFRESH },
	{ KEY_F(5),	REQ_REFRESH },
	{ 'O',		REQ_MAXIMIZE },

	/* Cursor navigation */
	{ 'k',		REQ_MOVE_UP },
	{ 'j',		REQ_MOVE_DOWN },
	{ KEY_HOME,	REQ_MOVE_FIRST_LINE },
	{ KEY_END,	REQ_MOVE_LAST_LINE },
	{ KEY_NPAGE,	REQ_MOVE_PAGE_DOWN },
	{ ' ',		REQ_MOVE_PAGE_DOWN },
	{ KEY_PPAGE,	REQ_MOVE_PAGE_UP },
	{ 'b',		REQ_MOVE_PAGE_UP },
	{ '-',		REQ_MOVE_PAGE_UP },

	/* Scrolling */
	{ KEY_LEFT,	REQ_SCROLL_LEFT },
	{ KEY_RIGHT,	REQ_SCROLL_RIGHT },
	{ KEY_IC,	REQ_SCROLL_LINE_UP },
	{ KEY_DC,	REQ_SCROLL_LINE_DOWN },
	{ 'w',		REQ_SCROLL_PAGE_UP },
	{ 's',		REQ_SCROLL_PAGE_DOWN },

	/* Searching */
	{ '/',		REQ_SEARCH },
	{ '?',		REQ_SEARCH_BACK },
	{ 'n',		REQ_FIND_NEXT },
	{ 'N',		REQ_FIND_PREV },

	/* Misc */
	{ 'Q',		REQ_QUIT },
	{ 'z',		REQ_STOP_LOADING },
	{ 'v',		REQ_SHOW_VERSION },
	{ 'r',		REQ_SCREEN_REDRAW },
	{ '.',		REQ_TOGGLE_LINENO },
	{ 'D',		REQ_TOGGLE_DATE },
	{ 'A',		REQ_TOGGLE_AUTHOR },
	{ 'g',		REQ_TOGGLE_REV_GRAPH },
	{ 'F',		REQ_TOGGLE_REFS },
	{ ':',		REQ_PROMPT },
	{ 'u',		REQ_STATUS_UPDATE },
	{ '!',		REQ_STATUS_REVERT },
	{ 'M',		REQ_STATUS_MERGE },
	{ '@',		REQ_STAGE_NEXT },
	{ ',',		REQ_PARENT },
	{ 'e',		REQ_EDIT },
};

#define KEYMAP_INFO \
	KEYMAP_(GENERIC), \
	KEYMAP_(MAIN), \
	KEYMAP_(DIFF), \
	KEYMAP_(LOG), \
	KEYMAP_(TREE), \
	KEYMAP_(BLOB), \
	KEYMAP_(BLAME), \
	KEYMAP_(PAGER), \
	KEYMAP_(HELP), \
	KEYMAP_(STATUS), \
	KEYMAP_(STAGE)

enum keymap {
#define KEYMAP_(name) KEYMAP_##name
	KEYMAP_INFO
#undef	KEYMAP_
};

static struct int_map keymap_table[] = {
#define KEYMAP_(name) { #name, STRING_SIZE(#name), KEYMAP_##name }
	KEYMAP_INFO
#undef	KEYMAP_
};

#define set_keymap(map, name) \
	set_from_int_map(keymap_table, ARRAY_SIZE(keymap_table), map, name, strlen(name))

struct keybinding_table {
	struct keybinding *data;
	size_t size;
};

static struct keybinding_table keybindings[ARRAY_SIZE(keymap_table)];

static void
add_keybinding(enum keymap keymap, enum request request, int key)
{
	struct keybinding_table *table = &keybindings[keymap];

	table->data = realloc(table->data, (table->size + 1) * sizeof(*table->data));
	if (!table->data)
		die("Failed to allocate keybinding");
	table->data[table->size].alias = key;
	table->data[table->size++].request = request;
}

/* Looks for a key binding first in the given map, then in the generic map, and
 * lastly in the default keybindings. */
static enum request
get_keybinding(enum keymap keymap, int key)
{
	size_t i;

	for (i = 0; i < keybindings[keymap].size; i++)
		if (keybindings[keymap].data[i].alias == key)
			return keybindings[keymap].data[i].request;

	for (i = 0; i < keybindings[KEYMAP_GENERIC].size; i++)
		if (keybindings[KEYMAP_GENERIC].data[i].alias == key)
			return keybindings[KEYMAP_GENERIC].data[i].request;

	for (i = 0; i < ARRAY_SIZE(default_keybindings); i++)
		if (default_keybindings[i].alias == key)
			return default_keybindings[i].request;

	return (enum request) key;
}


struct key {
	const char *name;
	int value;
};

static struct key key_table[] = {
	{ "Enter",	KEY_RETURN },
	{ "Space",	' ' },
	{ "Backspace",	KEY_BACKSPACE },
	{ "Tab",	KEY_TAB },
	{ "Escape",	KEY_ESC },
	{ "Left",	KEY_LEFT },
	{ "Right",	KEY_RIGHT },
	{ "Up",		KEY_UP },
	{ "Down",	KEY_DOWN },
	{ "Insert",	KEY_IC },
	{ "Delete",	KEY_DC },
	{ "Hash",	'#' },
	{ "Home",	KEY_HOME },
	{ "End",	KEY_END },
	{ "PageUp",	KEY_PPAGE },
	{ "PageDown",	KEY_NPAGE },
	{ "F1",		KEY_F(1) },
	{ "F2",		KEY_F(2) },
	{ "F3",		KEY_F(3) },
	{ "F4",		KEY_F(4) },
	{ "F5",		KEY_F(5) },
	{ "F6",		KEY_F(6) },
	{ "F7",		KEY_F(7) },
	{ "F8",		KEY_F(8) },
	{ "F9",		KEY_F(9) },
	{ "F10",	KEY_F(10) },
	{ "F11",	KEY_F(11) },
	{ "F12",	KEY_F(12) },
};

static int
get_key_value(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(key_table); i++)
		if (!strcasecmp(key_table[i].name, name))
			return key_table[i].value;

	if (strlen(name) == 1 && isprint(*name))
		return (int) *name;

	return ERR;
}

static const char *
get_key_name(int key_value)
{
	static char key_char[] = "'X'";
	const char *seq = NULL;
	int key;

	for (key = 0; key < ARRAY_SIZE(key_table); key++)
		if (key_table[key].value == key_value)
			seq = key_table[key].name;

	if (seq == NULL &&
	    key_value < 127 &&
	    isprint(key_value)) {
		key_char[1] = (char) key_value;
		seq = key_char;
	}

	return seq ? seq : "(no key)";
}

static const char *
get_key(enum request request)
{
	static char buf[BUFSIZ];
	size_t pos = 0;
	char *sep = "";
	int i;

	buf[pos] = 0;

	for (i = 0; i < ARRAY_SIZE(default_keybindings); i++) {
		struct keybinding *keybinding = &default_keybindings[i];

		if (keybinding->request != request)
			continue;

		if (!string_format_from(buf, &pos, "%s%s", sep,
					get_key_name(keybinding->alias)))
			return "Too many keybindings!";
		sep = ", ";
	}

	return buf;
}

struct run_request {
	enum keymap keymap;
	int key;
	const char *argv[SIZEOF_ARG];
};

static struct run_request *run_request;
static size_t run_requests;

static enum request
add_run_request(enum keymap keymap, int key, int argc, const char **argv)
{
	struct run_request *req;

	if (argc >= ARRAY_SIZE(req->argv) - 1)
		return REQ_NONE;

	req = realloc(run_request, (run_requests + 1) * sizeof(*run_request));
	if (!req)
		return REQ_NONE;

	run_request = req;
	req = &run_request[run_requests];
	req->keymap = keymap;
	req->key = key;
	req->argv[0] = NULL;

	if (!format_argv(req->argv, argv, FORMAT_NONE))
		return REQ_NONE;

	return REQ_NONE + ++run_requests;
}

static struct run_request *
get_run_request(enum request request)
{
	if (request <= REQ_NONE)
		return NULL;
	return &run_request[request - REQ_NONE - 1];
}

static void
add_builtin_run_requests(void)
{
	const char *cherry_pick[] = { "git", "cherry-pick", "%(commit)", NULL };
	const char *gc[] = { "git", "gc", NULL };
	struct {
		enum keymap keymap;
		int key;
		int argc;
		const char **argv;
	} reqs[] = {
		{ KEYMAP_MAIN,	  'C', ARRAY_SIZE(cherry_pick) - 1, cherry_pick },
		{ KEYMAP_GENERIC, 'G', ARRAY_SIZE(gc) - 1, gc },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(reqs); i++) {
		enum request req;

		req = add_run_request(reqs[i].keymap, reqs[i].key, reqs[i].argc, reqs[i].argv);
		if (req != REQ_NONE)
			add_keybinding(reqs[i].keymap, req, reqs[i].key);
	}
}

/*
 * User config file handling.
 */

static struct int_map color_map[] = {
#define COLOR_MAP(name) { #name, STRING_SIZE(#name), COLOR_##name }
	COLOR_MAP(DEFAULT),
	COLOR_MAP(BLACK),
	COLOR_MAP(BLUE),
	COLOR_MAP(CYAN),
	COLOR_MAP(GREEN),
	COLOR_MAP(MAGENTA),
	COLOR_MAP(RED),
	COLOR_MAP(WHITE),
	COLOR_MAP(YELLOW),
};

#define set_color(color, name) \
	set_from_int_map(color_map, ARRAY_SIZE(color_map), color, name, strlen(name))

static struct int_map attr_map[] = {
#define ATTR_MAP(name) { #name, STRING_SIZE(#name), A_##name }
	ATTR_MAP(NORMAL),
	ATTR_MAP(BLINK),
	ATTR_MAP(BOLD),
	ATTR_MAP(DIM),
	ATTR_MAP(REVERSE),
	ATTR_MAP(STANDOUT),
	ATTR_MAP(UNDERLINE),
};

#define set_attribute(attr, name) \
	set_from_int_map(attr_map, ARRAY_SIZE(attr_map), attr, name, strlen(name))

static int   config_lineno;
static bool  config_errors;
static const char *config_msg;

/* Wants: object fgcolor bgcolor [attr] */
static int
option_color_command(int argc, const char *argv[])
{
	struct line_info *info;

	if (argc != 3 && argc != 4) {
		config_msg = "Wrong number of arguments given to color command";
		return ERR;
	}

	info = get_line_info(argv[0]);
	if (!info) {
		if (!string_enum_compare(argv[0], "main-delim", strlen("main-delim"))) {
			info = get_line_info("delimiter");

		} else if (!string_enum_compare(argv[0], "main-date", strlen("main-date"))) {
			info = get_line_info("date");

		} else if (!string_enum_compare(argv[0], "main-author", strlen("main-author"))) {
			info = get_line_info("author");

		} else {
			config_msg = "Unknown color name";
			return ERR;
		}
	}

	if (set_color(&info->fg, argv[1]) == ERR ||
	    set_color(&info->bg, argv[2]) == ERR) {
		config_msg = "Unknown color";
		return ERR;
	}

	if (argc == 4 && set_attribute(&info->attr, argv[3]) == ERR) {
		config_msg = "Unknown attribute";
		return ERR;
	}

	return OK;
}

static bool parse_bool(const char *s)
{
	return (!strcmp(s, "1") || !strcmp(s, "true") ||
		!strcmp(s, "yes")) ? TRUE : FALSE;
}

static int
parse_int(const char *s, int default_value, int min, int max)
{
	int value = atoi(s);

	return (value < min || value > max) ? default_value : value;
}

/* Wants: name = value */
static int
option_set_command(int argc, const char *argv[])
{
	if (argc != 3) {
		config_msg = "Wrong number of arguments given to set command";
		return ERR;
	}

	if (strcmp(argv[1], "=")) {
		config_msg = "No value assigned";
		return ERR;
	}

	if (!strcmp(argv[0], "show-author")) {
		opt_author = parse_bool(argv[2]);
		return OK;
	}

	if (!strcmp(argv[0], "show-date")) {
		opt_date = parse_bool(argv[2]);
		return OK;
	}

	if (!strcmp(argv[0], "show-rev-graph")) {
		opt_rev_graph = parse_bool(argv[2]);
		return OK;
	}

	if (!strcmp(argv[0], "show-refs")) {
		opt_show_refs = parse_bool(argv[2]);
		return OK;
	}

	if (!strcmp(argv[0], "show-line-numbers")) {
		opt_line_number = parse_bool(argv[2]);
		return OK;
	}

	if (!strcmp(argv[0], "line-graphics")) {
		opt_line_graphics = parse_bool(argv[2]);
		return OK;
	}

	if (!strcmp(argv[0], "line-number-interval")) {
		opt_num_interval = parse_int(argv[2], opt_num_interval, 1, 1024);
		return OK;
	}

	if (!strcmp(argv[0], "author-width")) {
		opt_author_cols = parse_int(argv[2], opt_author_cols, 0, 1024);
		return OK;
	}

	if (!strcmp(argv[0], "tab-size")) {
		opt_tab_size = parse_int(argv[2], opt_tab_size, 1, 1024);
		return OK;
	}

	if (!strcmp(argv[0], "commit-encoding")) {
		const char *arg = argv[2];
		int arglen = strlen(arg);

		switch (arg[0]) {
		case '"':
		case '\'':
			if (arglen == 1 || arg[arglen - 1] != arg[0]) {
				config_msg = "Unmatched quotation";
				return ERR;
			}
			arg += 1; arglen -= 2;
		default:
			string_ncopy(opt_encoding, arg, strlen(arg));
			return OK;
		}
	}

	config_msg = "Unknown variable name";
	return ERR;
}

/* Wants: mode request key */
static int
option_bind_command(int argc, const char *argv[])
{
	enum request request;
	int keymap;
	int key;

	if (argc < 3) {
		config_msg = "Wrong number of arguments given to bind command";
		return ERR;
	}

	if (set_keymap(&keymap, argv[0]) == ERR) {
		config_msg = "Unknown key map";
		return ERR;
	}

	key = get_key_value(argv[1]);
	if (key == ERR) {
		config_msg = "Unknown key";
		return ERR;
	}

	request = get_request(argv[2]);
	if (request == REQ_NONE) {
		struct {
			const char *name;
			enum request request;
		} obsolete[] = {
			{ "cherry-pick",	REQ_NONE },
			{ "screen-resize",	REQ_NONE },
			{ "tree-parent",	REQ_PARENT },
		};
		size_t namelen = strlen(argv[2]);
		int i;

		for (i = 0; i < ARRAY_SIZE(obsolete); i++) {
			if (namelen != strlen(obsolete[i].name) ||
			    string_enum_compare(obsolete[i].name, argv[2], namelen))
				continue;
			if (obsolete[i].request != REQ_NONE)
				add_keybinding(keymap, obsolete[i].request, key);
			config_msg = "Obsolete request name";
			return ERR;
		}
	}
	if (request == REQ_NONE && *argv[2]++ == '!')
		request = add_run_request(keymap, key, argc - 2, argv + 2);
	if (request == REQ_NONE) {
		config_msg = "Unknown request name";
		return ERR;
	}

	add_keybinding(keymap, request, key);

	return OK;
}

static int
set_option(const char *opt, char *value)
{
	const char *argv[SIZEOF_ARG];
	int argc = 0;

	if (!argv_from_string(argv, &argc, value)) {
		config_msg = "Too many option arguments";
		return ERR;
	}

	if (!strcmp(opt, "color"))
		return option_color_command(argc, argv);

	if (!strcmp(opt, "set"))
		return option_set_command(argc, argv);

	if (!strcmp(opt, "bind"))
		return option_bind_command(argc, argv);

	config_msg = "Unknown option command";
	return ERR;
}

static int
read_option(char *opt, size_t optlen, char *value, size_t valuelen)
{
	int status = OK;

	config_lineno++;
	config_msg = "Internal error";

	/* Check for comment markers, since read_properties() will
	 * only ensure opt and value are split at first " \t". */
	optlen = strcspn(opt, "#");
	if (optlen == 0)
		return OK;

	if (opt[optlen] != 0) {
		config_msg = "No option value";
		status = ERR;

	}  else {
		/* Look for comment endings in the value. */
		size_t len = strcspn(value, "#");

		if (len < valuelen) {
			valuelen = len;
			value[valuelen] = 0;
		}

		status = set_option(opt, value);
	}

	if (status == ERR) {
		warn("Error on line %d, near '%.*s': %s",
		     config_lineno, (int) optlen, opt, config_msg);
		config_errors = TRUE;
	}

	/* Always keep going if errors are encountered. */
	return OK;
}

static void
load_option_file(const char *path)
{
	struct io io = {};

	/* It's ok that the file doesn't exist. */
	if (!io_open(&io, path))
		return;

	config_lineno = 0;
	config_errors = FALSE;

	if (io_load(&io, " \t", read_option) == ERR ||
	    config_errors == TRUE)
		warn("Errors while loading %s.", path);
}

static int
load_options(void)
{
	const char *home = getenv("HOME");
	const char *tigrc_user = getenv("TIGRC_USER");
	const char *tigrc_system = getenv("TIGRC_SYSTEM");
	char buf[SIZEOF_STR];

	add_builtin_run_requests();

	if (!tigrc_system) {
		if (!string_format(buf, "%s/tigrc", SYSCONFDIR))
			return ERR;
		tigrc_system = buf;
	}
	load_option_file(tigrc_system);

	if (!tigrc_user) {
		if (!home || !string_format(buf, "%s/.tigrc", home))
			return ERR;
		tigrc_user = buf;
	}
	load_option_file(tigrc_user);

	return OK;
}


/*
 * The viewer
 */

struct view;
struct view_ops;

/* The display array of active views and the index of the current view. */
static struct view *display[2];
static unsigned int current_view;

#define foreach_displayed_view(view, i) \
	for (i = 0; i < ARRAY_SIZE(display) && (view = display[i]); i++)

#define displayed_views()	(display[1] != NULL ? 2 : 1)

/* Current head and commit ID */
static char ref_blob[SIZEOF_REF]	= "";
static char ref_commit[SIZEOF_REF]	= "HEAD";
static char ref_head[SIZEOF_REF]	= "HEAD";

struct view {
	const char *name;	/* View name */
	const char *cmd_env;	/* Command line set via environment */
	const char *id;		/* Points to either of ref_{head,commit,blob} */

	struct view_ops *ops;	/* View operations */

	enum keymap keymap;	/* What keymap does this view have */
	bool git_dir;		/* Whether the view requires a git directory. */

	char ref[SIZEOF_REF];	/* Hovered commit reference */
	char vid[SIZEOF_REF];	/* View ID. Set to id member when updating. */

	int height, width;	/* The width and height of the main window */
	WINDOW *win;		/* The main window */
	WINDOW *title;		/* The title window living below the main window */

	/* Navigation */
	unsigned long offset;	/* Offset of the window top */
	unsigned long yoffset;	/* Offset from the window side. */
	unsigned long lineno;	/* Current line number */
	unsigned long p_offset;	/* Previous offset of the window top */
	unsigned long p_yoffset;/* Previous offset from the window side */
	unsigned long p_lineno;	/* Previous current line number */
	bool p_restore;		/* Should the previous position be restored. */

	/* Searching */
	char grep[SIZEOF_STR];	/* Search string */
	regex_t *regex;		/* Pre-compiled regex */

	/* If non-NULL, points to the view that opened this view. If this view
	 * is closed tig will switch back to the parent view. */
	struct view *parent;

	/* Buffering */
	size_t lines;		/* Total number of lines */
	struct line *line;	/* Line index */
	size_t line_alloc;	/* Total number of allocated lines */
	unsigned int digits;	/* Number of digits in the lines member. */

	/* Drawing */
	struct line *curline;	/* Line currently being drawn. */
	enum line_type curtype;	/* Attribute currently used for drawing. */
	unsigned long col;	/* Column when drawing. */
	bool has_scrolled;	/* View was scrolled. */
	bool can_hscroll;	/* View can be scrolled horizontally. */

	/* Loading */
	struct io io;
	struct io *pipe;
	time_t start_time;
	time_t update_secs;
};

struct view_ops {
	/* What type of content being displayed. Used in the title bar. */
	const char *type;
	/* Default command arguments. */
	const char **argv;
	/* Open and reads in all view content. */
	bool (*open)(struct view *view);
	/* Read one line; updates view->line. */
	bool (*read)(struct view *view, char *data);
	/* Draw one line; @lineno must be < view->height. */
	bool (*draw)(struct view *view, struct line *line, unsigned int lineno);
	/* Depending on view handle a special requests. */
	enum request (*request)(struct view *view, enum request request, struct line *line);
	/* Search for regex in a line. */
	bool (*grep)(struct view *view, struct line *line);
	/* Select line */
	void (*select)(struct view *view, struct line *line);
};

static struct view_ops blame_ops;
static struct view_ops blob_ops;
static struct view_ops diff_ops;
static struct view_ops help_ops;
static struct view_ops log_ops;
static struct view_ops main_ops;
static struct view_ops pager_ops;
static struct view_ops stage_ops;
static struct view_ops status_ops;
static struct view_ops tree_ops;

#define VIEW_STR(name, env, ref, ops, map, git) \
	{ name, #env, ref, ops, map, git }

#define VIEW_(id, name, ops, git, ref) \
	VIEW_STR(name, TIG_##id##_CMD, ref, ops, KEYMAP_##id, git)


static struct view views[] = {
	VIEW_(MAIN,   "main",   &main_ops,   TRUE,  ref_head),
	VIEW_(DIFF,   "diff",   &diff_ops,   TRUE,  ref_commit),
	VIEW_(LOG,    "log",    &log_ops,    TRUE,  ref_head),
	VIEW_(TREE,   "tree",   &tree_ops,   TRUE,  ref_commit),
	VIEW_(BLOB,   "blob",   &blob_ops,   TRUE,  ref_blob),
	VIEW_(BLAME,  "blame",  &blame_ops,  TRUE,  ref_commit),
	VIEW_(HELP,   "help",   &help_ops,   FALSE, ""),
	VIEW_(PAGER,  "pager",  &pager_ops,  FALSE, "stdin"),
	VIEW_(STATUS, "status", &status_ops, TRUE,  ""),
	VIEW_(STAGE,  "stage",	&stage_ops,  TRUE,  ""),
};

#define VIEW(req) 	(&views[(req) - REQ_OFFSET - 1])
#define VIEW_REQ(view)	((view) - views + REQ_OFFSET + 1)

#define foreach_view(view, i) \
	for (i = 0; i < ARRAY_SIZE(views) && (view = &views[i]); i++)

#define view_is_displayed(view) \
	(view == display[0] || view == display[1])


enum line_graphic {
	LINE_GRAPHIC_VLINE
};

static int line_graphics[] = {
	/* LINE_GRAPHIC_VLINE: */ '|'
};

static inline void
set_view_attr(struct view *view, enum line_type type)
{
	if (!view->curline->selected && view->curtype != type) {
		wattrset(view->win, get_line_attr(type));
		wchgat(view->win, -1, 0, type, NULL);
		view->curtype = type;
	}
}

static int
draw_chars(struct view *view, enum line_type type, const char *string,
	   int max_len, bool use_tilde)
{
	int len = 0;
	int col = 0;
	int trimmed = FALSE;
	size_t skip = view->yoffset > view->col ? view->yoffset - view->col : 0;

	if (max_len <= 0)
		return 0;

	if (opt_utf8) {
		len = utf8_length(&string, skip, &col, max_len, &trimmed, use_tilde);
	} else {
		col = len = strlen(string);
		if (len > max_len) {
			if (use_tilde) {
				max_len -= 1;
			}
			col = len = max_len;
			trimmed = TRUE;
		}
	}

	set_view_attr(view, type);
	if (len > 0)
		waddnstr(view->win, string, len);
	if (trimmed && use_tilde) {
		set_view_attr(view, LINE_DELIMITER);
		waddch(view->win, '~');
		col++;
	}

	if (view->col + col >= view->width + view->yoffset)
		view->can_hscroll = TRUE;

	return col;
}

static int
draw_space(struct view *view, enum line_type type, int max, int spaces)
{
	static char space[] = "                    ";
	int col = 0;

	spaces = MIN(max, spaces);

	while (spaces > 0) {
		int len = MIN(spaces, sizeof(space) - 1);

		col += draw_chars(view, type, space, spaces, FALSE);
		spaces -= len;
	}

	return col;
}

static bool
draw_lineno(struct view *view, unsigned int lineno)
{
	size_t skip = view->yoffset > view->col ? view->yoffset - view->col : 0;
	char number[10];
	int digits3 = view->digits < 3 ? 3 : view->digits;
	int max_number = MIN(digits3, STRING_SIZE(number));
	int max = view->width - view->col;
	int col;

	if (max < max_number)
		max_number = max;

	lineno += view->offset + 1;
	if (lineno == 1 || (lineno % opt_num_interval) == 0) {
		static char fmt[] = "%1ld";

		if (view->digits <= 9)
			fmt[1] = '0' + digits3;

		if (!string_format(number, fmt, lineno))
			number[0] = 0;
		col = draw_chars(view, LINE_LINE_NUMBER, number, max_number, TRUE);
	} else {
		col = draw_space(view, LINE_LINE_NUMBER, max_number, max_number);
	}

	if (col < max && skip <= col) {
		set_view_attr(view, LINE_DEFAULT);
		waddch(view->win, line_graphics[LINE_GRAPHIC_VLINE]);
	}
	col++;

	view->col += col;
	if (col < max && skip <= col)
		col = draw_space(view, LINE_DEFAULT, max - col, 1);
	view->col++;

	return view->width + view->yoffset <= view->col;
}

static bool
draw_text(struct view *view, enum line_type type, const char *string, bool trim)
{
	view->col += draw_chars(view, type, string, view->width + view->yoffset - view->col, trim);
	return view->width - view->col <= 0;
}

static bool
draw_graphic(struct view *view, enum line_type type, chtype graphic[], size_t size)
{
	size_t skip = view->yoffset > view->col ? view->yoffset - view->col : 0;
	int max = view->width - view->col;
	int i;

	if (max < size)
		size = max;

	set_view_attr(view, type);
	/* Using waddch() instead of waddnstr() ensures that
	 * they'll be rendered correctly for the cursor line. */
	for (i = skip; i < size; i++)
		waddch(view->win, graphic[i]);

	view->col += size;
	if (size < max && skip <= size)
		waddch(view->win, ' ');
	view->col++;

	return view->width - view->col <= 0;
}

static bool
draw_field(struct view *view, enum line_type type, const char *text, int len, bool trim)
{
	int max = MIN(view->width - view->col, len);
	int col;

	if (text)
		col = draw_chars(view, type, text, max - 1, trim);
	else
		col = draw_space(view, type, max - 1, max - 1);

	view->col += col;
	view->col += draw_space(view, LINE_DEFAULT, max - col, max - col);
	return view->width + view->yoffset <= view->col;
}

static bool
draw_date(struct view *view, struct tm *time)
{
	char buf[DATE_COLS];
	char *date;
	int timelen = 0;

	if (time)
		timelen = strftime(buf, sizeof(buf), DATE_FORMAT, time);
	date = timelen ? buf : NULL;

	return draw_field(view, LINE_DATE, date, DATE_COLS, FALSE);
}

static bool
draw_author(struct view *view, const char *author)
{
	bool trim = opt_author_cols == 0 || opt_author_cols > 5 || !author;

	if (!trim) {
		static char initials[10];
		size_t pos;

#define is_initial_sep(c) (isspace(c) || ispunct(c) || (c) == '@')

		memset(initials, 0, sizeof(initials));
		for (pos = 0; *author && pos < opt_author_cols - 1; author++, pos++) {
			while (is_initial_sep(*author))
				author++;
			strncpy(&initials[pos], author, sizeof(initials) - 1 - pos);
			while (*author && !is_initial_sep(author[1]))
				author++;
		}

		author = initials;
	}

	return draw_field(view, LINE_AUTHOR, author, opt_author_cols, trim);
}

static bool
draw_mode(struct view *view, mode_t mode)
{
	static const char dir_mode[]	= "drwxr-xr-x";
	static const char link_mode[]	= "lrwxrwxrwx";
	static const char exe_mode[]	= "-rwxr-xr-x";
	static const char file_mode[]	= "-rw-r--r--";
	const char *str;

	if (S_ISDIR(mode))
		str = dir_mode;
	else if (S_ISLNK(mode))
		str = link_mode;
	else if (mode & S_IXUSR)
		str = exe_mode;
	else
		str = file_mode;

	return draw_field(view, LINE_MODE, str, sizeof(file_mode), FALSE);
}

static bool
draw_view_line(struct view *view, unsigned int lineno)
{
	struct line *line;
	bool selected = (view->offset + lineno == view->lineno);

	assert(view_is_displayed(view));

	if (view->offset + lineno >= view->lines)
		return FALSE;

	line = &view->line[view->offset + lineno];

	wmove(view->win, lineno, 0);
	if (line->cleareol)
		wclrtoeol(view->win);
	view->col = 0;
	view->curline = line;
	view->curtype = LINE_NONE;
	line->selected = FALSE;
	line->dirty = line->cleareol = 0;

	if (selected) {
		set_view_attr(view, LINE_CURSOR);
		line->selected = TRUE;
		view->ops->select(view, line);
	}

	return view->ops->draw(view, line, lineno);
}

static void
redraw_view_dirty(struct view *view)
{
	bool dirty = FALSE;
	int lineno;

	for (lineno = 0; lineno < view->height; lineno++) {
		if (view->offset + lineno >= view->lines)
			break;
		if (!view->line[view->offset + lineno].dirty)
			continue;
		dirty = TRUE;
		if (!draw_view_line(view, lineno))
			break;
	}

	if (!dirty)
		return;
	wnoutrefresh(view->win);
}

static void
redraw_view_from(struct view *view, int lineno)
{
	assert(0 <= lineno && lineno < view->height);

	if (lineno == 0)
		view->can_hscroll = FALSE;

	for (; lineno < view->height; lineno++) {
		if (!draw_view_line(view, lineno))
			break;
	}

	wnoutrefresh(view->win);
}

static void
redraw_view(struct view *view)
{
	werase(view->win);
	redraw_view_from(view, 0);
}


static void
update_view_title(struct view *view)
{
	char buf[SIZEOF_STR];
	char state[SIZEOF_STR];
	size_t bufpos = 0, statelen = 0;

	assert(view_is_displayed(view));

	if (view != VIEW(REQ_VIEW_STATUS) && view->lines) {
		unsigned int view_lines = view->offset + view->height;
		unsigned int lines = view->lines
				   ? MIN(view_lines, view->lines) * 100 / view->lines
				   : 0;

		string_format_from(state, &statelen, " - %s %d of %d (%d%%)",
				   view->ops->type,
				   view->lineno + 1,
				   view->lines,
				   lines);

	}

	if (view->pipe) {
		time_t secs = time(NULL) - view->start_time;

		/* Three git seconds are a long time ... */
		if (secs > 2)
			string_format_from(state, &statelen, " loading %lds", secs);
	}

	string_format_from(buf, &bufpos, "[%s]", view->name);
	if (*view->ref && bufpos < view->width) {
		size_t refsize = strlen(view->ref);
		size_t minsize = bufpos + 1 + /* abbrev= */ 7 + 1 + statelen;

		if (minsize < view->width)
			refsize = view->width - minsize + 7;
		string_format_from(buf, &bufpos, " %.*s", (int) refsize, view->ref);
	}

	if (statelen && bufpos < view->width) {
		string_format_from(buf, &bufpos, "%s", state);
	}

	if (view == display[current_view])
		wbkgdset(view->title, get_line_attr(LINE_TITLE_FOCUS));
	else
		wbkgdset(view->title, get_line_attr(LINE_TITLE_BLUR));

	mvwaddnstr(view->title, 0, 0, buf, bufpos);
	wclrtoeol(view->title);
	wnoutrefresh(view->title);
}

static void
resize_display(void)
{
	int offset, i;
	struct view *base = display[0];
	struct view *view = display[1] ? display[1] : display[0];

	/* Setup window dimensions */

	getmaxyx(stdscr, base->height, base->width);

	/* Make room for the status window. */
	base->height -= 1;

	if (view != base) {
		/* Horizontal split. */
		view->width   = base->width;
		view->height  = SCALE_SPLIT_VIEW(base->height);
		base->height -= view->height;

		/* Make room for the title bar. */
		view->height -= 1;
	}

	/* Make room for the title bar. */
	base->height -= 1;

	offset = 0;

	foreach_displayed_view (view, i) {
		if (!view->win) {
			view->win = newwin(view->height, 0, offset, 0);
			if (!view->win)
				die("Failed to create %s view", view->name);

			scrollok(view->win, FALSE);

			view->title = newwin(1, 0, offset + view->height, 0);
			if (!view->title)
				die("Failed to create title window");

		} else {
			wresize(view->win, view->height, view->width);
			mvwin(view->win,   offset, 0);
			mvwin(view->title, offset + view->height, 0);
		}

		offset += view->height + 1;
	}
}

static void
redraw_display(bool clear)
{
	struct view *view;
	int i;

	foreach_displayed_view (view, i) {
		if (clear)
			wclear(view->win);
		redraw_view(view);
		update_view_title(view);
	}
}

static void
toggle_view_option(bool *option, const char *help)
{
	*option = !*option;
	redraw_display(FALSE);
	report("%sabling %s", *option ? "En" : "Dis", help);
}

/*
 * Navigation
 */

/* Scrolling backend */
static void
do_scroll_view(struct view *view, int lines)
{
	bool redraw_current_line = FALSE;

	/* The rendering expects the new offset. */
	view->offset += lines;

	assert(0 <= view->offset && view->offset < view->lines);
	assert(lines);

	/* Move current line into the view. */
	if (view->lineno < view->offset) {
		view->lineno = view->offset;
		redraw_current_line = TRUE;
	} else if (view->lineno >= view->offset + view->height) {
		view->lineno = view->offset + view->height - 1;
		redraw_current_line = TRUE;
	}

	assert(view->offset <= view->lineno && view->lineno < view->lines);

	/* Redraw the whole screen if scrolling is pointless. */
	if (view->height < ABS(lines)) {
		redraw_view(view);

	} else {
		int line = lines > 0 ? view->height - lines : 0;
		int end = line + ABS(lines);

		scrollok(view->win, TRUE);
		wscrl(view->win, lines);
		scrollok(view->win, FALSE);

		while (line < end && draw_view_line(view, line))
			line++;

		if (redraw_current_line)
			draw_view_line(view, view->lineno - view->offset);
		wnoutrefresh(view->win);
	}

	view->has_scrolled = TRUE;
	report("");
}

/* Scroll frontend */
static void
scroll_view(struct view *view, enum request request)
{
	int lines = 1;

	assert(view_is_displayed(view));

	switch (request) {
	case REQ_SCROLL_LEFT:
		if (view->yoffset == 0) {
			report("Cannot scroll beyond the first column");
			return;
		}
		if (view->yoffset <= SCROLL_INTERVAL)
			view->yoffset = 0;
		else
			view->yoffset -= SCROLL_INTERVAL;
		redraw_view_from(view, 0);
		report("");
		return;
	case REQ_SCROLL_RIGHT:
		if (!view->can_hscroll) {
			report("Cannot scroll beyond the last column");
			return;
		}
		view->yoffset += SCROLL_INTERVAL;
		redraw_view(view);
		report("");
		return;
	case REQ_SCROLL_PAGE_DOWN:
		lines = view->height;
	case REQ_SCROLL_LINE_DOWN:
		if (view->offset + lines > view->lines)
			lines = view->lines - view->offset;

		if (lines == 0 || view->offset + view->height >= view->lines) {
			report("Cannot scroll beyond the last line");
			return;
		}
		break;

	case REQ_SCROLL_PAGE_UP:
		lines = view->height;
	case REQ_SCROLL_LINE_UP:
		if (lines > view->offset)
			lines = view->offset;

		if (lines == 0) {
			report("Cannot scroll beyond the first line");
			return;
		}

		lines = -lines;
		break;

	default:
		die("request %d not handled in switch", request);
	}

	do_scroll_view(view, lines);
}

/* Cursor moving */
static void
move_view(struct view *view, enum request request)
{
	int scroll_steps = 0;
	int steps;

	switch (request) {
	case REQ_MOVE_FIRST_LINE:
		steps = -view->lineno;
		break;

	case REQ_MOVE_LAST_LINE:
		steps = view->lines - view->lineno - 1;
		break;

	case REQ_MOVE_PAGE_UP:
		steps = view->height > view->lineno
		      ? -view->lineno : -view->height;
		break;

	case REQ_MOVE_PAGE_DOWN:
		steps = view->lineno + view->height >= view->lines
		      ? view->lines - view->lineno - 1 : view->height;
		break;

	case REQ_MOVE_UP:
		steps = -1;
		break;

	case REQ_MOVE_DOWN:
		steps = 1;
		break;

	default:
		die("request %d not handled in switch", request);
	}

	if (steps <= 0 && view->lineno == 0) {
		report("Cannot move beyond the first line");
		return;

	} else if (steps >= 0 && view->lineno + 1 >= view->lines) {
		report("Cannot move beyond the last line");
		return;
	}

	/* Move the current line */
	view->lineno += steps;
	assert(0 <= view->lineno && view->lineno < view->lines);

	/* Check whether the view needs to be scrolled */
	if (view->lineno < view->offset ||
	    view->lineno >= view->offset + view->height) {
		scroll_steps = steps;
		if (steps < 0 && -steps > view->offset) {
			scroll_steps = -view->offset;

		} else if (steps > 0) {
			if (view->lineno == view->lines - 1 &&
			    view->lines > view->height) {
				scroll_steps = view->lines - view->offset - 1;
				if (scroll_steps >= view->height)
					scroll_steps -= view->height - 1;
			}
		}
	}

	if (!view_is_displayed(view)) {
		view->offset += scroll_steps;
		assert(0 <= view->offset && view->offset < view->lines);
		view->ops->select(view, &view->line[view->lineno]);
		return;
	}

	/* Repaint the old "current" line if we be scrolling */
	if (ABS(steps) < view->height)
		draw_view_line(view, view->lineno - steps - view->offset);

	if (scroll_steps) {
		do_scroll_view(view, scroll_steps);
		return;
	}

	/* Draw the current line */
	draw_view_line(view, view->lineno - view->offset);

	wnoutrefresh(view->win);
	report("");
}


/*
 * Searching
 */

static void search_view(struct view *view, enum request request);

static void
select_view_line(struct view *view, unsigned long lineno)
{
	if (lineno - view->offset >= view->height) {
		view->offset = lineno;
		view->lineno = lineno;
		if (view_is_displayed(view))
			redraw_view(view);

	} else {
		unsigned long old_lineno = view->lineno - view->offset;

		view->lineno = lineno;
		if (view_is_displayed(view)) {
			draw_view_line(view, old_lineno);
			draw_view_line(view, view->lineno - view->offset);
			wnoutrefresh(view->win);
		} else {
			view->ops->select(view, &view->line[view->lineno]);
		}
	}
}

static void
find_next(struct view *view, enum request request)
{
	unsigned long lineno = view->lineno;
	int direction;

	if (!*view->grep) {
		if (!*opt_search)
			report("No previous search");
		else
			search_view(view, request);
		return;
	}

	switch (request) {
	case REQ_SEARCH:
	case REQ_FIND_NEXT:
		direction = 1;
		break;

	case REQ_SEARCH_BACK:
	case REQ_FIND_PREV:
		direction = -1;
		break;

	default:
		return;
	}

	if (request == REQ_FIND_NEXT || request == REQ_FIND_PREV)
		lineno += direction;

	/* Note, lineno is unsigned long so will wrap around in which case it
	 * will become bigger than view->lines. */
	for (; lineno < view->lines; lineno += direction) {
		if (view->ops->grep(view, &view->line[lineno])) {
			select_view_line(view, lineno);
			report("Line %ld matches '%s'", lineno + 1, view->grep);
			return;
		}
	}

	report("No match found for '%s'", view->grep);
}

static void
search_view(struct view *view, enum request request)
{
	int regex_err;

	if (view->regex) {
		regfree(view->regex);
		*view->grep = 0;
	} else {
		view->regex = calloc(1, sizeof(*view->regex));
		if (!view->regex)
			return;
	}

	regex_err = regcomp(view->regex, opt_search, REG_EXTENDED);
	if (regex_err != 0) {
		char buf[SIZEOF_STR] = "unknown error";

		regerror(regex_err, view->regex, buf, sizeof(buf));
		report("Search failed: %s", buf);
		return;
	}

	string_copy(view->grep, opt_search);

	find_next(view, request);
}

/*
 * Incremental updating
 */

static void
reset_view(struct view *view)
{
	int i;

	for (i = 0; i < view->lines; i++)
		free(view->line[i].data);
	free(view->line);

	view->p_offset = view->offset;
	view->p_yoffset = view->yoffset;
	view->p_lineno = view->lineno;

	view->line = NULL;
	view->offset = 0;
	view->yoffset = 0;
	view->lines  = 0;
	view->lineno = 0;
	view->line_alloc = 0;
	view->vid[0] = 0;
	view->update_secs = 0;
}

static void
free_argv(const char *argv[])
{
	int argc;

	for (argc = 0; argv[argc]; argc++)
		free((void *) argv[argc]);
}

static bool
format_argv(const char *dst_argv[], const char *src_argv[], enum format_flags flags)
{
	char buf[SIZEOF_STR];
	int argc;
	bool noreplace = flags == FORMAT_NONE;

	free_argv(dst_argv);

	for (argc = 0; src_argv[argc]; argc++) {
		const char *arg = src_argv[argc];
		size_t bufpos = 0;

		while (arg) {
			char *next = strstr(arg, "%(");
			int len = next - arg;
			const char *value;

			if (!next || noreplace) {
				if (flags == FORMAT_DASH && !strcmp(arg, "--"))
					noreplace = TRUE;
				len = strlen(arg);
				value = "";

			} else if (!prefixcmp(next, "%(directory)")) {
				value = opt_path;

			} else if (!prefixcmp(next, "%(file)")) {
				value = opt_file;

			} else if (!prefixcmp(next, "%(ref)")) {
				value = *opt_ref ? opt_ref : "HEAD";

			} else if (!prefixcmp(next, "%(head)")) {
				value = ref_head;

			} else if (!prefixcmp(next, "%(commit)")) {
				value = ref_commit;

			} else if (!prefixcmp(next, "%(blob)")) {
				value = ref_blob;

			} else {
				report("Unknown replacement: `%s`", next);
				return FALSE;
			}

			if (!string_format_from(buf, &bufpos, "%.*s%s", len, arg, value))
				return FALSE;

			arg = next && !noreplace ? strchr(next, ')') + 1 : NULL;
		}

		dst_argv[argc] = strdup(buf);
		if (!dst_argv[argc])
			break;
	}

	dst_argv[argc] = NULL;

	return src_argv[argc] == NULL;
}

static bool
restore_view_position(struct view *view)
{
	if (!view->p_restore || (view->pipe && view->lines <= view->p_lineno))
		return FALSE;

	/* Changing the view position cancels the restoring. */
	/* FIXME: Changing back to the first line is not detected. */
	if (view->offset != 0 || view->lineno != 0) {
		view->p_restore = FALSE;
		return FALSE;
	}

	if (view->p_lineno >= view->lines) {
		view->p_lineno = view->lines > 0 ? view->lines - 1 : 0;
		if (view->p_offset >= view->p_lineno) {
			unsigned long half = view->height / 2;

			if (view->p_lineno > half)
				view->p_offset = view->p_lineno - half;
			else
				view->p_offset = 0;
		}
	}

	if (view_is_displayed(view) &&
	    view->offset != view->p_offset &&
	    view->lineno != view->p_lineno)
		werase(view->win);

	view->offset = view->p_offset;
	view->yoffset = view->p_yoffset;
	view->lineno = view->p_lineno;
	view->p_restore = FALSE;

	return TRUE;
}

static void
end_update(struct view *view, bool force)
{
	if (!view->pipe)
		return;
	while (!view->ops->read(view, NULL))
		if (!force)
			return;
	set_nonblocking_input(FALSE);
	if (force)
		kill_io(view->pipe);
	done_io(view->pipe);
	view->pipe = NULL;
}

static void
setup_update(struct view *view, const char *vid)
{
	set_nonblocking_input(TRUE);
	reset_view(view);
	string_copy_rev(view->vid, vid);
	view->pipe = &view->io;
	view->start_time = time(NULL);
}

static bool
prepare_update(struct view *view, const char *argv[], const char *dir,
	       enum format_flags flags)
{
	if (view->pipe)
		end_update(view, TRUE);
	return init_io_rd(&view->io, argv, dir, flags);
}

static bool
prepare_update_file(struct view *view, const char *name)
{
	if (view->pipe)
		end_update(view, TRUE);
	return io_open(&view->io, name);
}

static bool
begin_update(struct view *view, bool refresh)
{
	if (view->pipe)
		end_update(view, TRUE);

	if (refresh) {
		if (!start_io(&view->io))
			return FALSE;

	} else {
		if (view == VIEW(REQ_VIEW_TREE) && strcmp(view->vid, view->id))
			opt_path[0] = 0;

		if (!run_io_rd(&view->io, view->ops->argv, FORMAT_ALL))
			return FALSE;

		/* Put the current ref_* value to the view title ref
		 * member. This is needed by the blob view. Most other
		 * views sets it automatically after loading because the
		 * first line is a commit line. */
		string_copy_rev(view->ref, view->id);
	}

	setup_update(view, view->id);

	return TRUE;
}

#define ITEM_CHUNK_SIZE 256
static void *
realloc_items(void *mem, size_t *size, size_t new_size, size_t item_size)
{
	size_t num_chunks = *size / ITEM_CHUNK_SIZE;
	size_t num_chunks_new = (new_size + ITEM_CHUNK_SIZE - 1) / ITEM_CHUNK_SIZE;

	if (mem == NULL || num_chunks != num_chunks_new) {
		*size = num_chunks_new * ITEM_CHUNK_SIZE;
		mem = realloc(mem, *size * item_size);
	}

	return mem;
}

static struct line *
realloc_lines(struct view *view, size_t line_size)
{
	size_t alloc = view->line_alloc;
	struct line *tmp = realloc_items(view->line, &alloc, line_size,
					 sizeof(*view->line));

	if (!tmp)
		return NULL;

	view->line = tmp;
	view->line_alloc = alloc;
	return view->line;
}

static bool
update_view(struct view *view)
{
	char out_buffer[BUFSIZ * 2];
	char *line;
	/* Clear the view and redraw everything since the tree sorting
	 * might have rearranged things. */
	bool redraw = view->lines == 0;
	bool can_read = TRUE;

	if (!view->pipe)
		return TRUE;

	if (!io_can_read(view->pipe)) {
		if (view->lines == 0) {
			time_t secs = time(NULL) - view->start_time;

			if (secs > 1 && secs > view->update_secs) {
				if (view->update_secs == 0)
					redraw_view(view);
				update_view_title(view);
				view->update_secs = secs;
			}
		}
		return TRUE;
	}

	for (; (line = io_get(view->pipe, '\n', can_read)); can_read = FALSE) {
		if (opt_iconv != ICONV_NONE) {
			ICONV_CONST char *inbuf = line;
			size_t inlen = strlen(line) + 1;

			char *outbuf = out_buffer;
			size_t outlen = sizeof(out_buffer);

			size_t ret;

			ret = iconv(opt_iconv, &inbuf, &inlen, &outbuf, &outlen);
			if (ret != (size_t) -1)
				line = out_buffer;
		}

		if (!view->ops->read(view, line)) {
			report("Allocation failure");
			end_update(view, TRUE);
			return FALSE;
		}
	}

	{
		unsigned long lines = view->lines;
		int digits;

		for (digits = 0; lines; digits++)
			lines /= 10;

		/* Keep the displayed view in sync with line number scaling. */
		if (digits != view->digits) {
			view->digits = digits;
			if (opt_line_number || view == VIEW(REQ_VIEW_BLAME))
				redraw = TRUE;
		}
	}

	if (io_error(view->pipe)) {
		report("Failed to read: %s", io_strerror(view->pipe));
		end_update(view, TRUE);

	} else if (io_eof(view->pipe)) {
		report("");
		end_update(view, FALSE);
	}

	if (restore_view_position(view))
		redraw = TRUE;

	if (!view_is_displayed(view))
		return TRUE;

	if (redraw)
		redraw_view_from(view, 0);
	else
		redraw_view_dirty(view);

	/* Update the title _after_ the redraw so that if the redraw picks up a
	 * commit reference in view->ref it'll be available here. */
	update_view_title(view);
	return TRUE;
}

static struct line *
add_line_data(struct view *view, void *data, enum line_type type)
{
	struct line *line;

	if (!realloc_lines(view, view->lines + 1))
		return NULL;

	line = &view->line[view->lines++];
	memset(line, 0, sizeof(*line));
	line->type = type;
	line->data = data;
	line->dirty = 1;

	return line;
}

static struct line *
add_line_text(struct view *view, const char *text, enum line_type type)
{
	char *data = text ? strdup(text) : NULL;

	return data ? add_line_data(view, data, type) : NULL;
}

static struct line *
add_line_format(struct view *view, enum line_type type, const char *fmt, ...)
{
	char buf[SIZEOF_STR];
	va_list args;

	va_start(args, fmt);
	if (vsnprintf(buf, sizeof(buf), fmt, args) >= sizeof(buf))
		buf[0] = 0;
	va_end(args);

	return buf[0] ? add_line_text(view, buf, type) : NULL;
}

/*
 * View opening
 */

enum open_flags {
	OPEN_DEFAULT = 0,	/* Use default view switching. */
	OPEN_SPLIT = 1,		/* Split current view. */
	OPEN_BACKGROUNDED = 2,	/* Backgrounded. */
	OPEN_RELOAD = 4,	/* Reload view even if it is the current. */
	OPEN_NOMAXIMIZE = 8,	/* Do not maximize the current view. */
	OPEN_REFRESH = 16,	/* Refresh view using previous command. */
	OPEN_PREPARED = 32,	/* Open already prepared command. */
};

static void
open_view(struct view *prev, enum request request, enum open_flags flags)
{
	bool backgrounded = !!(flags & OPEN_BACKGROUNDED);
	bool split = !!(flags & OPEN_SPLIT);
	bool reload = !!(flags & (OPEN_RELOAD | OPEN_REFRESH | OPEN_PREPARED));
	bool nomaximize = !!(flags & (OPEN_NOMAXIMIZE | OPEN_REFRESH));
	struct view *view = VIEW(request);
	int nviews = displayed_views();
	struct view *base_view = display[0];

	if (view == prev && nviews == 1 && !reload) {
		report("Already in %s view", view->name);
		return;
	}

	if (view->git_dir && !opt_git_dir[0]) {
		report("The %s view is disabled in pager view", view->name);
		return;
	}

	if (split) {
		display[1] = view;
		if (!backgrounded)
			current_view = 1;
	} else if (!nomaximize) {
		/* Maximize the current view. */
		memset(display, 0, sizeof(display));
		current_view = 0;
		display[current_view] = view;
	}

	/* Resize the view when switching between split- and full-screen,
	 * or when switching between two different full-screen views. */
	if (nviews != displayed_views() ||
	    (nviews == 1 && base_view != display[0]))
		resize_display();

	if (view->ops->open) {
		if (view->pipe)
			end_update(view, TRUE);
		if (!view->ops->open(view)) {
			report("Failed to load %s view", view->name);
			return;
		}
		restore_view_position(view);

	} else if ((reload || strcmp(view->vid, view->id)) &&
		   !begin_update(view, flags & (OPEN_REFRESH | OPEN_PREPARED))) {
		report("Failed to load %s view", view->name);
		return;
	}

	if (split && prev->lineno - prev->offset >= prev->height) {
		/* Take the title line into account. */
		int lines = prev->lineno - prev->offset - prev->height + 1;

		/* Scroll the view that was split if the current line is
		 * outside the new limited view. */
		do_scroll_view(prev, lines);
	}

	if (prev && view != prev) {
		if (split && !backgrounded) {
			/* "Blur" the previous view. */
			update_view_title(prev);
		}

		view->parent = prev;
	}

	if (view->pipe && view->lines == 0) {
		/* Clear the old view and let the incremental updating refill
		 * the screen. */
		werase(view->win);
		view->p_restore = flags & (OPEN_RELOAD | OPEN_REFRESH);
		report("");
	} else if (view_is_displayed(view)) {
		redraw_view(view);
		report("");
	}

	/* If the view is backgrounded the above calls to report()
	 * won't redraw the view title. */
	if (backgrounded)
		update_view_title(view);
}

static void
open_external_viewer(const char *argv[], const char *dir)
{
	def_prog_mode();           /* save current tty modes */
	endwin();                  /* restore original tty modes */
	run_io_fg(argv, dir);
	fprintf(stderr, "Press Enter to continue");
	getc(opt_tty);
	reset_prog_mode();
	redraw_display(TRUE);
}

static void
open_mergetool(const char *file)
{
	const char *mergetool_argv[] = { "git", "mergetool", file, NULL };

	open_external_viewer(mergetool_argv, opt_cdup);
}

static void
open_editor(bool from_root, const char *file)
{
	const char *editor_argv[] = { "vi", file, NULL };
	const char *editor;

	editor = getenv("GIT_EDITOR");
	if (!editor && *opt_editor)
		editor = opt_editor;
	if (!editor)
		editor = getenv("VISUAL");
	if (!editor)
		editor = getenv("EDITOR");
	if (!editor)
		editor = "vi";

	editor_argv[0] = editor;
	open_external_viewer(editor_argv, from_root ? opt_cdup : NULL);
}

static void
open_run_request(enum request request)
{
	struct run_request *req = get_run_request(request);
	const char *argv[ARRAY_SIZE(req->argv)] = { NULL };

	if (!req) {
		report("Unknown run request");
		return;
	}

	if (format_argv(argv, req->argv, FORMAT_ALL))
		open_external_viewer(argv, NULL);
	free_argv(argv);
}

/*
 * User request switch noodle
 */

static int
view_driver(struct view *view, enum request request)
{
	int i;

	if (request == REQ_NONE) {
		doupdate();
		return TRUE;
	}

	if (request > REQ_NONE) {
		open_run_request(request);
		/* FIXME: When all views can refresh always do this. */
		if (view == VIEW(REQ_VIEW_STATUS) ||
		    view == VIEW(REQ_VIEW_MAIN) ||
		    view == VIEW(REQ_VIEW_LOG) ||
		    view == VIEW(REQ_VIEW_STAGE))
			request = REQ_REFRESH;
		else
			return TRUE;
	}

	if (view && view->lines) {
		request = view->ops->request(view, request, &view->line[view->lineno]);
		if (request == REQ_NONE)
			return TRUE;
	}

	switch (request) {
	case REQ_MOVE_UP:
	case REQ_MOVE_DOWN:
	case REQ_MOVE_PAGE_UP:
	case REQ_MOVE_PAGE_DOWN:
	case REQ_MOVE_FIRST_LINE:
	case REQ_MOVE_LAST_LINE:
		move_view(view, request);
		break;

	case REQ_SCROLL_LEFT:
	case REQ_SCROLL_RIGHT:
	case REQ_SCROLL_LINE_DOWN:
	case REQ_SCROLL_LINE_UP:
	case REQ_SCROLL_PAGE_DOWN:
	case REQ_SCROLL_PAGE_UP:
		scroll_view(view, request);
		break;

	case REQ_VIEW_BLAME:
		if (!opt_file[0]) {
			report("No file chosen, press %s to open tree view",
			       get_key(REQ_VIEW_TREE));
			break;
		}
		open_view(view, request, OPEN_DEFAULT);
		break;

	case REQ_VIEW_BLOB:
		if (!ref_blob[0]) {
			report("No file chosen, press %s to open tree view",
			       get_key(REQ_VIEW_TREE));
			break;
		}
		open_view(view, request, OPEN_DEFAULT);
		break;

	case REQ_VIEW_PAGER:
		if (!VIEW(REQ_VIEW_PAGER)->pipe && !VIEW(REQ_VIEW_PAGER)->lines) {
			report("No pager content, press %s to run command from prompt",
			       get_key(REQ_PROMPT));
			break;
		}
		open_view(view, request, OPEN_DEFAULT);
		break;

	case REQ_VIEW_STAGE:
		if (!VIEW(REQ_VIEW_STAGE)->lines) {
			report("No stage content, press %s to open the status view and choose file",
			       get_key(REQ_VIEW_STATUS));
			break;
		}
		open_view(view, request, OPEN_DEFAULT);
		break;

	case REQ_VIEW_STATUS:
		if (opt_is_inside_work_tree == FALSE) {
			report("The status view requires a working tree");
			break;
		}
		open_view(view, request, OPEN_DEFAULT);
		break;

	case REQ_VIEW_MAIN:
	case REQ_VIEW_DIFF:
	case REQ_VIEW_LOG:
	case REQ_VIEW_TREE:
	case REQ_VIEW_HELP:
		open_view(view, request, OPEN_DEFAULT);
		break;

	case REQ_NEXT:
	case REQ_PREVIOUS:
		request = request == REQ_NEXT ? REQ_MOVE_DOWN : REQ_MOVE_UP;

		if ((view == VIEW(REQ_VIEW_DIFF) &&
		     view->parent == VIEW(REQ_VIEW_MAIN)) ||
		   (view == VIEW(REQ_VIEW_DIFF) &&
		     view->parent == VIEW(REQ_VIEW_BLAME)) ||
		   (view == VIEW(REQ_VIEW_STAGE) &&
		     view->parent == VIEW(REQ_VIEW_STATUS)) ||
		   (view == VIEW(REQ_VIEW_BLOB) &&
		     view->parent == VIEW(REQ_VIEW_TREE))) {
			int line;

			view = view->parent;
			line = view->lineno;
			move_view(view, request);
			if (view_is_displayed(view))
				update_view_title(view);
			if (line != view->lineno)
				view->ops->request(view, REQ_ENTER,
						   &view->line[view->lineno]);

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
		report("");
		break;
	}
	case REQ_REFRESH:
		report("Refreshing is not yet supported for the %s view", view->name);
		break;

	case REQ_MAXIMIZE:
		if (displayed_views() == 2)
			open_view(view, VIEW_REQ(view), OPEN_DEFAULT);
		break;

	case REQ_TOGGLE_LINENO:
		toggle_view_option(&opt_line_number, "line numbers");
		break;

	case REQ_TOGGLE_DATE:
		toggle_view_option(&opt_date, "date display");
		break;

	case REQ_TOGGLE_AUTHOR:
		toggle_view_option(&opt_author, "author display");
		break;

	case REQ_TOGGLE_REV_GRAPH:
		toggle_view_option(&opt_rev_graph, "revision graph display");
		break;

	case REQ_TOGGLE_REFS:
		toggle_view_option(&opt_show_refs, "reference display");
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
		for (i = 0; i < ARRAY_SIZE(views); i++) {
			view = &views[i];
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
		/* XXX: Mark closed views by letting view->parent point to the
		 * view itself. Parents to closed view should never be
		 * followed. */
		if (view->parent &&
		    view->parent->parent != view->parent) {
			memset(display, 0, sizeof(display));
			current_view = 0;
			display[current_view] = view->parent;
			view->parent = view;
			resize_display();
			redraw_display(FALSE);
			report("");
			break;
		}
		/* Fall-through */
	case REQ_QUIT:
		return FALSE;

	default:
		report("Unknown key, press 'h' for help");
		return TRUE;
	}

	return TRUE;
}


/*
 * View backend utilities
 */

/* Parse author lines where the name may be empty:
 *	author  <email@address.tld> 1138474660 +0100
 */
static void
parse_author_line(char *ident, char *author, size_t authorsize, struct tm *tm)
{
	char *nameend = strchr(ident, '<');
	char *emailend = strchr(ident, '>');

	if (nameend && emailend)
		*nameend = *emailend = 0;
	ident = chomp_string(ident);
	if (!*ident) {
		if (nameend)
			ident = chomp_string(nameend + 1);
		if (!*ident)
			ident = "Unknown";
	}

	string_ncopy_do(author, authorsize, ident, strlen(ident));

	/* Parse epoch and timezone */
	if (emailend && emailend[1] == ' ') {
		char *secs = emailend + 2;
		char *zone = strchr(secs, ' ');
		time_t time = (time_t) atol(secs);

		if (zone && strlen(zone) == STRING_SIZE(" +0700")) {
			long tz;

			zone++;
			tz  = ('0' - zone[1]) * 60 * 60 * 10;
			tz += ('0' - zone[2]) * 60 * 60;
			tz += ('0' - zone[3]) * 60;
			tz += ('0' - zone[4]) * 60;

			if (zone[0] == '-')
				tz = -tz;

			time -= tz;
		}

		gmtime_r(&time, tm);
	}
}

static enum input_status
select_commit_parent_handler(void *data, char *buf, int c)
{
	size_t parents = *(size_t *) data;
	int parent = 0;

	if (!isdigit(c))
		return INPUT_SKIP;

	if (*buf)
		parent = atoi(buf) * 10;
	parent += c - '0';

	if (parent > parents)
		return INPUT_SKIP;
	return INPUT_OK;
}

static bool
select_commit_parent(const char *id, char rev[SIZEOF_REV])
{
	char buf[SIZEOF_STR * 4];
	const char *revlist_argv[] = {
		"git", "rev-list", "-1", "--parents", id, NULL
	};
	int parents;

	if (!run_io_buf(revlist_argv, buf, sizeof(buf)) ||
	    !*chomp_string(buf) ||
	    (parents = (strlen(buf) / 40) - 1) < 0) {
		report("Failed to get parent information");
		return FALSE;

	} else if (parents == 0) {
		report("The selected commit has no parents");
		return FALSE;
	}

	if (parents > 1) {
		char prompt[SIZEOF_STR];
		char *result;

		if (!string_format(prompt, "Which parent? [1..%d] ", parents))
			return FALSE;
		result = prompt_input(prompt, select_commit_parent_handler, &parents);
		if (!result)
			return FALSE;
		parents = atoi(result);
	}

	string_copy_rev(rev, &buf[41 * parents]);
	return TRUE;
}

/*
 * Pager backend
 */

static bool
pager_draw(struct view *view, struct line *line, unsigned int lineno)
{
	char text[SIZEOF_STR];

	if (opt_line_number && draw_lineno(view, lineno))
		return TRUE;

	string_expand(text, sizeof(text), line->data, opt_tab_size);
	draw_text(view, line->type, text, TRUE);
	return TRUE;
}

static bool
add_describe_ref(char *buf, size_t *bufpos, const char *commit_id, const char *sep)
{
	const char *describe_argv[] = { "git", "describe", commit_id, NULL };
	char refbuf[SIZEOF_STR];
	char *ref = NULL;

	if (run_io_buf(describe_argv, refbuf, sizeof(refbuf)))
		ref = chomp_string(refbuf);

	if (!ref || !*ref)
		return TRUE;

	/* This is the only fatal call, since it can "corrupt" the buffer. */
	if (!string_nformat(buf, SIZEOF_STR, bufpos, "%s%s", sep, ref))
		return FALSE;

	return TRUE;
}

static void
add_pager_refs(struct view *view, struct line *line)
{
	char buf[SIZEOF_STR];
	char *commit_id = (char *)line->data + STRING_SIZE("commit ");
	struct ref **refs;
	size_t bufpos = 0, refpos = 0;
	const char *sep = "Refs: ";
	bool is_tag = FALSE;

	assert(line->type == LINE_COMMIT);

	refs = get_refs(commit_id);
	if (!refs) {
		if (view == VIEW(REQ_VIEW_DIFF))
			goto try_add_describe_ref;
		return;
	}

	do {
		struct ref *ref = refs[refpos];
		const char *fmt = ref->tag    ? "%s[%s]" :
		                  ref->remote ? "%s<%s>" : "%s%s";

		if (!string_format_from(buf, &bufpos, fmt, sep, ref->name))
			return;
		sep = ", ";
		if (ref->tag)
			is_tag = TRUE;
	} while (refs[refpos++]->next);

	if (!is_tag && view == VIEW(REQ_VIEW_DIFF)) {
try_add_describe_ref:
		/* Add <tag>-g<commit_id> "fake" reference. */
		if (!add_describe_ref(buf, &bufpos, commit_id, sep))
			return;
	}

	if (bufpos == 0)
		return;

	add_line_text(view, buf, LINE_PP_REFS);
}

static bool
pager_read(struct view *view, char *data)
{
	struct line *line;

	if (!data)
		return TRUE;

	line = add_line_text(view, data, get_line_type(data));
	if (!line)
		return FALSE;

	if (line->type == LINE_COMMIT &&
	    (view == VIEW(REQ_VIEW_DIFF) ||
	     view == VIEW(REQ_VIEW_LOG)))
		add_pager_refs(view, line);

	return TRUE;
}

static enum request
pager_request(struct view *view, enum request request, struct line *line)
{
	int split = 0;

	if (request != REQ_ENTER)
		return request;

	if (line->type == LINE_COMMIT &&
	   (view == VIEW(REQ_VIEW_LOG) ||
	    view == VIEW(REQ_VIEW_PAGER))) {
		open_view(view, REQ_VIEW_DIFF, OPEN_SPLIT);
		split = 1;
	}

	/* Always scroll the view even if it was split. That way
	 * you can use Enter to scroll through the log view and
	 * split open each commit diff. */
	scroll_view(view, REQ_SCROLL_LINE_DOWN);

	/* FIXME: A minor workaround. Scrolling the view will call report("")
	 * but if we are scrolling a non-current view this won't properly
	 * update the view title. */
	if (split)
		update_view_title(view);

	return REQ_NONE;
}

static bool
pager_grep(struct view *view, struct line *line)
{
	regmatch_t pmatch;
	char *text = line->data;

	if (!*text)
		return FALSE;

	if (regexec(view->regex, text, 1, &pmatch, 0) == REG_NOMATCH)
		return FALSE;

	return TRUE;
}

static void
pager_select(struct view *view, struct line *line)
{
	if (line->type == LINE_COMMIT) {
		char *text = (char *)line->data + STRING_SIZE("commit ");

		if (view != VIEW(REQ_VIEW_PAGER))
			string_copy_rev(view->ref, text);
		string_copy_rev(ref_commit, text);
	}
}

static struct view_ops pager_ops = {
	"line",
	NULL,
	NULL,
	pager_read,
	pager_draw,
	pager_request,
	pager_grep,
	pager_select,
};

static const char *log_argv[SIZEOF_ARG] = {
	"git", "log", "--no-color", "--cc", "--stat", "-n100", "%(head)", NULL
};

static enum request
log_request(struct view *view, enum request request, struct line *line)
{
	switch (request) {
	case REQ_REFRESH:
		load_refs();
		open_view(view, REQ_VIEW_LOG, OPEN_REFRESH);
		return REQ_NONE;
	default:
		return pager_request(view, request, line);
	}
}

static struct view_ops log_ops = {
	"line",
	log_argv,
	NULL,
	pager_read,
	pager_draw,
	log_request,
	pager_grep,
	pager_select,
};

static const char *diff_argv[SIZEOF_ARG] = {
	"git", "show", "--pretty=fuller", "--no-color", "--root",
		"--patch-with-stat", "--find-copies-harder", "-C", "%(commit)", NULL
};

static struct view_ops diff_ops = {
	"line",
	diff_argv,
	NULL,
	pager_read,
	pager_draw,
	pager_request,
	pager_grep,
	pager_select,
};

/*
 * Help backend
 */

static bool
help_open(struct view *view)
{
	char buf[SIZEOF_STR];
	size_t bufpos;
	int i;

	if (view->lines > 0)
		return TRUE;

	add_line_text(view, "Quick reference for tig keybindings:", LINE_DEFAULT);

	for (i = 0; i < ARRAY_SIZE(req_info); i++) {
		const char *key;

		if (req_info[i].request == REQ_NONE)
			continue;

		if (!req_info[i].request) {
			add_line_text(view, "", LINE_DEFAULT);
			add_line_text(view, req_info[i].help, LINE_DEFAULT);
			continue;
		}

		key = get_key(req_info[i].request);
		if (!*key)
			key = "(no key defined)";

		for (bufpos = 0; bufpos <= req_info[i].namelen; bufpos++) {
			buf[bufpos] = tolower(req_info[i].name[bufpos]);
			if (buf[bufpos] == '_')
				buf[bufpos] = '-';
		}

		add_line_format(view, LINE_DEFAULT, "    %-25s %-20s %s",
				key, buf, req_info[i].help);
	}

	if (run_requests) {
		add_line_text(view, "", LINE_DEFAULT);
		add_line_text(view, "External commands:", LINE_DEFAULT);
	}

	for (i = 0; i < run_requests; i++) {
		struct run_request *req = get_run_request(REQ_NONE + i + 1);
		const char *key;
		int argc;

		if (!req)
			continue;

		key = get_key_name(req->key);
		if (!*key)
			key = "(no key defined)";

		for (bufpos = 0, argc = 0; req->argv[argc]; argc++)
			if (!string_format_from(buf, &bufpos, "%s%s",
					        argc ? " " : "", req->argv[argc]))
				return REQ_NONE;

		add_line_format(view, LINE_DEFAULT, "    %-10s %-14s `%s`",
				keymap_table[req->keymap].name, key, buf);
	}

	return TRUE;
}

static struct view_ops help_ops = {
	"line",
	NULL,
	help_open,
	NULL,
	pager_draw,
	pager_request,
	pager_grep,
	pager_select,
};


/*
 * Tree backend
 */

struct tree_stack_entry {
	struct tree_stack_entry *prev;	/* Entry below this in the stack */
	unsigned long lineno;		/* Line number to restore */
	char *name;			/* Position of name in opt_path */
};

/* The top of the path stack. */
static struct tree_stack_entry *tree_stack = NULL;
unsigned long tree_lineno = 0;

static void
pop_tree_stack_entry(void)
{
	struct tree_stack_entry *entry = tree_stack;

	tree_lineno = entry->lineno;
	entry->name[0] = 0;
	tree_stack = entry->prev;
	free(entry);
}

static void
push_tree_stack_entry(const char *name, unsigned long lineno)
{
	struct tree_stack_entry *entry = calloc(1, sizeof(*entry));
	size_t pathlen = strlen(opt_path);

	if (!entry)
		return;

	entry->prev = tree_stack;
	entry->name = opt_path + pathlen;
	tree_stack = entry;

	if (!string_format_from(opt_path, &pathlen, "%s/", name)) {
		pop_tree_stack_entry();
		return;
	}

	/* Move the current line to the first tree entry. */
	tree_lineno = 1;
	entry->lineno = lineno;
}

/* Parse output from git-ls-tree(1):
 *
 * 100644 blob fb0e31ea6cc679b7379631188190e975f5789c26	Makefile
 * 100644 blob 5304ca4260aaddaee6498f9630e7d471b8591ea6	README
 * 100644 blob f931e1d229c3e185caad4449bf5b66ed72462657	tig.c
 * 100644 blob ed09fe897f3c7c9af90bcf80cae92558ea88ae38	web.conf
 */

#define SIZEOF_TREE_ATTR \
	STRING_SIZE("100644 blob ed09fe897f3c7c9af90bcf80cae92558ea88ae38\t")

#define SIZEOF_TREE_MODE \
	STRING_SIZE("100644 ")

#define TREE_ID_OFFSET \
	STRING_SIZE("100644 blob ")

struct tree_entry {
	char id[SIZEOF_REV];
	mode_t mode;
	struct tm time;			/* Date from the author ident. */
	char author[75];		/* Author of the commit. */
	char name[1];
};

static const char *
tree_path(struct line *line)
{
	return ((struct tree_entry *) line->data)->name;
}


static int
tree_compare_entry(struct line *line1, struct line *line2)
{
	if (line1->type != line2->type)
		return line1->type == LINE_TREE_DIR ? -1 : 1;
	return strcmp(tree_path(line1), tree_path(line2));
}

static struct line *
tree_entry(struct view *view, enum line_type type, const char *path,
	   const char *mode, const char *id)
{
	struct tree_entry *entry = calloc(1, sizeof(*entry) + strlen(path));
	struct line *line = entry ? add_line_data(view, entry, type) : NULL;

	if (!entry || !line) {
		free(entry);
		return NULL;
	}

	strncpy(entry->name, path, strlen(path));
	if (mode)
		entry->mode = strtoul(mode, NULL, 8);
	if (id)
		string_copy_rev(entry->id, id);

	return line;
}

static bool
tree_read_date(struct view *view, char *text, bool *read_date)
{
	static char author_name[SIZEOF_STR];
	static struct tm author_time;

	if (!text && *read_date) {
		*read_date = FALSE;
		return TRUE;

	} else if (!text) {
		char *path = *opt_path ? opt_path : ".";
		/* Find next entry to process */
		const char *log_file[] = {
			"git", "log", "--no-color", "--pretty=raw",
				"--cc", "--raw", view->id, "--", path, NULL
		};
		struct io io = {};

		if (!view->lines) {
			tree_entry(view, LINE_TREE_PARENT, opt_path, NULL, NULL);
			report("Tree is empty");
			return TRUE;
		}

		if (!run_io_rd(&io, log_file, FORMAT_NONE)) {
			report("Failed to load tree data");
			return TRUE;
		}

		done_io(view->pipe);
		view->io = io;
		*read_date = TRUE;
		return FALSE;

	} else if (*text == 'a' && get_line_type(text) == LINE_AUTHOR) {
		parse_author_line(text + STRING_SIZE("author "),
				  author_name, sizeof(author_name), &author_time);

	} else if (*text == ':') {
		char *pos;
		size_t annotated = 1;
		size_t i;

		pos = strchr(text, '\t');
		if (!pos)
			return TRUE;
		text = pos + 1;
		if (*opt_prefix && !strncmp(text, opt_prefix, strlen(opt_prefix)))
			text += strlen(opt_prefix);
		if (*opt_path && !strncmp(text, opt_path, strlen(opt_path)))
			text += strlen(opt_path);
		pos = strchr(text, '/');
		if (pos)
			*pos = 0;

		for (i = 1; i < view->lines; i++) {
			struct line *line = &view->line[i];
			struct tree_entry *entry = line->data;

			annotated += !!*entry->author;
			if (*entry->author || strcmp(entry->name, text))
				continue;

			string_copy(entry->author, author_name);
			memcpy(&entry->time, &author_time, sizeof(entry->time));
			line->dirty = 1;
			break;
		}

		if (annotated == view->lines)
			kill_io(view->pipe);
	}
	return TRUE;
}

static bool
tree_read(struct view *view, char *text)
{
	static bool read_date = FALSE;
	struct tree_entry *data;
	struct line *entry, *line;
	enum line_type type;
	size_t textlen = text ? strlen(text) : 0;
	char *path = text + SIZEOF_TREE_ATTR;

	if (read_date || !text)
		return tree_read_date(view, text, &read_date);

	if (textlen <= SIZEOF_TREE_ATTR)
		return FALSE;
	if (view->lines == 0 &&
	    !tree_entry(view, LINE_TREE_PARENT, opt_path, NULL, NULL))
		return FALSE;

	/* Strip the path part ... */
	if (*opt_path) {
		size_t pathlen = textlen - SIZEOF_TREE_ATTR;
		size_t striplen = strlen(opt_path);

		if (pathlen > striplen)
			memmove(path, path + striplen,
				pathlen - striplen + 1);

		/* Insert "link" to parent directory. */
		if (view->lines == 1 &&
		    !tree_entry(view, LINE_TREE_DIR, "..", "040000", view->ref))
			return FALSE;
	}

	type = text[SIZEOF_TREE_MODE] == 't' ? LINE_TREE_DIR : LINE_TREE_FILE;
	entry = tree_entry(view, type, path, text, text + TREE_ID_OFFSET);
	if (!entry)
		return FALSE;
	data = entry->data;

	/* Skip "Directory ..." and ".." line. */
	for (line = &view->line[1 + !!*opt_path]; line < entry; line++) {
		if (tree_compare_entry(line, entry) <= 0)
			continue;

		memmove(line + 1, line, (entry - line) * sizeof(*entry));

		line->data = data;
		line->type = type;
		for (; line <= entry; line++)
			line->dirty = line->cleareol = 1;
		return TRUE;
	}

	if (tree_lineno > view->lineno) {
		view->lineno = tree_lineno;
		tree_lineno = 0;
	}

	return TRUE;
}

static bool
tree_draw(struct view *view, struct line *line, unsigned int lineno)
{
	struct tree_entry *entry = line->data;

	if (line->type == LINE_TREE_PARENT) {
		if (draw_text(view, line->type, "Directory path /", TRUE))
			return TRUE;
	} else {
		if (draw_mode(view, entry->mode))
			return TRUE;

		if (opt_author && draw_author(view, entry->author))
			return TRUE;

		if (opt_date && draw_date(view, *entry->author ? &entry->time : NULL))
			return TRUE;
	}
	if (draw_text(view, line->type, entry->name, TRUE))
		return TRUE;
	return TRUE;
}

static void
open_blob_editor()
{
	char file[SIZEOF_STR] = "/tmp/tigblob.XXXXXX";
	int fd = mkstemp(file);

	if (fd == -1)
		report("Failed to create temporary file");
	else if (!run_io_append(blob_ops.argv, FORMAT_ALL, fd))
		report("Failed to save blob data to file");
	else
		open_editor(FALSE, file);
	if (fd != -1)
		unlink(file);
}

static enum request
tree_request(struct view *view, enum request request, struct line *line)
{
	enum open_flags flags;

	switch (request) {
	case REQ_VIEW_BLAME:
		if (line->type != LINE_TREE_FILE) {
			report("Blame only supported for files");
			return REQ_NONE;
		}

		string_copy(opt_ref, view->vid);
		return request;

	case REQ_EDIT:
		if (line->type != LINE_TREE_FILE) {
			report("Edit only supported for files");
		} else if (!is_head_commit(view->vid)) {
			open_blob_editor();
		} else {
			open_editor(TRUE, opt_file);
		}
		return REQ_NONE;

	case REQ_PARENT:
		if (!*opt_path) {
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
	while (!*opt_path && tree_stack)
		pop_tree_stack_entry();

	switch (line->type) {
	case LINE_TREE_DIR:
		/* Depending on whether it is a subdir or parent (updir?) link
		 * mangle the path buffer. */
		if (line == &view->line[1] && *opt_path) {
			pop_tree_stack_entry();

		} else {
			const char *basename = tree_path(line);

			push_tree_stack_entry(basename, view->lineno);
		}

		/* Trees and subtrees share the same ID, so they are not not
		 * unique like blobs. */
		flags = OPEN_RELOAD;
		request = REQ_VIEW_TREE;
		break;

	case LINE_TREE_FILE:
		flags = display[0] == view ? OPEN_SPLIT : OPEN_DEFAULT;
		request = REQ_VIEW_BLOB;
		break;

	default:
		return REQ_NONE;
	}

	open_view(view, request, flags);
	if (request == REQ_VIEW_TREE)
		view->lineno = tree_lineno;

	return REQ_NONE;
}

static void
tree_select(struct view *view, struct line *line)
{
	struct tree_entry *entry = line->data;

	if (line->type == LINE_TREE_FILE) {
		string_copy_rev(ref_blob, entry->id);
		string_format(opt_file, "%s%s", opt_path, tree_path(line));

	} else if (line->type != LINE_TREE_DIR) {
		return;
	}

	string_copy_rev(view->ref, entry->id);
}

static const char *tree_argv[SIZEOF_ARG] = {
	"git", "ls-tree", "%(commit)", "%(directory)", NULL
};

static struct view_ops tree_ops = {
	"file",
	tree_argv,
	NULL,
	tree_read,
	tree_draw,
	tree_request,
	pager_grep,
	tree_select,
};

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
	case REQ_EDIT:
		open_blob_editor();
		return REQ_NONE;
	default:
		return pager_request(view, request, line);
	}
}

static const char *blob_argv[SIZEOF_ARG] = {
	"git", "cat-file", "blob", "%(blob)", NULL
};

static struct view_ops blob_ops = {
	"line",
	blob_argv,
	NULL,
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
 *  1. File content is read either using opt_file from the
 *     filesystem or using git-cat-file.
 *  2. Then blame information is incrementally added by
 *     reading output from git-blame.
 */

static const char *blame_head_argv[] = {
	"git", "blame", "--incremental", "--", "%(file)", NULL
};

static const char *blame_ref_argv[] = {
	"git", "blame", "--incremental", "%(ref)", "--", "%(file)", NULL
};

static const char *blame_cat_file_argv[] = {
	"git", "cat-file", "blob", "%(ref):%(file)", NULL
};

struct blame_commit {
	char id[SIZEOF_REV];		/* SHA1 ID. */
	char title[128];		/* First line of the commit message. */
	char author[75];		/* Author of the commit. */
	struct tm time;			/* Date from the author ident. */
	char filename[128];		/* Name of file. */
	bool has_previous;		/* Was a "previous" line detected. */
};

struct blame {
	struct blame_commit *commit;
	char text[1];
};

static bool
blame_open(struct view *view)
{
	if (*opt_ref || !io_open(&view->io, opt_file)) {
		if (!run_io_rd(&view->io, blame_cat_file_argv, FORMAT_ALL))
			return FALSE;
	}

	setup_update(view, opt_file);
	string_format(view->ref, "%s ...", opt_file);

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

static bool
parse_number(const char **posref, size_t *number, size_t min, size_t max)
{
	const char *pos = *posref;

	*posref = NULL;
	pos = strchr(pos + 1, ' ');
	if (!pos || !isdigit(pos[1]))
		return FALSE;
	*number = atoi(pos + 1);
	if (*number < min || *number > max)
		return FALSE;

	*posref = pos;
	return TRUE;
}

static struct blame_commit *
parse_blame_commit(struct view *view, const char *text, int *blamed)
{
	struct blame_commit *commit;
	struct blame *blame;
	const char *pos = text + SIZEOF_REV - 1;
	size_t lineno;
	size_t group;

	if (strlen(text) <= SIZEOF_REV || *pos != ' ')
		return NULL;

	if (!parse_number(&pos, &lineno, 1, view->lines) ||
	    !parse_number(&pos, &group, 1, view->lines - lineno + 1))
		return NULL;

	commit = get_blame_commit(view, text);
	if (!commit)
		return NULL;

	*blamed += group;
	while (group--) {
		struct line *line = &view->line[lineno + group - 1];

		blame = line->data;
		blame->commit = commit;
		line->dirty = 1;
	}

	return commit;
}

static bool
blame_read_file(struct view *view, const char *line, bool *read_file)
{
	if (!line) {
		const char **argv = *opt_ref ? blame_ref_argv : blame_head_argv;
		struct io io = {};

		if (view->lines == 0 && !view->parent)
			die("No blame exist for %s", view->vid);

		if (view->lines == 0 || !run_io_rd(&io, argv, FORMAT_ALL)) {
			report("Failed to load blame data");
			return TRUE;
		}

		done_io(view->pipe);
		view->io = io;
		*read_file = FALSE;
		return FALSE;

	} else {
		size_t linelen = string_expand_length(line, opt_tab_size);
		struct blame *blame = malloc(sizeof(*blame) + linelen);

		if (!blame)
			return FALSE;

		blame->commit = NULL;
		string_expand(blame->text, linelen + 1, line, opt_tab_size);
		return add_line_data(view, blame, LINE_BLAME_ID) != NULL;
	}
}

static bool
match_blame_header(const char *name, char **line)
{
	size_t namelen = strlen(name);
	bool matched = !strncmp(name, *line, namelen);

	if (matched)
		*line += namelen;

	return matched;
}

static bool
blame_read(struct view *view, char *line)
{
	static struct blame_commit *commit = NULL;
	static int blamed = 0;
	static time_t author_time;
	static bool read_file = TRUE;

	if (read_file)
		return blame_read_file(view, line, &read_file);

	if (!line) {
		/* Reset all! */
		commit = NULL;
		blamed = 0;
		read_file = TRUE;
		string_format(view->ref, "%s", view->vid);
		if (view_is_displayed(view)) {
			update_view_title(view);
			redraw_view_from(view, 0);
		}
		return TRUE;
	}

	if (!commit) {
		commit = parse_blame_commit(view, line, &blamed);
		string_format(view->ref, "%s %2d%%", view->vid,
			      view->lines ? blamed * 100 / view->lines : 0);

	} else if (match_blame_header("author ", &line)) {
		string_ncopy(commit->author, line, strlen(line));

	} else if (match_blame_header("author-time ", &line)) {
		author_time = (time_t) atol(line);

	} else if (match_blame_header("author-tz ", &line)) {
		long tz;

		tz  = ('0' - line[1]) * 60 * 60 * 10;
		tz += ('0' - line[2]) * 60 * 60;
		tz += ('0' - line[3]) * 60;
		tz += ('0' - line[4]) * 60;

		if (line[0] == '-')
			tz = -tz;

		author_time -= tz;
		gmtime_r(&author_time, &commit->time);

	} else if (match_blame_header("summary ", &line)) {
		string_ncopy(commit->title, line, strlen(line));

	} else if (match_blame_header("previous ", &line)) {
		commit->has_previous = TRUE;

	} else if (match_blame_header("filename ", &line)) {
		string_ncopy(commit->filename, line, strlen(line));
		commit = NULL;
	}

	return TRUE;
}

static bool
blame_draw(struct view *view, struct line *line, unsigned int lineno)
{
	struct blame *blame = line->data;
	struct tm *time = NULL;
	const char *id = NULL, *author = NULL;

	if (blame->commit && *blame->commit->filename) {
		id = blame->commit->id;
		author = blame->commit->author;
		time = &blame->commit->time;
	}

	if (opt_date && draw_date(view, time))
		return TRUE;

	if (opt_author && draw_author(view, author))
		return TRUE;

	if (draw_field(view, LINE_BLAME_ID, id, ID_COLS, FALSE))
		return TRUE;

	if (draw_lineno(view, lineno))
		return TRUE;

	draw_text(view, LINE_DEFAULT, blame->text, TRUE);
	return TRUE;
}

static bool
check_blame_commit(struct blame *blame)
{
	if (!blame->commit)
		report("Commit data not loaded yet");
	else if (!strcmp(blame->commit->id, NULL_ID))
		report("No commit exist for the selected line");
	else
		return TRUE;
	return FALSE;
}

static enum request
blame_request(struct view *view, enum request request, struct line *line)
{
	enum open_flags flags = display[0] == view ? OPEN_SPLIT : OPEN_DEFAULT;
	struct blame *blame = line->data;

	switch (request) {
	case REQ_VIEW_BLAME:
		if (check_blame_commit(blame)) {
			string_copy(opt_ref, blame->commit->id);
			open_view(view, REQ_VIEW_BLAME, OPEN_REFRESH);
		}
		break;

	case REQ_PARENT:
		if (check_blame_commit(blame) &&
		    select_commit_parent(blame->commit->id, opt_ref))
			open_view(view, REQ_VIEW_BLAME, OPEN_REFRESH);
		break;

	case REQ_ENTER:
		if (!blame->commit) {
			report("No commit loaded yet");
			break;
		}

		if (view_is_displayed(VIEW(REQ_VIEW_DIFF)) &&
		    !strcmp(blame->commit->id, VIEW(REQ_VIEW_DIFF)->ref))
			break;

		if (!strcmp(blame->commit->id, NULL_ID)) {
			struct view *diff = VIEW(REQ_VIEW_DIFF);
			const char *diff_index_argv[] = {
				"git", "diff-index", "--root", "--patch-with-stat",
					"-C", "-M", "HEAD", "--", view->vid, NULL
			};

			if (!blame->commit->has_previous) {
				diff_index_argv[1] = "diff";
				diff_index_argv[2] = "--no-color";
				diff_index_argv[6] = "--";
				diff_index_argv[7] = "/dev/null";
			}

			if (!prepare_update(diff, diff_index_argv, NULL, FORMAT_DASH)) {
				report("Failed to allocate diff command");
				break;
			}
			flags |= OPEN_PREPARED;
		}

		open_view(view, REQ_VIEW_DIFF, flags);
		if (VIEW(REQ_VIEW_DIFF)->pipe && !strcmp(blame->commit->id, NULL_ID))
			string_copy_rev(VIEW(REQ_VIEW_DIFF)->ref, NULL_ID);
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
	regmatch_t pmatch;

#define MATCH(text, on)							\
	(on && *text && regexec(view->regex, text, 1, &pmatch, 0) != REG_NOMATCH)

	if (commit) {
		char buf[DATE_COLS + 1];

		if (MATCH(commit->title, 1) ||
		    MATCH(commit->author, opt_author) ||
		    MATCH(commit->id, opt_date))
			return TRUE;

		if (strftime(buf, sizeof(buf), DATE_FORMAT, &commit->time) &&
		    MATCH(buf, 1))
			return TRUE;
	}

	return MATCH(blame->text, 1);

#undef MATCH
}

static void
blame_select(struct view *view, struct line *line)
{
	struct blame *blame = line->data;
	struct blame_commit *commit = blame->commit;

	if (!commit)
		return;

	if (!strcmp(commit->id, NULL_ID))
		string_ncopy(ref_commit, "HEAD", 4);
	else
		string_copy_rev(ref_commit, commit->id);
}

static struct view_ops blame_ops = {
	"line",
	NULL,
	blame_open,
	blame_read,
	blame_draw,
	blame_request,
	blame_grep,
	blame_select,
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
static size_t stage_chunks;
static int *stage_chunk;

/* This should work even for the "On branch" line. */
static inline bool
status_has_none(struct view *view, struct line *line)
{
	return line < view->line + view->lines && !line[1].data;
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
	struct io io = {};

	if (!run_io(&io, argv, NULL, IO_RD))
		return FALSE;

	add_line_data(view, NULL, type);

	while ((buf = io_get(&io, 0, TRUE))) {
		struct status *file = unmerged;

		if (!file) {
			file = calloc(1, sizeof(*file));
			if (!file || !add_line_data(view, file, type))
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

			/* Collapse all 'M'odified entries that follow a
			 * associated 'U'nmerged entry. */
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
		done_io(&io);
		return FALSE;
	}

	if (!view->line[view->lines - 1].data)
		add_line_data(view, NULL, LINE_STAT_NONE);

	done_io(&io);
	return TRUE;
}

/* Don't show unmerged entries in the staged section. */
static const char *status_diff_index_argv[] = {
	"git", "diff-index", "-z", "--diff-filter=ACDMRTXB",
			     "--cached", "-M", "HEAD", NULL
};

static const char *status_diff_files_argv[] = {
	"git", "diff-files", "-z", NULL
};

static const char *status_list_other_argv[] = {
	"git", "ls-files", "-z", "--others", "--exclude-standard", NULL
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
	if (view->p_lineno >= view->lines)
		view->p_lineno = view->lines - 1;
	while (view->p_lineno < view->lines && !view->line[view->p_lineno].data)
		view->p_lineno++;
	while (view->p_lineno > 0 && !view->line[view->p_lineno].data)
		view->p_lineno--;

	/* If the above fails, always skip the "On branch" line. */
	if (view->p_lineno < view->lines)
		view->lineno = view->p_lineno;
	else
		view->lineno = 1;

	if (view->lineno < view->offset)
		view->offset = view->lineno;
	else if (view->offset + view->height <= view->lineno)
		view->offset = view->lineno - view->height + 1;

	view->p_restore = FALSE;
}

/* First parse staged info using git-diff-index(1), then parse unstaged
 * info using git-diff-files(1), and finally untracked files using
 * git-ls-files(1). */
static bool
status_open(struct view *view)
{
	reset_view(view);

	add_line_data(view, NULL, LINE_STAT_HEAD);
	if (is_initial_commit())
		string_copy(status_onbranch, "Initial commit");
	else if (!*opt_head)
		string_copy(status_onbranch, "Not currently on any branch");
	else if (!string_format(status_onbranch, "On branch %s", opt_head))
		return FALSE;

	run_io_bg(update_index_argv);

	if (is_initial_commit()) {
		if (!status_run(view, status_list_no_head_argv, 'A', LINE_STAT_STAGED))
			return FALSE;
	} else if (!status_run(view, status_diff_index_argv, 0, LINE_STAT_STAGED)) {
		return FALSE;
	}

	if (!status_run(view, status_diff_files_argv, 0, LINE_STAT_UNSTAGED) ||
	    !status_run(view, status_list_other_argv, '?', LINE_STAT_UNTRACKED))
		return FALSE;

	/* Restore the exact position or use the specialized restore
	 * mode? */
	if (!view->p_restore)
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
		if (draw_text(view, line->type, buf, TRUE))
			return TRUE;
		type = LINE_DEFAULT;
		text = status->new.name;
	}

	draw_text(view, type, text, TRUE);
	return TRUE;
}

static enum request
status_enter(struct view *view, struct line *line)
{
	struct status *status = line->data;
	const char *oldpath = status ? status->old.name : NULL;
	/* Diffs for unmerged entries are empty when passing the new
	 * path, so leave it empty. */
	const char *newpath = status && status->status != 'U' ? status->new.name : NULL;
	const char *info;
	enum open_flags split;
	struct view *stage = VIEW(REQ_VIEW_STAGE);

	if (line->type == LINE_STAT_NONE ||
	    (!status && line[1].type == LINE_STAT_NONE)) {
		report("No file to diff");
		return REQ_NONE;
	}

	switch (line->type) {
	case LINE_STAT_STAGED:
		if (is_initial_commit()) {
			const char *no_head_diff_argv[] = {
				"git", "diff", "--no-color", "--patch-with-stat",
					"--", "/dev/null", newpath, NULL
			};

			if (!prepare_update(stage, no_head_diff_argv, opt_cdup, FORMAT_DASH))
				return REQ_QUIT;
		} else {
			const char *index_show_argv[] = {
				"git", "diff-index", "--root", "--patch-with-stat",
					"-C", "-M", "--cached", "HEAD", "--",
					oldpath, newpath, NULL
			};

			if (!prepare_update(stage, index_show_argv, opt_cdup, FORMAT_DASH))
				return REQ_QUIT;
		}

		if (status)
			info = "Staged changes to %s";
		else
			info = "Staged changes";
		break;

	case LINE_STAT_UNSTAGED:
	{
		const char *files_show_argv[] = {
			"git", "diff-files", "--root", "--patch-with-stat",
				"-C", "-M", "--", oldpath, newpath, NULL
		};

		if (!prepare_update(stage, files_show_argv, opt_cdup, FORMAT_DASH))
			return REQ_QUIT;
		if (status)
			info = "Unstaged changes to %s";
		else
			info = "Unstaged changes";
		break;
	}
	case LINE_STAT_UNTRACKED:
		if (!newpath) {
			report("No file to show");
			return REQ_NONE;
		}

	    	if (!suffixcmp(status->new.name, -1, "/")) {
			report("Cannot display a directory");
			return REQ_NONE;
		}

		if (!prepare_update_file(stage, newpath))
			return REQ_QUIT;
		info = "Untracked file %s";
		break;

	case LINE_STAT_HEAD:
		return REQ_NONE;

	default:
		die("line type %d not handled in switch", line->type);
	}

	split = view_is_displayed(view) ? OPEN_SPLIT : 0;
	open_view(view, REQ_VIEW_STAGE, OPEN_PREPARED | split);
	if (view_is_displayed(VIEW(REQ_VIEW_STAGE))) {
		if (status) {
			stage_status = *status;
		} else {
			memset(&stage_status, 0, sizeof(stage_status));
		}

		stage_line_type = line->type;
		stage_chunks = 0;
		string_format(VIEW(REQ_VIEW_STAGE)->ref, info, stage_status.new.name);
	}

	return REQ_NONE;
}

static bool
status_exists(struct status *status, enum line_type type)
{
	struct view *view = VIEW(REQ_VIEW_STATUS);
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
		return run_io(io, staged_argv, opt_cdup, IO_WR);

	case LINE_STAT_UNSTAGED:
		return run_io(io, others_argv, opt_cdup, IO_WR);

	case LINE_STAT_UNTRACKED:
		return run_io(io, others_argv, NULL, IO_WR);

	default:
		die("line type %d not handled in switch", type);
		return FALSE;
	}
}

static bool
status_update_write(struct io *io, struct status *status, enum line_type type)
{
	char buf[SIZEOF_STR];
	size_t bufsize = 0;

	switch (type) {
	case LINE_STAT_STAGED:
		if (!string_format_from(buf, &bufsize, "%06o %s\t%s%c",
					status->old.mode,
					status->old.rev,
					status->old.name, 0))
			return FALSE;
		break;

	case LINE_STAT_UNSTAGED:
	case LINE_STAT_UNTRACKED:
		if (!string_format_from(buf, &bufsize, "%s%c", status->new.name, 0))
			return FALSE;
		break;

	default:
		die("line type %d not handled in switch", type);
	}

	return io_write(io, buf, bufsize);
}

static bool
status_update_file(struct status *status, enum line_type type)
{
	struct io io = {};
	bool result;

	if (!status_update_prepare(&io, type))
		return FALSE;

	result = status_update_write(&io, status, type);
	done_io(&io);
	return result;
}

static bool
status_update_files(struct view *view, struct line *line)
{
	struct io io = {};
	bool result = TRUE;
	struct line *pos = view->line + view->lines;
	int files = 0;
	int file, done;

	if (!status_update_prepare(&io, line->type))
		return FALSE;

	for (pos = line; pos < view->line + view->lines && pos->data; pos++)
		files++;

	for (file = 0, done = 0; result && file < files; line++, file++) {
		int almost_done = file * 100 / files;

		if (almost_done > done) {
			done = almost_done;
			string_format(view->ref, "updating file %u of %u (%d%% done)",
				      file, files, done);
			update_view_title(view);
		}
		result = status_update_write(&io, line->data, line->type);
	}

	done_io(&io);
	return result;
}

static bool
status_update(struct view *view)
{
	struct line *line = &view->line[view->lineno];

	assert(view->lines);

	if (!line->data) {
		/* This should work even for the "On branch" line. */
		if (line < view->line + view->lines && !line[1].data) {
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
		return FALSE;

	} else {
		char mode[10] = "100644";
		const char *reset_argv[] = {
			"git", "update-index", "--cacheinfo", mode,
				status->old.rev, status->old.name, NULL
		};
		const char *checkout_argv[] = {
			"git", "checkout", "--", status->old.name, NULL
		};

		if (!prompt_yesno("Are you sure you want to overwrite any changes?"))
			return FALSE;
		string_format(mode, "%o", status->old.mode);
		return (status->status != 'U' || run_io_fg(reset_argv, opt_cdup)) &&
			run_io_fg(checkout_argv, opt_cdup);
	}
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

		open_editor(status->status != '?', status->new.name);
		break;

	case REQ_VIEW_BLAME:
		if (status) {
			string_copy(opt_file, status->new.name);
			opt_ref[0] = 0;
		}
		return request;

	case REQ_ENTER:
		/* After returning the status view has been split to
		 * show the stage view. No further reloading is
		 * necessary. */
		status_enter(view, line);
		return REQ_NONE;

	case REQ_REFRESH:
		/* Simply reload the view. */
		break;

	default:
		return request;
	}

	open_view(view, REQ_VIEW_STATUS, OPEN_RELOAD);

	return REQ_NONE;
}

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
		key = get_key(REQ_STATUS_MERGE);

	} else {
		key = get_key(REQ_STATUS_UPDATE);
	}

	string_format(view->ref, text, key, file);
}

static bool
status_grep(struct view *view, struct line *line)
{
	struct status *status = line->data;
	enum { S_STATUS, S_NAME, S_END } state;
	char buf[2] = "?";
	regmatch_t pmatch;

	if (!status)
		return FALSE;

	for (state = S_STATUS; state < S_END; state++) {
		const char *text;

		switch (state) {
		case S_NAME:	text = status->new.name;	break;
		case S_STATUS:
			buf[0] = status->status;
			text = buf;
			break;

		default:
			return FALSE;
		}

		if (regexec(view->regex, text, 1, &pmatch, 0) != REG_NOMATCH)
			return TRUE;
	}

	return FALSE;
}

static struct view_ops status_ops = {
	"file",
	NULL,
	status_open,
	NULL,
	status_draw,
	status_request,
	status_grep,
	status_select,
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

static struct line *
stage_diff_find(struct view *view, struct line *line, enum line_type type)
{
	for (; view->line < line; line--)
		if (line->type == type)
			return line;

	return NULL;
}

static bool
stage_apply_chunk(struct view *view, struct line *chunk, bool revert)
{
	const char *apply_argv[SIZEOF_ARG] = {
		"git", "apply", "--whitespace=nowarn", NULL
	};
	struct line *diff_hdr;
	struct io io = {};
	int argc = 3;

	diff_hdr = stage_diff_find(view, chunk, LINE_DIFF_HEADER);
	if (!diff_hdr)
		return FALSE;

	if (!revert)
		apply_argv[argc++] = "--cached";
	if (revert || stage_line_type == LINE_STAT_STAGED)
		apply_argv[argc++] = "-R";
	apply_argv[argc++] = "-";
	apply_argv[argc++] = NULL;
	if (!run_io(&io, apply_argv, opt_cdup, IO_WR))
		return FALSE;

	if (!stage_diff_write(&io, diff_hdr, chunk) ||
	    !stage_diff_write(&io, chunk, view->line + view->lines))
		chunk = NULL;

	done_io(&io);
	run_io_bg(update_index_argv);

	return chunk ? TRUE : FALSE;
}

static bool
stage_update(struct view *view, struct line *line)
{
	struct line *chunk = NULL;

	if (!is_initial_commit() && stage_line_type != LINE_STAT_UNTRACKED)
		chunk = stage_diff_find(view, line, LINE_DIFF_CHUNK);

	if (chunk) {
		if (!stage_apply_chunk(view, chunk, FALSE)) {
			report("Failed to apply chunk");
			return FALSE;
		}

	} else if (!stage_status.status) {
		view = VIEW(REQ_VIEW_STATUS);

		for (line = view->line; line < view->line + view->lines; line++)
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
		chunk = stage_diff_find(view, line, LINE_DIFF_CHUNK);

	if (chunk) {
		if (!prompt_yesno("Are you sure you want to revert changes?"))
			return FALSE;

		if (!stage_apply_chunk(view, chunk, TRUE)) {
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
	int i;

	if (!stage_chunks) {
		static size_t alloc = 0;
		int *tmp;

		for (line = view->line; line < view->line + view->lines; line++) {
			if (line->type != LINE_DIFF_CHUNK)
				continue;

			tmp = realloc_items(stage_chunk, &alloc,
					    stage_chunks, sizeof(*tmp));
			if (!tmp) {
				report("Allocation failure");
				return;
			}

			stage_chunk = tmp;
			stage_chunk[stage_chunks++] = line - view->line;
		}
	}

	for (i = 0; i < stage_chunks; i++) {
		if (stage_chunk[i] > view->lineno) {
			do_scroll_view(view, stage_chunk[i] - view->lineno);
			report("Chunk %d of %d", i + 1, stage_chunks);
			return;
		}
	}

	report("No next chunk found");
}

static enum request
stage_request(struct view *view, enum request request, struct line *line)
{
	switch (request) {
	case REQ_STATUS_UPDATE:
		if (!stage_update(view, line))
			return REQ_NONE;
		break;

	case REQ_STATUS_REVERT:
		if (!stage_revert(view, line))
			return REQ_NONE;
		break;

	case REQ_STAGE_NEXT:
		if (stage_line_type == LINE_STAT_UNTRACKED) {
			report("File is untracked; press %s to add",
			       get_key(REQ_STATUS_UPDATE));
			return REQ_NONE;
		}
		stage_next(view, line);
		return REQ_NONE;

	case REQ_EDIT:
		if (!stage_status.new.name[0])
			return request;
		if (stage_status.status == 'D') {
			report("File has been deleted.");
			return REQ_NONE;
		}

		open_editor(stage_status.status != '?', stage_status.new.name);
		break;

	case REQ_REFRESH:
		/* Reload everything ... */
		break;

	case REQ_VIEW_BLAME:
		if (stage_status.new.name[0]) {
			string_copy(opt_file, stage_status.new.name);
			opt_ref[0] = 0;
		}
		return request;

	case REQ_ENTER:
		return pager_request(view, request, line);

	default:
		return request;
	}

	VIEW(REQ_VIEW_STATUS)->p_restore = TRUE;
	open_view(view, REQ_VIEW_STATUS, OPEN_RELOAD | OPEN_NOMAXIMIZE);

	/* Check whether the staged entry still exists, and close the
	 * stage view if it doesn't. */
	if (!status_exists(&stage_status, stage_line_type)) {
		status_restore(VIEW(REQ_VIEW_STATUS));
		return REQ_VIEW_CLOSE;
	}

	if (stage_line_type == LINE_STAT_UNTRACKED) {
	    	if (!suffixcmp(stage_status.new.name, -1, "/")) {
			report("Cannot display a directory");
			return REQ_NONE;
		}

		if (!prepare_update_file(view, stage_status.new.name)) {
			report("Failed to open file: %s", strerror(errno));
			return REQ_NONE;
		}
	}
	open_view(view, REQ_VIEW_STAGE, OPEN_REFRESH);

	return REQ_NONE;
}

static struct view_ops stage_ops = {
	"line",
	NULL,
	NULL,
	pager_read,
	pager_draw,
	stage_request,
	pager_grep,
	pager_select,
};


/*
 * Revision graph
 */

struct commit {
	char id[SIZEOF_REV];		/* SHA1 ID. */
	char title[128];		/* First line of the commit message. */
	char author[75];		/* Author of the commit. */
	struct tm time;			/* Date from the author ident. */
	struct ref **refs;		/* Repository references. */
	chtype graph[SIZEOF_REVGRAPH];	/* Ancestry chain graphics. */
	size_t graph_size;		/* The width of the graph array. */
	bool has_parents;		/* Rewritten --parents seen. */
};

/* Size of rev graph with no  "padding" columns */
#define SIZEOF_REVITEMS	(SIZEOF_REVGRAPH - (SIZEOF_REVGRAPH / 2))

struct rev_graph {
	struct rev_graph *prev, *next, *parents;
	char rev[SIZEOF_REVITEMS][SIZEOF_REV];
	size_t size;
	struct commit *commit;
	size_t pos;
	unsigned int boundary:1;
};

/* Parents of the commit being visualized. */
static struct rev_graph graph_parents[4];

/* The current stack of revisions on the graph. */
static struct rev_graph graph_stacks[4] = {
	{ &graph_stacks[3], &graph_stacks[1], &graph_parents[0] },
	{ &graph_stacks[0], &graph_stacks[2], &graph_parents[1] },
	{ &graph_stacks[1], &graph_stacks[3], &graph_parents[2] },
	{ &graph_stacks[2], &graph_stacks[0], &graph_parents[3] },
};

static inline bool
graph_parent_is_merge(struct rev_graph *graph)
{
	return graph->parents->size > 1;
}

static inline void
append_to_rev_graph(struct rev_graph *graph, chtype symbol)
{
	struct commit *commit = graph->commit;

	if (commit->graph_size < ARRAY_SIZE(commit->graph) - 1)
		commit->graph[commit->graph_size++] = symbol;
}

static void
clear_rev_graph(struct rev_graph *graph)
{
	graph->boundary = 0;
	graph->size = graph->pos = 0;
	graph->commit = NULL;
	memset(graph->parents, 0, sizeof(*graph->parents));
}

static void
done_rev_graph(struct rev_graph *graph)
{
	if (graph_parent_is_merge(graph) &&
	    graph->pos < graph->size - 1 &&
	    graph->next->size == graph->size + graph->parents->size - 1) {
		size_t i = graph->pos + graph->parents->size - 1;

		graph->commit->graph_size = i * 2;
		while (i < graph->next->size - 1) {
			append_to_rev_graph(graph, ' ');
			append_to_rev_graph(graph, '\\');
			i++;
		}
	}

	clear_rev_graph(graph);
}

static void
push_rev_graph(struct rev_graph *graph, const char *parent)
{
	int i;

	/* "Collapse" duplicate parents lines.
	 *
	 * FIXME: This needs to also update update the drawn graph but
	 * for now it just serves as a method for pruning graph lines. */
	for (i = 0; i < graph->size; i++)
		if (!strncmp(graph->rev[i], parent, SIZEOF_REV))
			return;

	if (graph->size < SIZEOF_REVITEMS) {
		string_copy_rev(graph->rev[graph->size++], parent);
	}
}

static chtype
get_rev_graph_symbol(struct rev_graph *graph)
{
	chtype symbol;

	if (graph->boundary)
		symbol = REVGRAPH_BOUND;
	else if (graph->parents->size == 0)
		symbol = REVGRAPH_INIT;
	else if (graph_parent_is_merge(graph))
		symbol = REVGRAPH_MERGE;
	else if (graph->pos >= graph->size)
		symbol = REVGRAPH_BRANCH;
	else
		symbol = REVGRAPH_COMMIT;

	return symbol;
}

static void
draw_rev_graph(struct rev_graph *graph)
{
	struct rev_filler {
		chtype separator, line;
	};
	enum { DEFAULT, RSHARP, RDIAG, LDIAG };
	static struct rev_filler fillers[] = {
		{ ' ',	'|' },
		{ '`',	'.' },
		{ '\'',	' ' },
		{ '/',	' ' },
	};
	chtype symbol = get_rev_graph_symbol(graph);
	struct rev_filler *filler;
	size_t i;

	if (opt_line_graphics)
		fillers[DEFAULT].line = line_graphics[LINE_GRAPHIC_VLINE];

	filler = &fillers[DEFAULT];

	for (i = 0; i < graph->pos; i++) {
		append_to_rev_graph(graph, filler->line);
		if (graph_parent_is_merge(graph->prev) &&
		    graph->prev->pos == i)
			filler = &fillers[RSHARP];

		append_to_rev_graph(graph, filler->separator);
	}

	/* Place the symbol for this revision. */
	append_to_rev_graph(graph, symbol);

	if (graph->prev->size > graph->size)
		filler = &fillers[RDIAG];
	else
		filler = &fillers[DEFAULT];

	i++;

	for (; i < graph->size; i++) {
		append_to_rev_graph(graph, filler->separator);
		append_to_rev_graph(graph, filler->line);
		if (graph_parent_is_merge(graph->prev) &&
		    i < graph->prev->pos + graph->parents->size)
			filler = &fillers[RSHARP];
		if (graph->prev->size > graph->size)
			filler = &fillers[LDIAG];
	}

	if (graph->prev->size > graph->size) {
		append_to_rev_graph(graph, filler->separator);
		if (filler->line != ' ')
			append_to_rev_graph(graph, filler->line);
	}
}

/* Prepare the next rev graph */
static void
prepare_rev_graph(struct rev_graph *graph)
{
	size_t i;

	/* First, traverse all lines of revisions up to the active one. */
	for (graph->pos = 0; graph->pos < graph->size; graph->pos++) {
		if (!strcmp(graph->rev[graph->pos], graph->commit->id))
			break;

		push_rev_graph(graph->next, graph->rev[graph->pos]);
	}

	/* Interleave the new revision parent(s). */
	for (i = 0; !graph->boundary && i < graph->parents->size; i++)
		push_rev_graph(graph->next, graph->parents->rev[i]);

	/* Lastly, put any remaining revisions. */
	for (i = graph->pos + 1; i < graph->size; i++)
		push_rev_graph(graph->next, graph->rev[i]);
}

static void
update_rev_graph(struct view *view, struct rev_graph *graph)
{
	/* If this is the finalizing update ... */
	if (graph->commit)
		prepare_rev_graph(graph);

	/* Graph visualization needs a one rev look-ahead,
	 * so the first update doesn't visualize anything. */
	if (!graph->prev->commit)
		return;

	if (view->lines > 2)
		view->line[view->lines - 3].dirty = 1;
	if (view->lines > 1)
		view->line[view->lines - 2].dirty = 1;
	draw_rev_graph(graph->prev);
	done_rev_graph(graph->prev->prev);
}


/*
 * Main view backend
 */

static const char *main_argv[SIZEOF_ARG] = {
	"git", "log", "--no-color", "--pretty=raw", "--parents",
		      "--topo-order", "%(head)", NULL
};

static bool
main_draw(struct view *view, struct line *line, unsigned int lineno)
{
	struct commit *commit = line->data;

	if (!*commit->author)
		return FALSE;

	if (opt_date && draw_date(view, &commit->time))
		return TRUE;

	if (opt_author && draw_author(view, commit->author))
		return TRUE;

	if (opt_rev_graph && commit->graph_size &&
	    draw_graphic(view, LINE_MAIN_REVGRAPH, commit->graph, commit->graph_size))
		return TRUE;

	if (opt_show_refs && commit->refs) {
		size_t i = 0;

		do {
			enum line_type type;

			if (commit->refs[i]->head)
				type = LINE_MAIN_HEAD;
			else if (commit->refs[i]->ltag)
				type = LINE_MAIN_LOCAL_TAG;
			else if (commit->refs[i]->tag)
				type = LINE_MAIN_TAG;
			else if (commit->refs[i]->tracked)
				type = LINE_MAIN_TRACKED;
			else if (commit->refs[i]->remote)
				type = LINE_MAIN_REMOTE;
			else
				type = LINE_MAIN_REF;

			if (draw_text(view, type, "[", TRUE) ||
			    draw_text(view, type, commit->refs[i]->name, TRUE) ||
			    draw_text(view, type, "]", TRUE))
				return TRUE;

			if (draw_text(view, LINE_DEFAULT, " ", TRUE))
				return TRUE;
		} while (commit->refs[i++]->next);
	}

	draw_text(view, LINE_DEFAULT, commit->title, TRUE);
	return TRUE;
}

/* Reads git log --pretty=raw output and parses it into the commit struct. */
static bool
main_read(struct view *view, char *line)
{
	static struct rev_graph *graph = graph_stacks;
	enum line_type type;
	struct commit *commit;

	if (!line) {
		int i;

		if (!view->lines && !view->parent)
			die("No revisions match the given arguments.");
		if (view->lines > 0) {
			commit = view->line[view->lines - 1].data;
			view->line[view->lines - 1].dirty = 1;
			if (!*commit->author) {
				view->lines--;
				free(commit);
				graph->commit = NULL;
			}
		}
		update_rev_graph(view, graph);

		for (i = 0; i < ARRAY_SIZE(graph_stacks); i++)
			clear_rev_graph(&graph_stacks[i]);
		return TRUE;
	}

	type = get_line_type(line);
	if (type == LINE_COMMIT) {
		commit = calloc(1, sizeof(struct commit));
		if (!commit)
			return FALSE;

		line += STRING_SIZE("commit ");
		if (*line == '-') {
			graph->boundary = 1;
			line++;
		}

		string_copy_rev(commit->id, line);
		commit->refs = get_refs(commit->id);
		graph->commit = commit;
		add_line_data(view, commit, LINE_MAIN_COMMIT);

		while ((line = strchr(line, ' '))) {
			line++;
			push_rev_graph(graph->parents, line);
			commit->has_parents = TRUE;
		}
		return TRUE;
	}

	if (!view->lines)
		return TRUE;
	commit = view->line[view->lines - 1].data;

	switch (type) {
	case LINE_PARENT:
		if (commit->has_parents)
			break;
		push_rev_graph(graph->parents, line + STRING_SIZE("parent "));
		break;

	case LINE_AUTHOR:
		parse_author_line(line + STRING_SIZE("author "),
				  commit->author, sizeof(commit->author),
				  &commit->time);
		update_rev_graph(view, graph);
		graph = graph->next;
		break;

	default:
		/* Fill in the commit title if it has not already been set. */
		if (commit->title[0])
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
		/* FIXME: More graceful handling of titles; append "..." to
		 * shortened titles, etc. */

		string_expand(commit->title, sizeof(commit->title), line, 1);
		view->line[view->lines - 1].dirty = 1;
	}

	return TRUE;
}

static enum request
main_request(struct view *view, enum request request, struct line *line)
{
	enum open_flags flags = display[0] == view ? OPEN_SPLIT : OPEN_DEFAULT;

	switch (request) {
	case REQ_ENTER:
		open_view(view, REQ_VIEW_DIFF, flags);
		break;
	case REQ_REFRESH:
		load_refs();
		open_view(view, REQ_VIEW_MAIN, OPEN_REFRESH);
		break;
	default:
		return request;
	}

	return REQ_NONE;
}

static bool
grep_refs(struct ref **refs, regex_t *regex)
{
	regmatch_t pmatch;
	size_t i = 0;

	if (!refs)
		return FALSE;
	do {
		if (regexec(regex, refs[i]->name, 1, &pmatch, 0) != REG_NOMATCH)
			return TRUE;
	} while (refs[i++]->next);

	return FALSE;
}

static bool
main_grep(struct view *view, struct line *line)
{
	struct commit *commit = line->data;
	enum { S_TITLE, S_AUTHOR, S_DATE, S_REFS, S_END } state;
	char buf[DATE_COLS + 1];
	regmatch_t pmatch;

	for (state = S_TITLE; state < S_END; state++) {
		char *text;

		switch (state) {
		case S_TITLE:	text = commit->title;	break;
		case S_AUTHOR:
			if (!opt_author)
				continue;
			text = commit->author;
			break;
		case S_DATE:
			if (!opt_date)
				continue;
			if (!strftime(buf, sizeof(buf), DATE_FORMAT, &commit->time))
				continue;
			text = buf;
			break;
		case S_REFS:
			if (!opt_show_refs)
				continue;
			if (grep_refs(commit->refs, view->regex) == TRUE)
				return TRUE;
			continue;
		default:
			return FALSE;
		}

		if (regexec(view->regex, text, 1, &pmatch, 0) != REG_NOMATCH)
			return TRUE;
	}

	return FALSE;
}

static void
main_select(struct view *view, struct line *line)
{
	struct commit *commit = line->data;

	string_copy_rev(view->ref, commit->id);
	string_copy_rev(ref_commit, view->ref);
}

static struct view_ops main_ops = {
	"commit",
	main_argv,
	NULL,
	main_read,
	main_draw,
	main_request,
	main_grep,
	main_select,
};


/*
 * Unicode / UTF-8 handling
 *
 * NOTE: Much of the following code for dealing with unicode is derived from
 * ELinks' UTF-8 code developed by Scrool <scroolik@gmail.com>. Origin file is
 * src/intl/charset.c from the utf8 branch commit elinks-0.11.0-g31f2c28.
 */

/* I've (over)annotated a lot of code snippets because I am not entirely
 * confident that the approach taken by this small UTF-8 interface is correct.
 * --jonas */

static inline int
unicode_width(unsigned long c)
{
	if (c >= 0x1100 &&
	   (c <= 0x115f				/* Hangul Jamo */
	    || c == 0x2329
	    || c == 0x232a
	    || (c >= 0x2e80  && c <= 0xa4cf && c != 0x303f)
						/* CJK ... Yi */
	    || (c >= 0xac00  && c <= 0xd7a3)	/* Hangul Syllables */
	    || (c >= 0xf900  && c <= 0xfaff)	/* CJK Compatibility Ideographs */
	    || (c >= 0xfe30  && c <= 0xfe6f)	/* CJK Compatibility Forms */
	    || (c >= 0xff00  && c <= 0xff60)	/* Fullwidth Forms */
	    || (c >= 0xffe0  && c <= 0xffe6)
	    || (c >= 0x20000 && c <= 0x2fffd)
	    || (c >= 0x30000 && c <= 0x3fffd)))
		return 2;

	if (c == '\t')
		return opt_tab_size;

	return 1;
}

/* Number of bytes used for encoding a UTF-8 character indexed by first byte.
 * Illegal bytes are set one. */
static const unsigned char utf8_bytes[256] = {
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
	2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
	3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 4,4,4,4,4,4,4,4, 5,5,5,5,6,6,1,1,
};

/* Decode UTF-8 multi-byte representation into a unicode character. */
static inline unsigned long
utf8_to_unicode(const char *string, size_t length)
{
	unsigned long unicode;

	switch (length) {
	case 1:
		unicode  =   string[0];
		break;
	case 2:
		unicode  =  (string[0] & 0x1f) << 6;
		unicode +=  (string[1] & 0x3f);
		break;
	case 3:
		unicode  =  (string[0] & 0x0f) << 12;
		unicode += ((string[1] & 0x3f) << 6);
		unicode +=  (string[2] & 0x3f);
		break;
	case 4:
		unicode  =  (string[0] & 0x0f) << 18;
		unicode += ((string[1] & 0x3f) << 12);
		unicode += ((string[2] & 0x3f) << 6);
		unicode +=  (string[3] & 0x3f);
		break;
	case 5:
		unicode  =  (string[0] & 0x0f) << 24;
		unicode += ((string[1] & 0x3f) << 18);
		unicode += ((string[2] & 0x3f) << 12);
		unicode += ((string[3] & 0x3f) << 6);
		unicode +=  (string[4] & 0x3f);
		break;
	case 6:
		unicode  =  (string[0] & 0x01) << 30;
		unicode += ((string[1] & 0x3f) << 24);
		unicode += ((string[2] & 0x3f) << 18);
		unicode += ((string[3] & 0x3f) << 12);
		unicode += ((string[4] & 0x3f) << 6);
		unicode +=  (string[5] & 0x3f);
		break;
	default:
		die("Invalid unicode length");
	}

	/* Invalid characters could return the special 0xfffd value but NUL
	 * should be just as good. */
	return unicode > 0xffff ? 0 : unicode;
}

/* Calculates how much of string can be shown within the given maximum width
 * and sets trimmed parameter to non-zero value if all of string could not be
 * shown. If the reserve flag is TRUE, it will reserve at least one
 * trailing character, which can be useful when drawing a delimiter.
 *
 * Returns the number of bytes to output from string to satisfy max_width. */
static size_t
utf8_length(const char **start, size_t skip, int *width, size_t max_width, int *trimmed, bool reserve)
{
	const char *string = *start;
	const char *end = strchr(string, '\0');
	unsigned char last_bytes = 0;
	size_t last_ucwidth = 0;

	*width = 0;
	*trimmed = 0;

	while (string < end) {
		int c = *(unsigned char *) string;
		unsigned char bytes = utf8_bytes[c];
		size_t ucwidth;
		unsigned long unicode;

		if (string + bytes > end)
			break;

		/* Change representation to figure out whether
		 * it is a single- or double-width character. */

		unicode = utf8_to_unicode(string, bytes);
		/* FIXME: Graceful handling of invalid unicode character. */
		if (!unicode)
			break;

		ucwidth = unicode_width(unicode);
		if (skip > 0) {
			skip -= ucwidth <= skip ? ucwidth : skip;
			*start += bytes;
		}
		*width  += ucwidth;
		if (*width > max_width) {
			*trimmed = 1;
			*width -= ucwidth;
			if (reserve && *width == max_width) {
				string -= last_bytes;
				*width -= last_ucwidth;
			}
			break;
		}

		string  += bytes;
		last_bytes = ucwidth ? bytes : 0;
		last_ucwidth = ucwidth;
	}

	return string - *start;
}


/*
 * Status management
 */

/* Whether or not the curses interface has been initialized. */
static bool cursed = FALSE;

/* Terminal hacks and workarounds. */
static bool use_scroll_redrawwin;
static bool use_scroll_status_wclear;

/* The status window is used for polling keystrokes. */
static WINDOW *status_win;

/* Reading from the prompt? */
static bool input_mode = FALSE;

static bool status_empty = FALSE;

/* Update status and title window. */
static void
report(const char *msg, ...)
{
	struct view *view = display[current_view];

	if (input_mode)
		return;

	if (!view) {
		char buf[SIZEOF_STR];
		va_list args;

		va_start(args, msg);
		if (vsnprintf(buf, sizeof(buf), msg, args) >= sizeof(buf)) {
			buf[sizeof(buf) - 1] = 0;
			buf[sizeof(buf) - 2] = '.';
			buf[sizeof(buf) - 3] = '.';
			buf[sizeof(buf) - 4] = '.';
		}
		va_end(args);
		die("%s", buf);
	}

	if (!status_empty || *msg) {
		va_list args;

		va_start(args, msg);

		wmove(status_win, 0, 0);
		if (view->has_scrolled && use_scroll_status_wclear)
			wclear(status_win);
		if (*msg) {
			vwprintw(status_win, msg, args);
			status_empty = FALSE;
		} else {
			status_empty = TRUE;
		}
		wclrtoeol(status_win);
		wnoutrefresh(status_win);

		va_end(args);
	}

	update_view_title(view);
}

/* Controls when nodelay should be in effect when polling user input. */
static void
set_nonblocking_input(bool loading)
{
	static unsigned int loading_views;

	if ((loading == FALSE && loading_views-- == 1) ||
	    (loading == TRUE  && loading_views++ == 0))
		nodelay(status_win, loading);
}

static void
init_display(void)
{
	const char *term;
	int x, y;

	/* Initialize the curses library */
	if (isatty(STDIN_FILENO)) {
		cursed = !!initscr();
		opt_tty = stdin;
	} else {
		/* Leave stdin and stdout alone when acting as a pager. */
		opt_tty = fopen("/dev/tty", "r+");
		if (!opt_tty)
			die("Failed to open /dev/tty");
		cursed = !!newterm(NULL, opt_tty, opt_tty);
	}

	if (!cursed)
		die("Failed to initialize curses");

	nonl();         /* Tell curses not to do NL->CR/NL on output */
	cbreak();       /* Take input chars one at a time, no wait for \n */
	noecho();       /* Don't echo input */
	leaveok(stdscr, FALSE);

	if (has_colors())
		init_colors();

	getmaxyx(stdscr, y, x);
	status_win = newwin(1, 0, y - 1, 0);
	if (!status_win)
		die("Failed to create status window");

	/* Enable keyboard mapping */
	keypad(status_win, TRUE);
	wbkgdset(status_win, get_line_attr(LINE_STATUS));

	TABSIZE = opt_tab_size;
	if (opt_line_graphics) {
		line_graphics[LINE_GRAPHIC_VLINE] = ACS_VLINE;
	}

	term = getenv("XTERM_VERSION") ? NULL : getenv("COLORTERM");
	if (term && !strcmp(term, "gnome-terminal")) {
		/* In the gnome-terminal-emulator, the message from
		 * scrolling up one line when impossible followed by
		 * scrolling down one line causes corruption of the
		 * status line. This is fixed by calling wclear. */
		use_scroll_status_wclear = TRUE;
		use_scroll_redrawwin = FALSE;

	} else if (term && !strcmp(term, "xrvt-xpm")) {
		/* No problems with full optimizations in xrvt-(unicode)
		 * and aterm. */
		use_scroll_status_wclear = use_scroll_redrawwin = FALSE;

	} else {
		/* When scrolling in (u)xterm the last line in the
		 * scrolling direction will update slowly. */
		use_scroll_redrawwin = TRUE;
		use_scroll_status_wclear = FALSE;
	}
}

static int
get_input(int prompt_position)
{
	struct view *view;
	int i, key, cursor_y, cursor_x;

	if (prompt_position)
		input_mode = TRUE;

	while (TRUE) {
		foreach_view (view, i) {
			update_view(view);
			if (view_is_displayed(view) && view->has_scrolled &&
			    use_scroll_redrawwin)
				redrawwin(view->win);
			view->has_scrolled = FALSE;
		}

		/* Update the cursor position. */
		if (prompt_position) {
			getbegyx(status_win, cursor_y, cursor_x);
			cursor_x = prompt_position;
		} else {
			view = display[current_view];
			getbegyx(view->win, cursor_y, cursor_x);
			cursor_x = view->width - 1;
			cursor_y += view->lineno - view->offset;
		}
		setsyx(cursor_y, cursor_x);

		/* Refresh, accept single keystroke of input */
		doupdate();
		key = wgetch(status_win);

		/* wgetch() with nodelay() enabled returns ERR when
		 * there's no input. */
		if (key == ERR) {

		} else if (key == KEY_RESIZE) {
			int height, width;

			getmaxyx(stdscr, height, width);

			wresize(status_win, 1, width);
			mvwin(status_win, height - 1, 0);
			wnoutrefresh(status_win);
			resize_display();
			redraw_display(TRUE);

		} else {
			input_mode = FALSE;
			return key;
		}
	}
}

static char *
prompt_input(const char *prompt, input_handler handler, void *data)
{
	enum input_status status = INPUT_OK;
	static char buf[SIZEOF_STR];
	size_t pos = 0;

	buf[pos] = 0;

	while (status == INPUT_OK || status == INPUT_SKIP) {
		int key;

		mvwprintw(status_win, 0, 0, "%s%.*s", prompt, pos, buf);
		wclrtoeol(status_win);

		key = get_input(pos + 1);
		switch (key) {
		case KEY_RETURN:
		case KEY_ENTER:
		case '\n':
			status = pos ? INPUT_STOP : INPUT_CANCEL;
			break;

		case KEY_BACKSPACE:
			if (pos > 0)
				buf[--pos] = 0;
			else
				status = INPUT_CANCEL;
			break;

		case KEY_ESC:
			status = INPUT_CANCEL;
			break;

		default:
			if (pos >= sizeof(buf)) {
				report("Input string too long");
				return NULL;
			}

			status = handler(data, buf, key);
			if (status == INPUT_OK)
				buf[pos++] = (char) key;
		}
	}

	/* Clear the status window */
	status_empty = FALSE;
	report("");

	if (status == INPUT_CANCEL)
		return NULL;

	buf[pos++] = 0;

	return buf;
}

static enum input_status
prompt_yesno_handler(void *data, char *buf, int c)
{
	if (c == 'y' || c == 'Y')
		return INPUT_STOP;
	if (c == 'n' || c == 'N')
		return INPUT_CANCEL;
	return INPUT_SKIP;
}

static bool
prompt_yesno(const char *prompt)
{
	char prompt2[SIZEOF_STR];

	if (!string_format(prompt2, "%s [Yy/Nn]", prompt))
		return FALSE;

	return !!prompt_input(prompt2, prompt_yesno_handler, NULL);
}

static enum input_status
read_prompt_handler(void *data, char *buf, int c)
{
	return isprint(c) ? INPUT_OK : INPUT_SKIP;
}

static char *
read_prompt(const char *prompt)
{
	return prompt_input(prompt, read_prompt_handler, NULL);
}

/*
 * Repository properties
 */

static struct ref *refs = NULL;
static size_t refs_alloc = 0;
static size_t refs_size = 0;

/* Id <-> ref store */
static struct ref ***id_refs = NULL;
static size_t id_refs_alloc = 0;
static size_t id_refs_size = 0;

static int
compare_refs(const void *ref1_, const void *ref2_)
{
	const struct ref *ref1 = *(const struct ref **)ref1_;
	const struct ref *ref2 = *(const struct ref **)ref2_;

	if (ref1->tag != ref2->tag)
		return ref2->tag - ref1->tag;
	if (ref1->ltag != ref2->ltag)
		return ref2->ltag - ref2->ltag;
	if (ref1->head != ref2->head)
		return ref2->head - ref1->head;
	if (ref1->tracked != ref2->tracked)
		return ref2->tracked - ref1->tracked;
	if (ref1->remote != ref2->remote)
		return ref2->remote - ref1->remote;
	return strcmp(ref1->name, ref2->name);
}

static struct ref **
get_refs(const char *id)
{
	struct ref ***tmp_id_refs;
	struct ref **ref_list = NULL;
	size_t ref_list_alloc = 0;
	size_t ref_list_size = 0;
	size_t i;

	for (i = 0; i < id_refs_size; i++)
		if (!strcmp(id, id_refs[i][0]->id))
			return id_refs[i];

	tmp_id_refs = realloc_items(id_refs, &id_refs_alloc, id_refs_size + 1,
				    sizeof(*id_refs));
	if (!tmp_id_refs)
		return NULL;

	id_refs = tmp_id_refs;

	for (i = 0; i < refs_size; i++) {
		struct ref **tmp;

		if (strcmp(id, refs[i].id))
			continue;

		tmp = realloc_items(ref_list, &ref_list_alloc,
				    ref_list_size + 1, sizeof(*ref_list));
		if (!tmp) {
			if (ref_list)
				free(ref_list);
			return NULL;
		}

		ref_list = tmp;
		ref_list[ref_list_size] = &refs[i];
		/* XXX: The properties of the commit chains ensures that we can
		 * safely modify the shared ref. The repo references will
		 * always be similar for the same id. */
		ref_list[ref_list_size]->next = 1;

		ref_list_size++;
	}

	if (ref_list) {
		qsort(ref_list, ref_list_size, sizeof(*ref_list), compare_refs);
		ref_list[ref_list_size - 1]->next = 0;
		id_refs[id_refs_size++] = ref_list;
	}

	return ref_list;
}

static int
read_ref(char *id, size_t idlen, char *name, size_t namelen)
{
	struct ref *ref;
	bool tag = FALSE;
	bool ltag = FALSE;
	bool remote = FALSE;
	bool tracked = FALSE;
	bool check_replace = FALSE;
	bool head = FALSE;

	if (!prefixcmp(name, "refs/tags/")) {
		if (!suffixcmp(name, namelen, "^{}")) {
			namelen -= 3;
			name[namelen] = 0;
			if (refs_size > 0 && refs[refs_size - 1].ltag == TRUE)
				check_replace = TRUE;
		} else {
			ltag = TRUE;
		}

		tag = TRUE;
		namelen -= STRING_SIZE("refs/tags/");
		name	+= STRING_SIZE("refs/tags/");

	} else if (!prefixcmp(name, "refs/remotes/")) {
		remote = TRUE;
		namelen -= STRING_SIZE("refs/remotes/");
		name	+= STRING_SIZE("refs/remotes/");
		tracked  = !strcmp(opt_remote, name);

	} else if (!prefixcmp(name, "refs/heads/")) {
		namelen -= STRING_SIZE("refs/heads/");
		name	+= STRING_SIZE("refs/heads/");
		head	 = !strncmp(opt_head, name, namelen);

	} else if (!strcmp(name, "HEAD")) {
		string_ncopy(opt_head_rev, id, idlen);
		return OK;
	}

	if (check_replace && !strcmp(name, refs[refs_size - 1].name)) {
		/* it's an annotated tag, replace the previous sha1 with the
		 * resolved commit id; relies on the fact git-ls-remote lists
		 * the commit id of an annotated tag right before the commit id
		 * it points to. */
		refs[refs_size - 1].ltag = ltag;
		string_copy_rev(refs[refs_size - 1].id, id);

		return OK;
	}
	refs = realloc_items(refs, &refs_alloc, refs_size + 1, sizeof(*refs));
	if (!refs)
		return ERR;

	ref = &refs[refs_size++];
	ref->name = malloc(namelen + 1);
	if (!ref->name)
		return ERR;

	strncpy(ref->name, name, namelen);
	ref->name[namelen] = 0;
	ref->head = head;
	ref->tag = tag;
	ref->ltag = ltag;
	ref->remote = remote;
	ref->tracked = tracked;
	string_copy_rev(ref->id, id);

	return OK;
}

static int
load_refs(void)
{
	static const char *ls_remote_argv[SIZEOF_ARG] = {
		"git", "ls-remote", ".", NULL
	};
	static bool init = FALSE;

	if (!init) {
		argv_from_env(ls_remote_argv, "TIG_LS_REMOTE");
		init = TRUE;
	}

	if (!*opt_git_dir)
		return OK;

	while (refs_size > 0)
		free(refs[--refs_size].name);
	while (id_refs_size > 0)
		free(id_refs[--id_refs_size]);

	return run_io_load(ls_remote_argv, "\t", read_ref);
}

static int
read_repo_config_option(char *name, size_t namelen, char *value, size_t valuelen)
{
	if (!strcmp(name, "i18n.commitencoding"))
		string_ncopy(opt_encoding, value, valuelen);

	if (!strcmp(name, "core.editor"))
		string_ncopy(opt_editor, value, valuelen);

	/* branch.<head>.remote */
	if (*opt_head &&
	    !strncmp(name, "branch.", 7) &&
	    !strncmp(name + 7, opt_head, strlen(opt_head)) &&
	    !strcmp(name + 7 + strlen(opt_head), ".remote"))
		string_ncopy(opt_remote, value, valuelen);

	if (*opt_head && *opt_remote &&
	    !strncmp(name, "branch.", 7) &&
	    !strncmp(name + 7, opt_head, strlen(opt_head)) &&
	    !strcmp(name + 7 + strlen(opt_head), ".merge")) {
		size_t from = strlen(opt_remote);

		if (!prefixcmp(value, "refs/heads/")) {
			value += STRING_SIZE("refs/heads/");
			valuelen -= STRING_SIZE("refs/heads/");
		}

		if (!string_format_from(opt_remote, &from, "/%s", value))
			opt_remote[0] = 0;
	}

	return OK;
}

static int
load_git_config(void)
{
	const char *config_list_argv[] = { "git", GIT_CONFIG, "--list", NULL };

	return run_io_load(config_list_argv, "=", read_repo_config_option);
}

static int
read_repo_info(char *name, size_t namelen, char *value, size_t valuelen)
{
	if (!opt_git_dir[0]) {
		string_ncopy(opt_git_dir, name, namelen);

	} else if (opt_is_inside_work_tree == -1) {
		/* This can be 3 different values depending on the
		 * version of git being used. If git-rev-parse does not
		 * understand --is-inside-work-tree it will simply echo
		 * the option else either "true" or "false" is printed.
		 * Default to true for the unknown case. */
		opt_is_inside_work_tree = strcmp(name, "false") ? TRUE : FALSE;

	} else if (*name == '.') {
		string_ncopy(opt_cdup, name, namelen);

	} else {
		string_ncopy(opt_prefix, name, namelen);
	}

	return OK;
}

static int
load_repo_info(void)
{
	const char *head_argv[] = {
		"git", "symbolic-ref", "HEAD", NULL
	};
	const char *rev_parse_argv[] = {
		"git", "rev-parse", "--git-dir", "--is-inside-work-tree",
			"--show-cdup", "--show-prefix", NULL
	};

	if (run_io_buf(head_argv, opt_head, sizeof(opt_head))) {
		chomp_string(opt_head);
		if (!prefixcmp(opt_head, "refs/heads/")) {
			char *offset = opt_head + STRING_SIZE("refs/heads/");

			memmove(opt_head, offset, strlen(offset) + 1);
		}
	}

	return run_io_load(rev_parse_argv, "=", read_repo_info);
}


/*
 * Main
 */

static const char usage[] =
"tig " TIG_VERSION " (" __DATE__ ")\n"
"\n"
"Usage: tig        [options] [revs] [--] [paths]\n"
"   or: tig show   [options] [revs] [--] [paths]\n"
"   or: tig blame  [rev] path\n"
"   or: tig status\n"
"   or: tig <      [git command output]\n"
"\n"
"Options:\n"
"  -v, --version   Show version and exit\n"
"  -h, --help      Show help message and exit";

static void __NORETURN
quit(int sig)
{
	/* XXX: Restore tty modes and let the OS cleanup the rest! */
	if (cursed)
		endwin();
	exit(0);
}

static void __NORETURN
die(const char *err, ...)
{
	va_list args;

	endwin();

	va_start(args, err);
	fputs("tig: ", stderr);
	vfprintf(stderr, err, args);
	fputs("\n", stderr);
	va_end(args);

	exit(1);
}

static void
warn(const char *msg, ...)
{
	va_list args;

	va_start(args, msg);
	fputs("tig warning: ", stderr);
	vfprintf(stderr, msg, args);
	fputs("\n", stderr);
	va_end(args);
}

static enum request
parse_options(int argc, const char *argv[])
{
	enum request request = REQ_VIEW_MAIN;
	const char *subcommand;
	bool seen_dashdash = FALSE;
	/* XXX: This is vulnerable to the user overriding options
	 * required for the main view parser. */
	const char *custom_argv[SIZEOF_ARG] = {
		"git", "log", "--no-color", "--pretty=raw", "--parents",
			"--topo-order", NULL
	};
	int i, j = 6;

	if (!isatty(STDIN_FILENO)) {
		io_open(&VIEW(REQ_VIEW_PAGER)->io, "");
		return REQ_VIEW_PAGER;
	}

	if (argc <= 1)
		return REQ_NONE;

	subcommand = argv[1];
	if (!strcmp(subcommand, "status")) {
		if (argc > 2)
			warn("ignoring arguments after `%s'", subcommand);
		return REQ_VIEW_STATUS;

	} else if (!strcmp(subcommand, "blame")) {
		if (argc <= 2 || argc > 4)
			die("invalid number of options to blame\n\n%s", usage);

		i = 2;
		if (argc == 4) {
			string_ncopy(opt_ref, argv[i], strlen(argv[i]));
			i++;
		}

		string_ncopy(opt_file, argv[i], strlen(argv[i]));
		return REQ_VIEW_BLAME;

	} else if (!strcmp(subcommand, "show")) {
		request = REQ_VIEW_DIFF;

	} else {
		subcommand = NULL;
	}

	if (subcommand) {
		custom_argv[1] = subcommand;
		j = 2;
	}

	for (i = 1 + !!subcommand; i < argc; i++) {
		const char *opt = argv[i];

		if (seen_dashdash || !strcmp(opt, "--")) {
			seen_dashdash = TRUE;

		} else if (!strcmp(opt, "-v") || !strcmp(opt, "--version")) {
			printf("tig version %s\n", TIG_VERSION);
			quit(0);

		} else if (!strcmp(opt, "-h") || !strcmp(opt, "--help")) {
			printf("%s\n", usage);
			quit(0);
		}

		custom_argv[j++] = opt;
		if (j >= ARRAY_SIZE(custom_argv))
			die("command too long");
	}

	if (!prepare_update(VIEW(request), custom_argv, NULL, FORMAT_NONE))                                                                        
		die("Failed to format arguments"); 

	return request;
}

int
main(int argc, const char *argv[])
{
	enum request request = parse_options(argc, argv);
	struct view *view;
	size_t i;

	signal(SIGINT, quit);

	if (setlocale(LC_ALL, "")) {
		char *codeset = nl_langinfo(CODESET);

		string_ncopy(opt_codeset, codeset, strlen(codeset));
	}

	if (load_repo_info() == ERR)
		die("Failed to load repo info.");

	if (load_options() == ERR)
		die("Failed to load user config.");

	if (load_git_config() == ERR)
		die("Failed to load repo config.");

	/* Require a git repository unless when running in pager mode. */
	if (!opt_git_dir[0] && request != REQ_VIEW_PAGER)
		die("Not a git repository");

	if (*opt_encoding && strcasecmp(opt_encoding, "UTF-8"))
		opt_utf8 = FALSE;

	if (*opt_codeset && strcmp(opt_codeset, opt_encoding)) {
		opt_iconv = iconv_open(opt_codeset, opt_encoding);
		if (opt_iconv == ICONV_NONE)
			die("Failed to initialize character set conversion");
	}

	if (load_refs() == ERR)
		die("Failed to load refs.");

	foreach_view (view, i)
		argv_from_env(view->ops->argv, view->cmd_env);

	init_display();

	if (request != REQ_NONE)
		open_view(NULL, request, OPEN_PREPARED);
	request = request == REQ_NONE ? REQ_VIEW_MAIN : REQ_NONE;

	while (view_driver(display[current_view], request)) {
		int key = get_input(0);

		view = display[current_view];
		request = get_keybinding(view->keymap, key);

		/* Some low-level request handling. This keeps access to
		 * status_win restricted. */
		switch (request) {
		case REQ_PROMPT:
		{
			char *cmd = read_prompt(":");

			if (cmd) {
				struct view *next = VIEW(REQ_VIEW_PAGER);
				const char *argv[SIZEOF_ARG] = { "git" };
				int argc = 1;

				/* When running random commands, initially show the
				 * command in the title. However, it maybe later be
				 * overwritten if a commit line is selected. */
				string_ncopy(next->ref, cmd, strlen(cmd));

				if (!argv_from_string(argv, &argc, cmd)) {
					report("Too many arguments");
				} else if (!prepare_update(next, argv, NULL, FORMAT_DASH)) {
					report("Failed to format command");
				} else {
					open_view(view, REQ_VIEW_PAGER, OPEN_PREPARED);
				}
			}

			request = REQ_NONE;
			break;
		}
		case REQ_SEARCH:
		case REQ_SEARCH_BACK:
		{
			const char *prompt = request == REQ_SEARCH ? "/" : "?";
			char *search = read_prompt(prompt);

			if (search)
				string_ncopy(opt_search, search, strlen(search));
			else if (*opt_search)
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
