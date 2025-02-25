/*
 * apply.c
 *
 * Copyright (C) Linus Torvalds, 2005
 *
 * This applies patches on top of some (arbitrary) version of the SCM.
 *
 */
#include "cache.h"
#include "cache-tree.h"
#include "quote.h"
#include "blob.h"
#include "delta.h"
#include "builtin.h"
#include "string-list.h"
#include "dir.h"
#include "parse-options.h"

/*
 *  --check turns on checking that the working tree matches the
 *    files that are being modified, but doesn't apply the patch
 *  --stat does just a diffstat, and doesn't actually apply
 *  --numstat does numeric diffstat, and doesn't actually apply
 *  --index-info shows the old and new index info for paths if available.
 *  --index updates the cache as well.
 *  --cached updates only the cache without ever touching the working tree.
 */
static const char *prefix;
static int prefix_length = -1;
static int newfd = -1;

static int unidiff_zero;
static int p_value = 1;
static int p_value_known;
static int check_index;
static int update_index;
static int cached;
static int diffstat;
static int numstat;
static int summary;
static int check;
static int apply = 1;
static int apply_in_reverse;
static int apply_with_reject;
static int apply_verbosely;
static int no_add;
static const char *fake_ancestor;
static int line_termination = '\n';
static unsigned int p_context = UINT_MAX;
static const char * const apply_usage[] = {
	"git apply [options] [<patch>...]",
	NULL
};

static enum ws_error_action {
	nowarn_ws_error,
	warn_on_ws_error,
	die_on_ws_error,
	correct_ws_error,
} ws_error_action = warn_on_ws_error;
static int whitespace_error;
static int squelch_whitespace_errors = 5;
static int applied_after_fixing_ws;
static const char *patch_input_file;
static const char *root;
static int root_len;
static int read_stdin = 1;
static int options;

static void parse_whitespace_option(const char *option)
{
	if (!option) {
		ws_error_action = warn_on_ws_error;
		return;
	}
	if (!strcmp(option, "warn")) {
		ws_error_action = warn_on_ws_error;
		return;
	}
	if (!strcmp(option, "nowarn")) {
		ws_error_action = nowarn_ws_error;
		return;
	}
	if (!strcmp(option, "error")) {
		ws_error_action = die_on_ws_error;
		return;
	}
	if (!strcmp(option, "error-all")) {
		ws_error_action = die_on_ws_error;
		squelch_whitespace_errors = 0;
		return;
	}
	if (!strcmp(option, "strip") || !strcmp(option, "fix")) {
		ws_error_action = correct_ws_error;
		return;
	}
	die("unrecognized whitespace option '%s'", option);
}

static void set_default_whitespace_mode(const char *whitespace_option)
{
	if (!whitespace_option && !apply_default_whitespace)
		ws_error_action = (apply ? warn_on_ws_error : nowarn_ws_error);
}

/*
 * For "diff-stat" like behaviour, we keep track of the biggest change
 * we've seen, and the longest filename. That allows us to do simple
 * scaling.
 */
static int max_change, max_len;

/*
 * Various "current state", notably line numbers and what
 * file (and how) we're patching right now.. The "is_xxxx"
 * things are flags, where -1 means "don't know yet".
 */
static int linenr = 1;

/*
 * This represents one "hunk" from a patch, starting with
 * "@@ -oldpos,oldlines +newpos,newlines @@" marker.  The
 * patch text is pointed at by patch, and its byte length
 * is stored in size.  leading and trailing are the number
 * of context lines.
 */
struct fragment {
	unsigned long leading, trailing;
	unsigned long oldpos, oldlines;
	unsigned long newpos, newlines;
	const char *patch;
	int size;
	int rejected;
	struct fragment *next;
};

/*
 * When dealing with a binary patch, we reuse "leading" field
 * to store the type of the binary hunk, either deflated "delta"
 * or deflated "literal".
 */
#define binary_patch_method leading
#define BINARY_DELTA_DEFLATED	1
#define BINARY_LITERAL_DEFLATED 2

/*
 * This represents a "patch" to a file, both metainfo changes
 * such as creation/deletion, filemode and content changes represented
 * as a series of fragments.
 */
struct patch {
	char *new_name, *old_name, *def_name;
	unsigned int old_mode, new_mode;
	int is_new, is_delete;	/* -1 = unknown, 0 = false, 1 = true */
	int rejected;
	unsigned ws_rule;
	unsigned long deflate_origlen;
	int lines_added, lines_deleted;
	int score;
	unsigned int is_toplevel_relative:1;
	unsigned int inaccurate_eof:1;
	unsigned int is_binary:1;
	unsigned int is_copy:1;
	unsigned int is_rename:1;
	unsigned int recount:1;
	struct fragment *fragments;
	char *result;
	size_t resultsize;
	char old_sha1_prefix[41];
	char new_sha1_prefix[41];
	struct patch *next;
};

/*
 * A line in a file, len-bytes long (includes the terminating LF,
 * except for an incomplete line at the end if the file ends with
 * one), and its contents hashes to 'hash'.
 */
struct line {
	size_t len;
	unsigned hash : 24;
	unsigned flag : 8;
#define LINE_COMMON     1
};

/*
 * This represents a "file", which is an array of "lines".
 */
struct image {
	char *buf;
	size_t len;
	size_t nr;
	size_t alloc;
	struct line *line_allocated;
	struct line *line;
};

/*
 * Records filenames that have been touched, in order to handle
 * the case where more than one patches touch the same file.
 */

static struct string_list fn_table;

static uint32_t hash_line(const char *cp, size_t len)
{
	size_t i;
	uint32_t h;
	for (i = 0, h = 0; i < len; i++) {
		if (!isspace(cp[i])) {
			h = h * 3 + (cp[i] & 0xff);
		}
	}
	return h;
}

static void add_line_info(struct image *img, const char *bol, size_t len, unsigned flag)
{
	ALLOC_GROW(img->line_allocated, img->nr + 1, img->alloc);
	img->line_allocated[img->nr].len = len;
	img->line_allocated[img->nr].hash = hash_line(bol, len);
	img->line_allocated[img->nr].flag = flag;
	img->nr++;
}

static void prepare_image(struct image *image, char *buf, size_t len,
			  int prepare_linetable)
{
	const char *cp, *ep;

	memset(image, 0, sizeof(*image));
	image->buf = buf;
	image->len = len;

	if (!prepare_linetable)
		return;

	ep = image->buf + image->len;
	cp = image->buf;
	while (cp < ep) {
		const char *next;
		for (next = cp; next < ep && *next != '\n'; next++)
			;
		if (next < ep)
			next++;
		add_line_info(image, cp, next - cp, 0);
		cp = next;
	}
	image->line = image->line_allocated;
}

static void clear_image(struct image *image)
{
	free(image->buf);
	image->buf = NULL;
	image->len = 0;
}

static void say_patch_name(FILE *output, const char *pre,
			   struct patch *patch, const char *post)
{
	fputs(pre, output);
	if (patch->old_name && patch->new_name &&
	    strcmp(patch->old_name, patch->new_name)) {
		quote_c_style(patch->old_name, NULL, output, 0);
		fputs(" => ", output);
		quote_c_style(patch->new_name, NULL, output, 0);
	} else {
		const char *n = patch->new_name;
		if (!n)
			n = patch->old_name;
		quote_c_style(n, NULL, output, 0);
	}
	fputs(post, output);
}

#define CHUNKSIZE (8192)
#define SLOP (16)

static void read_patch_file(struct strbuf *sb, int fd)
{
	if (strbuf_read(sb, fd, 0) < 0)
		die("git apply: read returned %s", strerror(errno));

	/*
	 * Make sure that we have some slop in the buffer
	 * so that we can do speculative "memcmp" etc, and
	 * see to it that it is NUL-filled.
	 */
	strbuf_grow(sb, SLOP);
	memset(sb->buf + sb->len, 0, SLOP);
}

static unsigned long linelen(const char *buffer, unsigned long size)
{
	unsigned long len = 0;
	while (size--) {
		len++;
		if (*buffer++ == '\n')
			break;
	}
	return len;
}

static int is_dev_null(const char *str)
{
	return !memcmp("/dev/null", str, 9) && isspace(str[9]);
}

#define TERM_SPACE	1
#define TERM_TAB	2

static int name_terminate(const char *name, int namelen, int c, int terminate)
{
	if (c == ' ' && !(terminate & TERM_SPACE))
		return 0;
	if (c == '\t' && !(terminate & TERM_TAB))
		return 0;

	return 1;
}

/* remove double slashes to make --index work with such filenames */
static char *squash_slash(char *name)
{
	int i = 0, j = 0;

	while (name[i]) {
		if ((name[j++] = name[i++]) == '/')
			while (name[i] == '/')
				i++;
	}
	name[j] = '\0';
	return name;
}

static char *find_name(const char *line, char *def, int p_value, int terminate)
{
	int len;
	const char *start = line;

	if (*line == '"') {
		struct strbuf name = STRBUF_INIT;

		/*
		 * Proposed "new-style" GNU patch/diff format; see
		 * http://marc.theaimsgroup.com/?l=git&m=112927316408690&w=2
		 */
		if (!unquote_c_style(&name, line, NULL)) {
			char *cp;

			for (cp = name.buf; p_value; p_value--) {
				cp = strchr(cp, '/');
				if (!cp)
					break;
				cp++;
			}
			if (cp) {
				/* name can later be freed, so we need
				 * to memmove, not just return cp
				 */
				strbuf_remove(&name, 0, cp - name.buf);
				free(def);
				if (root)
					strbuf_insert(&name, 0, root, root_len);
				return squash_slash(strbuf_detach(&name, NULL));
			}
		}
		strbuf_release(&name);
	}

	for (;;) {
		char c = *line;

		if (isspace(c)) {
			if (c == '\n')
				break;
			if (name_terminate(start, line-start, c, terminate))
				break;
		}
		line++;
		if (c == '/' && !--p_value)
			start = line;
	}
	if (!start)
		return squash_slash(def);
	len = line - start;
	if (!len)
		return squash_slash(def);

	/*
	 * Generally we prefer the shorter name, especially
	 * if the other one is just a variation of that with
	 * something else tacked on to the end (ie "file.orig"
	 * or "file~").
	 */
	if (def) {
		int deflen = strlen(def);
		if (deflen < len && !strncmp(start, def, deflen))
			return squash_slash(def);
		free(def);
	}

	if (root) {
		char *ret = xmalloc(root_len + len + 1);
		strcpy(ret, root);
		memcpy(ret + root_len, start, len);
		ret[root_len + len] = '\0';
		return squash_slash(ret);
	}

	return squash_slash(xmemdupz(start, len));
}

static int count_slashes(const char *cp)
{
	int cnt = 0;
	char ch;

	while ((ch = *cp++))
		if (ch == '/')
			cnt++;
	return cnt;
}

/*
 * Given the string after "--- " or "+++ ", guess the appropriate
 * p_value for the given patch.
 */
static int guess_p_value(const char *nameline)
{
	char *name, *cp;
	int val = -1;

	if (is_dev_null(nameline))
		return -1;
	name = find_name(nameline, NULL, 0, TERM_SPACE | TERM_TAB);
	if (!name)
		return -1;
	cp = strchr(name, '/');
	if (!cp)
		val = 0;
	else if (prefix) {
		/*
		 * Does it begin with "a/$our-prefix" and such?  Then this is
		 * very likely to apply to our directory.
		 */
		if (!strncmp(name, prefix, prefix_length))
			val = count_slashes(prefix);
		else {
			cp++;
			if (!strncmp(cp, prefix, prefix_length))
				val = count_slashes(prefix) + 1;
		}
	}
	free(name);
	return val;
}

/*
 * Get the name etc info from the ---/+++ lines of a traditional patch header
 *
 * FIXME! The end-of-filename heuristics are kind of screwy. For existing
 * files, we can happily check the index for a match, but for creating a
 * new file we should try to match whatever "patch" does. I have no idea.
 */
static void parse_traditional_patch(const char *first, const char *second, struct patch *patch)
{
	char *name;

	first += 4;	/* skip "--- " */
	second += 4;	/* skip "+++ " */
	if (!p_value_known) {
		int p, q;
		p = guess_p_value(first);
		q = guess_p_value(second);
		if (p < 0) p = q;
		if (0 <= p && p == q) {
			p_value = p;
			p_value_known = 1;
		}
	}
	if (is_dev_null(first)) {
		patch->is_new = 1;
		patch->is_delete = 0;
		name = find_name(second, NULL, p_value, TERM_SPACE | TERM_TAB);
		patch->new_name = name;
	} else if (is_dev_null(second)) {
		patch->is_new = 0;
		patch->is_delete = 1;
		name = find_name(first, NULL, p_value, TERM_SPACE | TERM_TAB);
		patch->old_name = name;
	} else {
		name = find_name(first, NULL, p_value, TERM_SPACE | TERM_TAB);
		name = find_name(second, name, p_value, TERM_SPACE | TERM_TAB);
		patch->old_name = patch->new_name = name;
	}
	if (!name)
		die("unable to find filename in patch at line %d", linenr);
}

static int gitdiff_hdrend(const char *line, struct patch *patch)
{
	return -1;
}

/*
 * We're anal about diff header consistency, to make
 * sure that we don't end up having strange ambiguous
 * patches floating around.
 *
 * As a result, gitdiff_{old|new}name() will check
 * their names against any previous information, just
 * to make sure..
 */
static char *gitdiff_verify_name(const char *line, int isnull, char *orig_name, const char *oldnew)
{
	if (!orig_name && !isnull)
		return find_name(line, NULL, p_value, TERM_TAB);

	if (orig_name) {
		int len;
		const char *name;
		char *another;
		name = orig_name;
		len = strlen(name);
		if (isnull)
			die("git apply: bad git-diff - expected /dev/null, got %s on line %d", name, linenr);
		another = find_name(line, NULL, p_value, TERM_TAB);
		if (!another || memcmp(another, name, len))
			die("git apply: bad git-diff - inconsistent %s filename on line %d", oldnew, linenr);
		free(another);
		return orig_name;
	}
	else {
		/* expect "/dev/null" */
		if (memcmp("/dev/null", line, 9) || line[9] != '\n')
			die("git apply: bad git-diff - expected /dev/null on line %d", linenr);
		return NULL;
	}
}

static int gitdiff_oldname(const char *line, struct patch *patch)
{
	patch->old_name = gitdiff_verify_name(line, patch->is_new, patch->old_name, "old");
	return 0;
}

static int gitdiff_newname(const char *line, struct patch *patch)
{
	patch->new_name = gitdiff_verify_name(line, patch->is_delete, patch->new_name, "new");
	return 0;
}

static int gitdiff_oldmode(const char *line, struct patch *patch)
{
	patch->old_mode = strtoul(line, NULL, 8);
	return 0;
}

static int gitdiff_newmode(const char *line, struct patch *patch)
{
	patch->new_mode = strtoul(line, NULL, 8);
	return 0;
}

static int gitdiff_delete(const char *line, struct patch *patch)
{
	patch->is_delete = 1;
	patch->old_name = patch->def_name;
	return gitdiff_oldmode(line, patch);
}

static int gitdiff_newfile(const char *line, struct patch *patch)
{
	patch->is_new = 1;
	patch->new_name = patch->def_name;
	return gitdiff_newmode(line, patch);
}

static int gitdiff_copysrc(const char *line, struct patch *patch)
{
	patch->is_copy = 1;
	patch->old_name = find_name(line, NULL, 0, 0);
	return 0;
}

static int gitdiff_copydst(const char *line, struct patch *patch)
{
	patch->is_copy = 1;
	patch->new_name = find_name(line, NULL, 0, 0);
	return 0;
}

static int gitdiff_renamesrc(const char *line, struct patch *patch)
{
	patch->is_rename = 1;
	patch->old_name = find_name(line, NULL, 0, 0);
	return 0;
}

static int gitdiff_renamedst(const char *line, struct patch *patch)
{
	patch->is_rename = 1;
	patch->new_name = find_name(line, NULL, 0, 0);
	return 0;
}

static int gitdiff_similarity(const char *line, struct patch *patch)
{
	if ((patch->score = strtoul(line, NULL, 10)) == ULONG_MAX)
		patch->score = 0;
	return 0;
}

static int gitdiff_dissimilarity(const char *line, struct patch *patch)
{
	if ((patch->score = strtoul(line, NULL, 10)) == ULONG_MAX)
		patch->score = 0;
	return 0;
}

static int gitdiff_index(const char *line, struct patch *patch)
{
	/*
	 * index line is N hexadecimal, "..", N hexadecimal,
	 * and optional space with octal mode.
	 */
	const char *ptr, *eol;
	int len;

	ptr = strchr(line, '.');
	if (!ptr || ptr[1] != '.' || 40 < ptr - line)
		return 0;
	len = ptr - line;
	memcpy(patch->old_sha1_prefix, line, len);
	patch->old_sha1_prefix[len] = 0;

	line = ptr + 2;
	ptr = strchr(line, ' ');
	eol = strchr(line, '\n');

	if (!ptr || eol < ptr)
		ptr = eol;
	len = ptr - line;

	if (40 < len)
		return 0;
	memcpy(patch->new_sha1_prefix, line, len);
	patch->new_sha1_prefix[len] = 0;
	if (*ptr == ' ')
		patch->old_mode = strtoul(ptr+1, NULL, 8);
	return 0;
}

/*
 * This is normal for a diff that doesn't change anything: we'll fall through
 * into the next diff. Tell the parser to break out.
 */
static int gitdiff_unrecognized(const char *line, struct patch *patch)
{
	return -1;
}

static const char *stop_at_slash(const char *line, int llen)
{
	int i;

	for (i = 0; i < llen; i++) {
		int ch = line[i];
		if (ch == '/')
			return line + i;
	}
	return NULL;
}

/*
 * This is to extract the same name that appears on "diff --git"
 * line.  We do not find and return anything if it is a rename
 * patch, and it is OK because we will find the name elsewhere.
 * We need to reliably find name only when it is mode-change only,
 * creation or deletion of an empty file.  In any of these cases,
 * both sides are the same name under a/ and b/ respectively.
 */
static char *git_header_name(char *line, int llen)
{
	const char *name;
	const char *second = NULL;
	size_t len;

	line += strlen("diff --git ");
	llen -= strlen("diff --git ");

	if (*line == '"') {
		const char *cp;
		struct strbuf first = STRBUF_INIT;
		struct strbuf sp = STRBUF_INIT;

		if (unquote_c_style(&first, line, &second))
			goto free_and_fail1;

		/* advance to the first slash */
		cp = stop_at_slash(first.buf, first.len);
		/* we do not accept absolute paths */
		if (!cp || cp == first.buf)
			goto free_and_fail1;
		strbuf_remove(&first, 0, cp + 1 - first.buf);

		/*
		 * second points at one past closing dq of name.
		 * find the second name.
		 */
		while ((second < line + llen) && isspace(*second))
			second++;

		if (line + llen <= second)
			goto free_and_fail1;
		if (*second == '"') {
			if (unquote_c_style(&sp, second, NULL))
				goto free_and_fail1;
			cp = stop_at_slash(sp.buf, sp.len);
			if (!cp || cp == sp.buf)
				goto free_and_fail1;
			/* They must match, otherwise ignore */
			if (strcmp(cp + 1, first.buf))
				goto free_and_fail1;
			strbuf_release(&sp);
			return strbuf_detach(&first, NULL);
		}

		/* unquoted second */
		cp = stop_at_slash(second, line + llen - second);
		if (!cp || cp == second)
			goto free_and_fail1;
		cp++;
		if (line + llen - cp != first.len + 1 ||
		    memcmp(first.buf, cp, first.len))
			goto free_and_fail1;
		return strbuf_detach(&first, NULL);

	free_and_fail1:
		strbuf_release(&first);
		strbuf_release(&sp);
		return NULL;
	}

	/* unquoted first name */
	name = stop_at_slash(line, llen);
	if (!name || name == line)
		return NULL;
	name++;

	/*
	 * since the first name is unquoted, a dq if exists must be
	 * the beginning of the second name.
	 */
	for (second = name; second < line + llen; second++) {
		if (*second == '"') {
			struct strbuf sp = STRBUF_INIT;
			const char *np;

			if (unquote_c_style(&sp, second, NULL))
				goto free_and_fail2;

			np = stop_at_slash(sp.buf, sp.len);
			if (!np || np == sp.buf)
				goto free_and_fail2;
			np++;

			len = sp.buf + sp.len - np;
			if (len < second - name &&
			    !strncmp(np, name, len) &&
			    isspace(name[len])) {
				/* Good */
				strbuf_remove(&sp, 0, np - sp.buf);
				return strbuf_detach(&sp, NULL);
			}

		free_and_fail2:
			strbuf_release(&sp);
			return NULL;
		}
	}

	/*
	 * Accept a name only if it shows up twice, exactly the same
	 * form.
	 */
	for (len = 0 ; ; len++) {
		switch (name[len]) {
		default:
			continue;
		case '\n':
			return NULL;
		case '\t': case ' ':
			second = name+len;
			for (;;) {
				char c = *second++;
				if (c == '\n')
					return NULL;
				if (c == '/')
					break;
			}
			if (second[len] == '\n' && !memcmp(name, second, len)) {
				return xmemdupz(name, len);
			}
		}
	}
}

/* Verify that we recognize the lines following a git header */
static int parse_git_header(char *line, int len, unsigned int size, struct patch *patch)
{
	unsigned long offset;

	/* A git diff has explicit new/delete information, so we don't guess */
	patch->is_new = 0;
	patch->is_delete = 0;

	/*
	 * Some things may not have the old name in the
	 * rest of the headers anywhere (pure mode changes,
	 * or removing or adding empty files), so we get
	 * the default name from the header.
	 */
	patch->def_name = git_header_name(line, len);
	if (patch->def_name && root) {
		char *s = xmalloc(root_len + strlen(patch->def_name) + 1);
		strcpy(s, root);
		strcpy(s + root_len, patch->def_name);
		free(patch->def_name);
		patch->def_name = s;
	}

	line += len;
	size -= len;
	linenr++;
	for (offset = len ; size > 0 ; offset += len, size -= len, line += len, linenr++) {
		static const struct opentry {
			const char *str;
			int (*fn)(const char *, struct patch *);
		} optable[] = {
			{ "@@ -", gitdiff_hdrend },
			{ "--- ", gitdiff_oldname },
			{ "+++ ", gitdiff_newname },
			{ "old mode ", gitdiff_oldmode },
			{ "new mode ", gitdiff_newmode },
			{ "deleted file mode ", gitdiff_delete },
			{ "new file mode ", gitdiff_newfile },
			{ "copy from ", gitdiff_copysrc },
			{ "copy to ", gitdiff_copydst },
			{ "rename old ", gitdiff_renamesrc },
			{ "rename new ", gitdiff_renamedst },
			{ "rename from ", gitdiff_renamesrc },
			{ "rename to ", gitdiff_renamedst },
			{ "similarity index ", gitdiff_similarity },
			{ "dissimilarity index ", gitdiff_dissimilarity },
			{ "index ", gitdiff_index },
			{ "", gitdiff_unrecognized },
		};
		int i;

		len = linelen(line, size);
		if (!len || line[len-1] != '\n')
			break;
		for (i = 0; i < ARRAY_SIZE(optable); i++) {
			const struct opentry *p = optable + i;
			int oplen = strlen(p->str);
			if (len < oplen || memcmp(p->str, line, oplen))
				continue;
			if (p->fn(line + oplen, patch) < 0)
				return offset;
			break;
		}
	}

	return offset;
}

static int parse_num(const char *line, unsigned long *p)
{
	char *ptr;

	if (!isdigit(*line))
		return 0;
	*p = strtoul(line, &ptr, 10);
	return ptr - line;
}

static int parse_range(const char *line, int len, int offset, const char *expect,
		       unsigned long *p1, unsigned long *p2)
{
	int digits, ex;

	if (offset < 0 || offset >= len)
		return -1;
	line += offset;
	len -= offset;

	digits = parse_num(line, p1);
	if (!digits)
		return -1;

	offset += digits;
	line += digits;
	len -= digits;

	*p2 = 1;
	if (*line == ',') {
		digits = parse_num(line+1, p2);
		if (!digits)
			return -1;

		offset += digits+1;
		line += digits+1;
		len -= digits+1;
	}

	ex = strlen(expect);
	if (ex > len)
		return -1;
	if (memcmp(line, expect, ex))
		return -1;

	return offset + ex;
}

static void recount_diff(char *line, int size, struct fragment *fragment)
{
	int oldlines = 0, newlines = 0, ret = 0;

	if (size < 1) {
		warning("recount: ignore empty hunk");
		return;
	}

	for (;;) {
		int len = linelen(line, size);
		size -= len;
		line += len;

		if (size < 1)
			break;

		switch (*line) {
		case ' ': case '\n':
			newlines++;
			/* fall through */
		case '-':
			oldlines++;
			continue;
		case '+':
			newlines++;
			continue;
		case '\\':
			continue;
		case '@':
			ret = size < 3 || prefixcmp(line, "@@ ");
			break;
		case 'd':
			ret = size < 5 || prefixcmp(line, "diff ");
			break;
		default:
			ret = -1;
			break;
		}
		if (ret) {
			warning("recount: unexpected line: %.*s",
				(int)linelen(line, size), line);
			return;
		}
		break;
	}
	fragment->oldlines = oldlines;
	fragment->newlines = newlines;
}

/*
 * Parse a unified diff fragment header of the
 * form "@@ -a,b +c,d @@"
 */
static int parse_fragment_header(char *line, int len, struct fragment *fragment)
{
	int offset;

	if (!len || line[len-1] != '\n')
		return -1;

	/* Figure out the number of lines in a fragment */
	offset = parse_range(line, len, 4, " +", &fragment->oldpos, &fragment->oldlines);
	offset = parse_range(line, len, offset, " @@", &fragment->newpos, &fragment->newlines);

	return offset;
}

static int find_header(char *line, unsigned long size, int *hdrsize, struct patch *patch)
{
	unsigned long offset, len;

	patch->is_toplevel_relative = 0;
	patch->is_rename = patch->is_copy = 0;
	patch->is_new = patch->is_delete = -1;
	patch->old_mode = patch->new_mode = 0;
	patch->old_name = patch->new_name = NULL;
	for (offset = 0; size > 0; offset += len, size -= len, line += len, linenr++) {
		unsigned long nextlen;

		len = linelen(line, size);
		if (!len)
			break;

		/* Testing this early allows us to take a few shortcuts.. */
		if (len < 6)
			continue;

		/*
		 * Make sure we don't find any unconnected patch fragments.
		 * That's a sign that we didn't find a header, and that a
		 * patch has become corrupted/broken up.
		 */
		if (!memcmp("@@ -", line, 4)) {
			struct fragment dummy;
			if (parse_fragment_header(line, len, &dummy) < 0)
				continue;
			die("patch fragment without header at line %d: %.*s",
			    linenr, (int)len-1, line);
		}

		if (size < len + 6)
			break;

		/*
		 * Git patch? It might not have a real patch, just a rename
		 * or mode change, so we handle that specially
		 */
		if (!memcmp("diff --git ", line, 11)) {
			int git_hdr_len = parse_git_header(line, len, size, patch);
			if (git_hdr_len <= len)
				continue;
			if (!patch->old_name && !patch->new_name) {
				if (!patch->def_name)
					die("git diff header lacks filename information (line %d)", linenr);
				patch->old_name = patch->new_name = patch->def_name;
			}
			patch->is_toplevel_relative = 1;
			*hdrsize = git_hdr_len;
			return offset;
		}

		/* --- followed by +++ ? */
		if (memcmp("--- ", line,  4) || memcmp("+++ ", line + len, 4))
			continue;

		/*
		 * We only accept unified patches, so we want it to
		 * at least have "@@ -a,b +c,d @@\n", which is 14 chars
		 * minimum ("@@ -0,0 +1 @@\n" is the shortest).
		 */
		nextlen = linelen(line + len, size - len);
		if (size < nextlen + 14 || memcmp("@@ -", line + len + nextlen, 4))
			continue;

		/* Ok, we'll consider it a patch */
		parse_traditional_patch(line, line+len, patch);
		*hdrsize = len + nextlen;
		linenr += 2;
		return offset;
	}
	return -1;
}

static void check_whitespace(const char *line, int len, unsigned ws_rule)
{
	char *err;
	unsigned result = ws_check(line + 1, len - 1, ws_rule);
	if (!result)
		return;

	whitespace_error++;
	if (squelch_whitespace_errors &&
	    squelch_whitespace_errors < whitespace_error)
		;
	else {
		err = whitespace_error_string(result);
		fprintf(stderr, "%s:%d: %s.\n%.*s\n",
			patch_input_file, linenr, err, len - 2, line + 1);
		free(err);
	}
}

/*
 * Parse a unified diff. Note that this really needs to parse each
 * fragment separately, since the only way to know the difference
 * between a "---" that is part of a patch, and a "---" that starts
 * the next patch is to look at the line counts..
 */
static int parse_fragment(char *line, unsigned long size,
			  struct patch *patch, struct fragment *fragment)
{
	int added, deleted;
	int len = linelen(line, size), offset;
	unsigned long oldlines, newlines;
	unsigned long leading, trailing;

	offset = parse_fragment_header(line, len, fragment);
	if (offset < 0)
		return -1;
	if (offset > 0 && patch->recount)
		recount_diff(line + offset, size - offset, fragment);
	oldlines = fragment->oldlines;
	newlines = fragment->newlines;
	leading = 0;
	trailing = 0;

	/* Parse the thing.. */
	line += len;
	size -= len;
	linenr++;
	added = deleted = 0;
	for (offset = len;
	     0 < size;
	     offset += len, size -= len, line += len, linenr++) {
		if (!oldlines && !newlines)
			break;
		len = linelen(line, size);
		if (!len || line[len-1] != '\n')
			return -1;
		switch (*line) {
		default:
			return -1;
		case '\n': /* newer GNU diff, an empty context line */
		case ' ':
			oldlines--;
			newlines--;
			if (!deleted && !added)
				leading++;
			trailing++;
			break;
		case '-':
			if (apply_in_reverse &&
			    ws_error_action != nowarn_ws_error)
				check_whitespace(line, len, patch->ws_rule);
			deleted++;
			oldlines--;
			trailing = 0;
			break;
		case '+':
			if (!apply_in_reverse &&
			    ws_error_action != nowarn_ws_error)
				check_whitespace(line, len, patch->ws_rule);
			added++;
			newlines--;
			trailing = 0;
			break;

		/*
		 * We allow "\ No newline at end of file". Depending
                 * on locale settings when the patch was produced we
                 * don't know what this line looks like. The only
                 * thing we do know is that it begins with "\ ".
		 * Checking for 12 is just for sanity check -- any
		 * l10n of "\ No newline..." is at least that long.
		 */
		case '\\':
			if (len < 12 || memcmp(line, "\\ ", 2))
				return -1;
			break;
		}
	}
	if (oldlines || newlines)
		return -1;
	fragment->leading = leading;
	fragment->trailing = trailing;

	/*
	 * If a fragment ends with an incomplete line, we failed to include
	 * it in the above loop because we hit oldlines == newlines == 0
	 * before seeing it.
	 */
	if (12 < size && !memcmp(line, "\\ ", 2))
		offset += linelen(line, size);

	patch->lines_added += added;
	patch->lines_deleted += deleted;

	if (0 < patch->is_new && oldlines)
		return error("new file depends on old contents");
	if (0 < patch->is_delete && newlines)
		return error("deleted file still has contents");
	return offset;
}

static int parse_single_patch(char *line, unsigned long size, struct patch *patch)
{
	unsigned long offset = 0;
	unsigned long oldlines = 0, newlines = 0, context = 0;
	struct fragment **fragp = &patch->fragments;

	while (size > 4 && !memcmp(line, "@@ -", 4)) {
		struct fragment *fragment;
		int len;

		fragment = xcalloc(1, sizeof(*fragment));
		len = parse_fragment(line, size, patch, fragment);
		if (len <= 0)
			die("corrupt patch at line %d", linenr);
		fragment->patch = line;
		fragment->size = len;
		oldlines += fragment->oldlines;
		newlines += fragment->newlines;
		context += fragment->leading + fragment->trailing;

		*fragp = fragment;
		fragp = &fragment->next;

		offset += len;
		line += len;
		size -= len;
	}

	/*
	 * If something was removed (i.e. we have old-lines) it cannot
	 * be creation, and if something was added it cannot be
	 * deletion.  However, the reverse is not true; --unified=0
	 * patches that only add are not necessarily creation even
	 * though they do not have any old lines, and ones that only
	 * delete are not necessarily deletion.
	 *
	 * Unfortunately, a real creation/deletion patch do _not_ have
	 * any context line by definition, so we cannot safely tell it
	 * apart with --unified=0 insanity.  At least if the patch has
	 * more than one hunk it is not creation or deletion.
	 */
	if (patch->is_new < 0 &&
	    (oldlines || (patch->fragments && patch->fragments->next)))
		patch->is_new = 0;
	if (patch->is_delete < 0 &&
	    (newlines || (patch->fragments && patch->fragments->next)))
		patch->is_delete = 0;

	if (0 < patch->is_new && oldlines)
		die("new file %s depends on old contents", patch->new_name);
	if (0 < patch->is_delete && newlines)
		die("deleted file %s still has contents", patch->old_name);
	if (!patch->is_delete && !newlines && context)
		fprintf(stderr, "** warning: file %s becomes empty but "
			"is not deleted\n", patch->new_name);

	return offset;
}

static inline int metadata_changes(struct patch *patch)
{
	return	patch->is_rename > 0 ||
		patch->is_copy > 0 ||
		patch->is_new > 0 ||
		patch->is_delete ||
		(patch->old_mode && patch->new_mode &&
		 patch->old_mode != patch->new_mode);
}

static char *inflate_it(const void *data, unsigned long size,
			unsigned long inflated_size)
{
	z_stream stream;
	void *out;
	int st;

	memset(&stream, 0, sizeof(stream));

	stream.next_in = (unsigned char *)data;
	stream.avail_in = size;
	stream.next_out = out = xmalloc(inflated_size);
	stream.avail_out = inflated_size;
	git_inflate_init(&stream);
	st = git_inflate(&stream, Z_FINISH);
	git_inflate_end(&stream);
	if ((st != Z_STREAM_END) || stream.total_out != inflated_size) {
		free(out);
		return NULL;
	}
	return out;
}

static struct fragment *parse_binary_hunk(char **buf_p,
					  unsigned long *sz_p,
					  int *status_p,
					  int *used_p)
{
	/*
	 * Expect a line that begins with binary patch method ("literal"
	 * or "delta"), followed by the length of data before deflating.
	 * a sequence of 'length-byte' followed by base-85 encoded data
	 * should follow, terminated by a newline.
	 *
	 * Each 5-byte sequence of base-85 encodes up to 4 bytes,
	 * and we would limit the patch line to 66 characters,
	 * so one line can fit up to 13 groups that would decode
	 * to 52 bytes max.  The length byte 'A'-'Z' corresponds
	 * to 1-26 bytes, and 'a'-'z' corresponds to 27-52 bytes.
	 */
	int llen, used;
	unsigned long size = *sz_p;
	char *buffer = *buf_p;
	int patch_method;
	unsigned long origlen;
	char *data = NULL;
	int hunk_size = 0;
	struct fragment *frag;

	llen = linelen(buffer, size);
	used = llen;

	*status_p = 0;

	if (!prefixcmp(buffer, "delta ")) {
		patch_method = BINARY_DELTA_DEFLATED;
		origlen = strtoul(buffer + 6, NULL, 10);
	}
	else if (!prefixcmp(buffer, "literal ")) {
		patch_method = BINARY_LITERAL_DEFLATED;
		origlen = strtoul(buffer + 8, NULL, 10);
	}
	else
		return NULL;

	linenr++;
	buffer += llen;
	while (1) {
		int byte_length, max_byte_length, newsize;
		llen = linelen(buffer, size);
		used += llen;
		linenr++;
		if (llen == 1) {
			/* consume the blank line */
			buffer++;
			size--;
			break;
		}
		/*
		 * Minimum line is "A00000\n" which is 7-byte long,
		 * and the line length must be multiple of 5 plus 2.
		 */
		if ((llen < 7) || (llen-2) % 5)
			goto corrupt;
		max_byte_length = (llen - 2) / 5 * 4;
		byte_length = *buffer;
		if ('A' <= byte_length && byte_length <= 'Z')
			byte_length = byte_length - 'A' + 1;
		else if ('a' <= byte_length && byte_length <= 'z')
			byte_length = byte_length - 'a' + 27;
		else
			goto corrupt;
		/* if the input length was not multiple of 4, we would
		 * have filler at the end but the filler should never
		 * exceed 3 bytes
		 */
		if (max_byte_length < byte_length ||
		    byte_length <= max_byte_length - 4)
			goto corrupt;
		newsize = hunk_size + byte_length;
		data = xrealloc(data, newsize);
		if (decode_85(data + hunk_size, buffer + 1, byte_length))
			goto corrupt;
		hunk_size = newsize;
		buffer += llen;
		size -= llen;
	}

	frag = xcalloc(1, sizeof(*frag));
	frag->patch = inflate_it(data, hunk_size, origlen);
	if (!frag->patch)
		goto corrupt;
	free(data);
	frag->size = origlen;
	*buf_p = buffer;
	*sz_p = size;
	*used_p = used;
	frag->binary_patch_method = patch_method;
	return frag;

 corrupt:
	free(data);
	*status_p = -1;
	error("corrupt binary patch at line %d: %.*s",
	      linenr-1, llen-1, buffer);
	return NULL;
}

static int parse_binary(char *buffer, unsigned long size, struct patch *patch)
{
	/*
	 * We have read "GIT binary patch\n"; what follows is a line
	 * that says the patch method (currently, either "literal" or
	 * "delta") and the length of data before deflating; a
	 * sequence of 'length-byte' followed by base-85 encoded data
	 * follows.
	 *
	 * When a binary patch is reversible, there is another binary
	 * hunk in the same format, starting with patch method (either
	 * "literal" or "delta") with the length of data, and a sequence
	 * of length-byte + base-85 encoded data, terminated with another
	 * empty line.  This data, when applied to the postimage, produces
	 * the preimage.
	 */
	struct fragment *forward;
	struct fragment *reverse;
	int status;
	int used, used_1;

	forward = parse_binary_hunk(&buffer, &size, &status, &used);
	if (!forward && !status)
		/* there has to be one hunk (forward hunk) */
		return error("unrecognized binary patch at line %d", linenr-1);
	if (status)
		/* otherwise we already gave an error message */
		return status;

	reverse = parse_binary_hunk(&buffer, &size, &status, &used_1);
	if (reverse)
		used += used_1;
	else if (status) {
		/*
		 * Not having reverse hunk is not an error, but having
		 * a corrupt reverse hunk is.
		 */
		free((void*) forward->patch);
		free(forward);
		return status;
	}
	forward->next = reverse;
	patch->fragments = forward;
	patch->is_binary = 1;
	return used;
}

static int parse_chunk(char *buffer, unsigned long size, struct patch *patch)
{
	int hdrsize, patchsize;
	int offset = find_header(buffer, size, &hdrsize, patch);

	if (offset < 0)
		return offset;

	patch->ws_rule = whitespace_rule(patch->new_name
					 ? patch->new_name
					 : patch->old_name);

	patchsize = parse_single_patch(buffer + offset + hdrsize,
				       size - offset - hdrsize, patch);

	if (!patchsize) {
		static const char *binhdr[] = {
			"Binary files ",
			"Files ",
			NULL,
		};
		static const char git_binary[] = "GIT binary patch\n";
		int i;
		int hd = hdrsize + offset;
		unsigned long llen = linelen(buffer + hd, size - hd);

		if (llen == sizeof(git_binary) - 1 &&
		    !memcmp(git_binary, buffer + hd, llen)) {
			int used;
			linenr++;
			used = parse_binary(buffer + hd + llen,
					    size - hd - llen, patch);
			if (used)
				patchsize = used + llen;
			else
				patchsize = 0;
		}
		else if (!memcmp(" differ\n", buffer + hd + llen - 8, 8)) {
			for (i = 0; binhdr[i]; i++) {
				int len = strlen(binhdr[i]);
				if (len < size - hd &&
				    !memcmp(binhdr[i], buffer + hd, len)) {
					linenr++;
					patch->is_binary = 1;
					patchsize = llen;
					break;
				}
			}
		}

		/* Empty patch cannot be applied if it is a text patch
		 * without metadata change.  A binary patch appears
		 * empty to us here.
		 */
		if ((apply || check) &&
		    (!patch->is_binary && !metadata_changes(patch)))
			die("patch with only garbage at line %d", linenr);
	}

	return offset + hdrsize + patchsize;
}

#define swap(a,b) myswap((a),(b),sizeof(a))

#define myswap(a, b, size) do {		\
	unsigned char mytmp[size];	\
	memcpy(mytmp, &a, size);		\
	memcpy(&a, &b, size);		\
	memcpy(&b, mytmp, size);		\
} while (0)

static void reverse_patches(struct patch *p)
{
	for (; p; p = p->next) {
		struct fragment *frag = p->fragments;

		swap(p->new_name, p->old_name);
		swap(p->new_mode, p->old_mode);
		swap(p->is_new, p->is_delete);
		swap(p->lines_added, p->lines_deleted);
		swap(p->old_sha1_prefix, p->new_sha1_prefix);

		for (; frag; frag = frag->next) {
			swap(frag->newpos, frag->oldpos);
			swap(frag->newlines, frag->oldlines);
		}
	}
}

static const char pluses[] =
"++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++";
static const char minuses[]=
"----------------------------------------------------------------------";

static void show_stats(struct patch *patch)
{
	struct strbuf qname = STRBUF_INIT;
	char *cp = patch->new_name ? patch->new_name : patch->old_name;
	int max, add, del;

	quote_c_style(cp, &qname, NULL, 0);

	/*
	 * "scale" the filename
	 */
	max = max_len;
	if (max > 50)
		max = 50;

	if (qname.len > max) {
		cp = strchr(qname.buf + qname.len + 3 - max, '/');
		if (!cp)
			cp = qname.buf + qname.len + 3 - max;
		strbuf_splice(&qname, 0, cp - qname.buf, "...", 3);
	}

	if (patch->is_binary) {
		printf(" %-*s |  Bin\n", max, qname.buf);
		strbuf_release(&qname);
		return;
	}

	printf(" %-*s |", max, qname.buf);
	strbuf_release(&qname);

	/*
	 * scale the add/delete
	 */
	max = max + max_change > 70 ? 70 - max : max_change;
	add = patch->lines_added;
	del = patch->lines_deleted;

	if (max_change > 0) {
		int total = ((add + del) * max + max_change / 2) / max_change;
		add = (add * max + max_change / 2) / max_change;
		del = total - add;
	}
	printf("%5d %.*s%.*s\n", patch->lines_added + patch->lines_deleted,
		add, pluses, del, minuses);
}

static int read_old_data(struct stat *st, const char *path, struct strbuf *buf)
{
	switch (st->st_mode & S_IFMT) {
	case S_IFLNK:
		if (strbuf_readlink(buf, path, st->st_size) < 0)
			return error("unable to read symlink %s", path);
		return 0;
	case S_IFREG:
		if (strbuf_read_file(buf, path, st->st_size) != st->st_size)
			return error("unable to open or read %s", path);
		convert_to_git(path, buf->buf, buf->len, buf, 0);
		return 0;
	default:
		return -1;
	}
}

static void update_pre_post_images(struct image *preimage,
				   struct image *postimage,
				   char *buf,
				   size_t len)
{
	int i, ctx;
	char *new, *old, *fixed;
	struct image fixed_preimage;

	/*
	 * Update the preimage with whitespace fixes.  Note that we
	 * are not losing preimage->buf -- apply_one_fragment() will
	 * free "oldlines".
	 */
	prepare_image(&fixed_preimage, buf, len, 1);
	assert(fixed_preimage.nr == preimage->nr);
	for (i = 0; i < preimage->nr; i++)
		fixed_preimage.line[i].flag = preimage->line[i].flag;
	free(preimage->line_allocated);
	*preimage = fixed_preimage;

	/*
	 * Adjust the common context lines in postimage, in place.
	 * This is possible because whitespace fixing does not make
	 * the string grow.
	 */
	new = old = postimage->buf;
	fixed = preimage->buf;
	for (i = ctx = 0; i < postimage->nr; i++) {
		size_t len = postimage->line[i].len;
		if (!(postimage->line[i].flag & LINE_COMMON)) {
			/* an added line -- no counterparts in preimage */
			memmove(new, old, len);
			old += len;
			new += len;
			continue;
		}

		/* a common context -- skip it in the original postimage */
		old += len;

		/* and find the corresponding one in the fixed preimage */
		while (ctx < preimage->nr &&
		       !(preimage->line[ctx].flag & LINE_COMMON)) {
			fixed += preimage->line[ctx].len;
			ctx++;
		}
		if (preimage->nr <= ctx)
			die("oops");

		/* and copy it in, while fixing the line length */
		len = preimage->line[ctx].len;
		memcpy(new, fixed, len);
		new += len;
		fixed += len;
		postimage->line[i].len = len;
		ctx++;
	}

	/* Fix the length of the whole thing */
	postimage->len = new - postimage->buf;
}

static int match_fragment(struct image *img,
			  struct image *preimage,
			  struct image *postimage,
			  unsigned long try,
			  int try_lno,
			  unsigned ws_rule,
			  int match_beginning, int match_end)
{
	int i;
	char *fixed_buf, *buf, *orig, *target;

	if (preimage->nr + try_lno > img->nr)
		return 0;

	if (match_beginning && try_lno)
		return 0;

	if (match_end && preimage->nr + try_lno != img->nr)
		return 0;

	/* Quick hash check */
	for (i = 0; i < preimage->nr; i++)
		if (preimage->line[i].hash != img->line[try_lno + i].hash)
			return 0;

	/*
	 * Do we have an exact match?  If we were told to match
	 * at the end, size must be exactly at try+fragsize,
	 * otherwise try+fragsize must be still within the preimage,
	 * and either case, the old piece should match the preimage
	 * exactly.
	 */
	if ((match_end
	     ? (try + preimage->len == img->len)
	     : (try + preimage->len <= img->len)) &&
	    !memcmp(img->buf + try, preimage->buf, preimage->len))
		return 1;

	if (ws_error_action != correct_ws_error)
		return 0;

	/*
	 * The hunk does not apply byte-by-byte, but the hash says
	 * it might with whitespace fuzz.
	 */
	fixed_buf = xmalloc(preimage->len + 1);
	buf = fixed_buf;
	orig = preimage->buf;
	target = img->buf + try;
	for (i = 0; i < preimage->nr; i++) {
		size_t fixlen; /* length after fixing the preimage */
		size_t oldlen = preimage->line[i].len;
		size_t tgtlen = img->line[try_lno + i].len;
		size_t tgtfixlen; /* length after fixing the target line */
		char tgtfixbuf[1024], *tgtfix;
		int match;

		/* Try fixing the line in the preimage */
		fixlen = ws_fix_copy(buf, orig, oldlen, ws_rule, NULL);

		/* Try fixing the line in the target */
		if (sizeof(tgtfixbuf) > tgtlen)
			tgtfix = tgtfixbuf;
		else
			tgtfix = xmalloc(tgtlen);
		tgtfixlen = ws_fix_copy(tgtfix, target, tgtlen, ws_rule, NULL);

		/*
		 * If they match, either the preimage was based on
		 * a version before our tree fixed whitespace breakage,
		 * or we are lacking a whitespace-fix patch the tree
		 * the preimage was based on already had (i.e. target
		 * has whitespace breakage, the preimage doesn't).
		 * In either case, we are fixing the whitespace breakages
		 * so we might as well take the fix together with their
		 * real change.
		 */
		match = (tgtfixlen == fixlen && !memcmp(tgtfix, buf, fixlen));

		if (tgtfix != tgtfixbuf)
			free(tgtfix);
		if (!match)
			goto unmatch_exit;

		orig += oldlen;
		buf += fixlen;
		target += tgtlen;
	}

	/*
	 * Yes, the preimage is based on an older version that still
	 * has whitespace breakages unfixed, and fixing them makes the
	 * hunk match.  Update the context lines in the postimage.
	 */
	update_pre_post_images(preimage, postimage,
			       fixed_buf, buf - fixed_buf);
	return 1;

 unmatch_exit:
	free(fixed_buf);
	return 0;
}

static int find_pos(struct image *img,
		    struct image *preimage,
		    struct image *postimage,
		    int line,
		    unsigned ws_rule,
		    int match_beginning, int match_end)
{
	int i;
	unsigned long backwards, forwards, try;
	int backwards_lno, forwards_lno, try_lno;

	if (preimage->nr > img->nr)
		return -1;

	/*
	 * If match_begining or match_end is specified, there is no
	 * point starting from a wrong line that will never match and
	 * wander around and wait for a match at the specified end.
	 */
	if (match_beginning)
		line = 0;
	else if (match_end)
		line = img->nr - preimage->nr;

	if (line > img->nr)
		line = img->nr;

	try = 0;
	for (i = 0; i < line; i++)
		try += img->line[i].len;

	/*
	 * There's probably some smart way to do this, but I'll leave
	 * that to the smart and beautiful people. I'm simple and stupid.
	 */
	backwards = try;
	backwards_lno = line;
	forwards = try;
	forwards_lno = line;
	try_lno = line;

	for (i = 0; ; i++) {
		if (match_fragment(img, preimage, postimage,
				   try, try_lno, ws_rule,
				   match_beginning, match_end))
			return try_lno;

	again:
		if (backwards_lno == 0 && forwards_lno == img->nr)
			break;

		if (i & 1) {
			if (backwards_lno == 0) {
				i++;
				goto again;
			}
			backwards_lno--;
			backwards -= img->line[backwards_lno].len;
			try = backwards;
			try_lno = backwards_lno;
		} else {
			if (forwards_lno == img->nr) {
				i++;
				goto again;
			}
			forwards += img->line[forwards_lno].len;
			forwards_lno++;
			try = forwards;
			try_lno = forwards_lno;
		}

	}
	return -1;
}

static void remove_first_line(struct image *img)
{
	img->buf += img->line[0].len;
	img->len -= img->line[0].len;
	img->line++;
	img->nr--;
}

static void remove_last_line(struct image *img)
{
	img->len -= img->line[--img->nr].len;
}

static void update_image(struct image *img,
			 int applied_pos,
			 struct image *preimage,
			 struct image *postimage)
{
	/*
	 * remove the copy of preimage at offset in img
	 * and replace it with postimage
	 */
	int i, nr;
	size_t remove_count, insert_count, applied_at = 0;
	char *result;

	for (i = 0; i < applied_pos; i++)
		applied_at += img->line[i].len;

	remove_count = 0;
	for (i = 0; i < preimage->nr; i++)
		remove_count += img->line[applied_pos + i].len;
	insert_count = postimage->len;

	/* Adjust the contents */
	result = xmalloc(img->len + insert_count - remove_count + 1);
	memcpy(result, img->buf, applied_at);
	memcpy(result + applied_at, postimage->buf, postimage->len);
	memcpy(result + applied_at + postimage->len,
	       img->buf + (applied_at + remove_count),
	       img->len - (applied_at + remove_count));
	free(img->buf);
	img->buf = result;
	img->len += insert_count - remove_count;
	result[img->len] = '\0';

	/* Adjust the line table */
	nr = img->nr + postimage->nr - preimage->nr;
	if (preimage->nr < postimage->nr) {
		/*
		 * NOTE: this knows that we never call remove_first_line()
		 * on anything other than pre/post image.
		 */
		img->line = xrealloc(img->line, nr * sizeof(*img->line));
		img->line_allocated = img->line;
	}
	if (preimage->nr != postimage->nr)
		memmove(img->line + applied_pos + postimage->nr,
			img->line + applied_pos + preimage->nr,
			(img->nr - (applied_pos + preimage->nr)) *
			sizeof(*img->line));
	memcpy(img->line + applied_pos,
	       postimage->line,
	       postimage->nr * sizeof(*img->line));
	img->nr = nr;
}

static int apply_one_fragment(struct image *img, struct fragment *frag,
			      int inaccurate_eof, unsigned ws_rule)
{
	int match_beginning, match_end;
	const char *patch = frag->patch;
	int size = frag->size;
	char *old, *new, *oldlines, *newlines;
	int new_blank_lines_at_end = 0;
	unsigned long leading, trailing;
	int pos, applied_pos;
	struct image preimage;
	struct image postimage;

	memset(&preimage, 0, sizeof(preimage));
	memset(&postimage, 0, sizeof(postimage));
	oldlines = xmalloc(size);
	newlines = xmalloc(size);

	old = oldlines;
	new = newlines;
	while (size > 0) {
		char first;
		int len = linelen(patch, size);
		int plen, added;
		int added_blank_line = 0;

		if (!len)
			break;

		/*
		 * "plen" is how much of the line we should use for
		 * the actual patch data. Normally we just remove the
		 * first character on the line, but if the line is
		 * followed by "\ No newline", then we also remove the
		 * last one (which is the newline, of course).
		 */
		plen = len - 1;
		if (len < size && patch[len] == '\\')
			plen--;
		first = *patch;
		if (apply_in_reverse) {
			if (first == '-')
				first = '+';
			else if (first == '+')
				first = '-';
		}

		switch (first) {
		case '\n':
			/* Newer GNU diff, empty context line */
			if (plen < 0)
				/* ... followed by '\No newline'; nothing */
				break;
			*old++ = '\n';
			*new++ = '\n';
			add_line_info(&preimage, "\n", 1, LINE_COMMON);
			add_line_info(&postimage, "\n", 1, LINE_COMMON);
			break;
		case ' ':
		case '-':
			memcpy(old, patch + 1, plen);
			add_line_info(&preimage, old, plen,
				      (first == ' ' ? LINE_COMMON : 0));
			old += plen;
			if (first == '-')
				break;
		/* Fall-through for ' ' */
		case '+':
			/* --no-add does not add new lines */
			if (first == '+' && no_add)
				break;

			if (first != '+' ||
			    !whitespace_error ||
			    ws_error_action != correct_ws_error) {
				memcpy(new, patch + 1, plen);
				added = plen;
			}
			else {
				added = ws_fix_copy(new, patch + 1, plen, ws_rule, &applied_after_fixing_ws);
			}
			add_line_info(&postimage, new, added,
				      (first == '+' ? 0 : LINE_COMMON));
			new += added;
			if (first == '+' &&
			    added == 1 && new[-1] == '\n')
				added_blank_line = 1;
			break;
		case '@': case '\\':
			/* Ignore it, we already handled it */
			break;
		default:
			if (apply_verbosely)
				error("invalid start of line: '%c'", first);
			return -1;
		}
		if (added_blank_line)
			new_blank_lines_at_end++;
		else
			new_blank_lines_at_end = 0;
		patch += len;
		size -= len;
	}
	if (inaccurate_eof &&
	    old > oldlines && old[-1] == '\n' &&
	    new > newlines && new[-1] == '\n') {
		old--;
		new--;
	}

	leading = frag->leading;
	trailing = frag->trailing;

	/*
	 * A hunk to change lines at the beginning would begin with
	 * @@ -1,L +N,M @@
	 * but we need to be careful.  -U0 that inserts before the second
	 * line also has this pattern.
	 *
	 * And a hunk to add to an empty file would begin with
	 * @@ -0,0 +N,M @@
	 *
	 * In other words, a hunk that is (frag->oldpos <= 1) with or
	 * without leading context must match at the beginning.
	 */
	match_beginning = (!frag->oldpos ||
			   (frag->oldpos == 1 && !unidiff_zero));

	/*
	 * A hunk without trailing lines must match at the end.
	 * However, we simply cannot tell if a hunk must match end
	 * from the lack of trailing lines if the patch was generated
	 * with unidiff without any context.
	 */
	match_end = !unidiff_zero && !trailing;

	pos = frag->newpos ? (frag->newpos - 1) : 0;
	preimage.buf = oldlines;
	preimage.len = old - oldlines;
	postimage.buf = newlines;
	postimage.len = new - newlines;
	preimage.line = preimage.line_allocated;
	postimage.line = postimage.line_allocated;

	for (;;) {

		applied_pos = find_pos(img, &preimage, &postimage, pos,
				       ws_rule, match_beginning, match_end);

		if (applied_pos >= 0)
			break;

		/* Am I at my context limits? */
		if ((leading <= p_context) && (trailing <= p_context))
			break;
		if (match_beginning || match_end) {
			match_beginning = match_end = 0;
			continue;
		}

		/*
		 * Reduce the number of context lines; reduce both
		 * leading and trailing if they are equal otherwise
		 * just reduce the larger context.
		 */
		if (leading >= trailing) {
			remove_first_line(&preimage);
			remove_first_line(&postimage);
			pos--;
			leading--;
		}
		if (trailing > leading) {
			remove_last_line(&preimage);
			remove_last_line(&postimage);
			trailing--;
		}
	}

	if (applied_pos >= 0) {
		if (ws_error_action == correct_ws_error &&
		    new_blank_lines_at_end &&
		    postimage.nr + applied_pos == img->nr) {
			/*
			 * If the patch application adds blank lines
			 * at the end, and if the patch applies at the
			 * end of the image, remove those added blank
			 * lines.
			 */
			while (new_blank_lines_at_end--)
				remove_last_line(&postimage);
		}

		/*
		 * Warn if it was necessary to reduce the number
		 * of context lines.
		 */
		if ((leading != frag->leading) ||
		    (trailing != frag->trailing))
			fprintf(stderr, "Context reduced to (%ld/%ld)"
				" to apply fragment at %d\n",
				leading, trailing, applied_pos+1);
		update_image(img, applied_pos, &preimage, &postimage);
	} else {
		if (apply_verbosely)
			error("while searching for:\n%.*s",
			      (int)(old - oldlines), oldlines);
	}

	free(oldlines);
	free(newlines);
	free(preimage.line_allocated);
	free(postimage.line_allocated);

	return (applied_pos < 0);
}

static int apply_binary_fragment(struct image *img, struct patch *patch)
{
	struct fragment *fragment = patch->fragments;
	unsigned long len;
	void *dst;

	/* Binary patch is irreversible without the optional second hunk */
	if (apply_in_reverse) {
		if (!fragment->next)
			return error("cannot reverse-apply a binary patch "
				     "without the reverse hunk to '%s'",
				     patch->new_name
				     ? patch->new_name : patch->old_name);
		fragment = fragment->next;
	}
	switch (fragment->binary_patch_method) {
	case BINARY_DELTA_DEFLATED:
		dst = patch_delta(img->buf, img->len, fragment->patch,
				  fragment->size, &len);
		if (!dst)
			return -1;
		clear_image(img);
		img->buf = dst;
		img->len = len;
		return 0;
	case BINARY_LITERAL_DEFLATED:
		clear_image(img);
		img->len = fragment->size;
		img->buf = xmalloc(img->len+1);
		memcpy(img->buf, fragment->patch, img->len);
		img->buf[img->len] = '\0';
		return 0;
	}
	return -1;
}

static int apply_binary(struct image *img, struct patch *patch)
{
	const char *name = patch->old_name ? patch->old_name : patch->new_name;
	unsigned char sha1[20];

	/*
	 * For safety, we require patch index line to contain
	 * full 40-byte textual SHA1 for old and new, at least for now.
	 */
	if (strlen(patch->old_sha1_prefix) != 40 ||
	    strlen(patch->new_sha1_prefix) != 40 ||
	    get_sha1_hex(patch->old_sha1_prefix, sha1) ||
	    get_sha1_hex(patch->new_sha1_prefix, sha1))
		return error("cannot apply binary patch to '%s' "
			     "without full index line", name);

	if (patch->old_name) {
		/*
		 * See if the old one matches what the patch
		 * applies to.
		 */
		hash_sha1_file(img->buf, img->len, blob_type, sha1);
		if (strcmp(sha1_to_hex(sha1), patch->old_sha1_prefix))
			return error("the patch applies to '%s' (%s), "
				     "which does not match the "
				     "current contents.",
				     name, sha1_to_hex(sha1));
	}
	else {
		/* Otherwise, the old one must be empty. */
		if (img->len)
			return error("the patch applies to an empty "
				     "'%s' but it is not empty", name);
	}

	get_sha1_hex(patch->new_sha1_prefix, sha1);
	if (is_null_sha1(sha1)) {
		clear_image(img);
		return 0; /* deletion patch */
	}

	if (has_sha1_file(sha1)) {
		/* We already have the postimage */
		enum object_type type;
		unsigned long size;
		char *result;

		result = read_sha1_file(sha1, &type, &size);
		if (!result)
			return error("the necessary postimage %s for "
				     "'%s' cannot be read",
				     patch->new_sha1_prefix, name);
		clear_image(img);
		img->buf = result;
		img->len = size;
	} else {
		/*
		 * We have verified buf matches the preimage;
		 * apply the patch data to it, which is stored
		 * in the patch->fragments->{patch,size}.
		 */
		if (apply_binary_fragment(img, patch))
			return error("binary patch does not apply to '%s'",
				     name);

		/* verify that the result matches */
		hash_sha1_file(img->buf, img->len, blob_type, sha1);
		if (strcmp(sha1_to_hex(sha1), patch->new_sha1_prefix))
			return error("binary patch to '%s' creates incorrect result (expecting %s, got %s)",
				name, patch->new_sha1_prefix, sha1_to_hex(sha1));
	}

	return 0;
}

static int apply_fragments(struct image *img, struct patch *patch)
{
	struct fragment *frag = patch->fragments;
	const char *name = patch->old_name ? patch->old_name : patch->new_name;
	unsigned ws_rule = patch->ws_rule;
	unsigned inaccurate_eof = patch->inaccurate_eof;

	if (patch->is_binary)
		return apply_binary(img, patch);

	while (frag) {
		if (apply_one_fragment(img, frag, inaccurate_eof, ws_rule)) {
			error("patch failed: %s:%ld", name, frag->oldpos);
			if (!apply_with_reject)
				return -1;
			frag->rejected = 1;
		}
		frag = frag->next;
	}
	return 0;
}

static int read_file_or_gitlink(struct cache_entry *ce, struct strbuf *buf)
{
	if (!ce)
		return 0;

	if (S_ISGITLINK(ce->ce_mode)) {
		strbuf_grow(buf, 100);
		strbuf_addf(buf, "Subproject commit %s\n", sha1_to_hex(ce->sha1));
	} else {
		enum object_type type;
		unsigned long sz;
		char *result;

		result = read_sha1_file(ce->sha1, &type, &sz);
		if (!result)
			return -1;
		/* XXX read_sha1_file NUL-terminates */
		strbuf_attach(buf, result, sz, sz + 1);
	}
	return 0;
}

static struct patch *in_fn_table(const char *name)
{
	struct string_list_item *item;

	if (name == NULL)
		return NULL;

	item = string_list_lookup(name, &fn_table);
	if (item != NULL)
		return (struct patch *)item->util;

	return NULL;
}

/*
 * item->util in the filename table records the status of the path.
 * Usually it points at a patch (whose result records the contents
 * of it after applying it), but it could be PATH_WAS_DELETED for a
 * path that a previously applied patch has already removed.
 */
 #define PATH_TO_BE_DELETED ((struct patch *) -2)
#define PATH_WAS_DELETED ((struct patch *) -1)

static int to_be_deleted(struct patch *patch)
{
	return patch == PATH_TO_BE_DELETED;
}

static int was_deleted(struct patch *patch)
{
	return patch == PATH_WAS_DELETED;
}

static void add_to_fn_table(struct patch *patch)
{
	struct string_list_item *item;

	/*
	 * Always add new_name unless patch is a deletion
	 * This should cover the cases for normal diffs,
	 * file creations and copies
	 */
	if (patch->new_name != NULL) {
		item = string_list_insert(patch->new_name, &fn_table);
		item->util = patch;
	}

	/*
	 * store a failure on rename/deletion cases because
	 * later chunks shouldn't patch old names
	 */
	if ((patch->new_name == NULL) || (patch->is_rename)) {
		item = string_list_insert(patch->old_name, &fn_table);
		item->util = PATH_WAS_DELETED;
	}
}

static void prepare_fn_table(struct patch *patch)
{
	/*
	 * store information about incoming file deletion
	 */
	while (patch) {
		if ((patch->new_name == NULL) || (patch->is_rename)) {
			struct string_list_item *item;
			item = string_list_insert(patch->old_name, &fn_table);
			item->util = PATH_TO_BE_DELETED;
		}
		patch = patch->next;
	}
}

static int apply_data(struct patch *patch, struct stat *st, struct cache_entry *ce)
{
	struct strbuf buf = STRBUF_INIT;
	struct image image;
	size_t len;
	char *img;
	struct patch *tpatch;

	if (!(patch->is_copy || patch->is_rename) &&
	    (tpatch = in_fn_table(patch->old_name)) != NULL && !to_be_deleted(tpatch)) {
		if (was_deleted(tpatch)) {
			return error("patch %s has been renamed/deleted",
				patch->old_name);
		}
		/* We have a patched copy in memory use that */
		strbuf_add(&buf, tpatch->result, tpatch->resultsize);
	} else if (cached) {
		if (read_file_or_gitlink(ce, &buf))
			return error("read of %s failed", patch->old_name);
	} else if (patch->old_name) {
		if (S_ISGITLINK(patch->old_mode)) {
			if (ce) {
				read_file_or_gitlink(ce, &buf);
			} else {
				/*
				 * There is no way to apply subproject
				 * patch without looking at the index.
				 */
				patch->fragments = NULL;
			}
		} else {
			if (read_old_data(st, patch->old_name, &buf))
				return error("read of %s failed", patch->old_name);
		}
	}

	img = strbuf_detach(&buf, &len);
	prepare_image(&image, img, len, !patch->is_binary);

	if (apply_fragments(&image, patch) < 0)
		return -1; /* note with --reject this succeeds. */
	patch->result = image.buf;
	patch->resultsize = image.len;
	add_to_fn_table(patch);
	free(image.line_allocated);

	if (0 < patch->is_delete && patch->resultsize)
		return error("removal patch leaves file contents");

	return 0;
}

static int check_to_create_blob(const char *new_name, int ok_if_exists)
{
	struct stat nst;
	if (!lstat(new_name, &nst)) {
		if (S_ISDIR(nst.st_mode) || ok_if_exists)
			return 0;
		/*
		 * A leading component of new_name might be a symlink
		 * that is going to be removed with this patch, but
		 * still pointing at somewhere that has the path.
		 * In such a case, path "new_name" does not exist as
		 * far as git is concerned.
		 */
		if (has_symlink_leading_path(new_name, strlen(new_name)))
			return 0;

		return error("%s: already exists in working directory", new_name);
	}
	else if ((errno != ENOENT) && (errno != ENOTDIR))
		return error("%s: %s", new_name, strerror(errno));
	return 0;
}

static int verify_index_match(struct cache_entry *ce, struct stat *st)
{
	if (S_ISGITLINK(ce->ce_mode)) {
		if (!S_ISDIR(st->st_mode))
			return -1;
		return 0;
	}
	return ce_match_stat(ce, st, CE_MATCH_IGNORE_VALID);
}

static int check_preimage(struct patch *patch, struct cache_entry **ce, struct stat *st)
{
	const char *old_name = patch->old_name;
	struct patch *tpatch = NULL;
	int stat_ret = 0;
	unsigned st_mode = 0;

	/*
	 * Make sure that we do not have local modifications from the
	 * index when we are looking at the index.  Also make sure
	 * we have the preimage file to be patched in the work tree,
	 * unless --cached, which tells git to apply only in the index.
	 */
	if (!old_name)
		return 0;

	assert(patch->is_new <= 0);

	if (!(patch->is_copy || patch->is_rename) &&
	    (tpatch = in_fn_table(old_name)) != NULL && !to_be_deleted(tpatch)) {
		if (was_deleted(tpatch))
			return error("%s: has been deleted/renamed", old_name);
		st_mode = tpatch->new_mode;
	} else if (!cached) {
		stat_ret = lstat(old_name, st);
		if (stat_ret && errno != ENOENT)
			return error("%s: %s", old_name, strerror(errno));
	}

	if (to_be_deleted(tpatch))
		tpatch = NULL;

	if (check_index && !tpatch) {
		int pos = cache_name_pos(old_name, strlen(old_name));
		if (pos < 0) {
			if (patch->is_new < 0)
				goto is_new;
			return error("%s: does not exist in index", old_name);
		}
		*ce = active_cache[pos];
		if (stat_ret < 0) {
			struct checkout costate;
			/* checkout */
			costate.base_dir = "";
			costate.base_dir_len = 0;
			costate.force = 0;
			costate.quiet = 0;
			costate.not_new = 0;
			costate.refresh_cache = 1;
			if (checkout_entry(*ce, &costate, NULL) ||
			    lstat(old_name, st))
				return -1;
		}
		if (!cached && verify_index_match(*ce, st))
			return error("%s: does not match index", old_name);
		if (cached)
			st_mode = (*ce)->ce_mode;
	} else if (stat_ret < 0) {
		if (patch->is_new < 0)
			goto is_new;
		return error("%s: %s", old_name, strerror(errno));
	}

	if (!cached && !tpatch)
		st_mode = ce_mode_from_stat(*ce, st->st_mode);

	if (patch->is_new < 0)
		patch->is_new = 0;
	if (!patch->old_mode)
		patch->old_mode = st_mode;
	if ((st_mode ^ patch->old_mode) & S_IFMT)
		return error("%s: wrong type", old_name);
	if (st_mode != patch->old_mode)
		warning("%s has type %o, expected %o",
			old_name, st_mode, patch->old_mode);
	if (!patch->new_mode && !patch->is_delete)
		patch->new_mode = st_mode;
	return 0;

 is_new:
	patch->is_new = 1;
	patch->is_delete = 0;
	patch->old_name = NULL;
	return 0;
}

static int check_patch(struct patch *patch)
{
	struct stat st;
	const char *old_name = patch->old_name;
	const char *new_name = patch->new_name;
	const char *name = old_name ? old_name : new_name;
	struct cache_entry *ce = NULL;
	struct patch *tpatch;
	int ok_if_exists;
	int status;

	patch->rejected = 1; /* we will drop this after we succeed */

	status = check_preimage(patch, &ce, &st);
	if (status)
		return status;
	old_name = patch->old_name;

	if ((tpatch = in_fn_table(new_name)) &&
			(was_deleted(tpatch) || to_be_deleted(tpatch)))
		/*
		 * A type-change diff is always split into a patch to
		 * delete old, immediately followed by a patch to
		 * create new (see diff.c::run_diff()); in such a case
		 * it is Ok that the entry to be deleted by the
		 * previous patch is still in the working tree and in
		 * the index.
		 */
		ok_if_exists = 1;
	else
		ok_if_exists = 0;

	if (new_name &&
	    ((0 < patch->is_new) | (0 < patch->is_rename) | patch->is_copy)) {
		if (check_index &&
		    cache_name_pos(new_name, strlen(new_name)) >= 0 &&
		    !ok_if_exists)
			return error("%s: already exists in index", new_name);
		if (!cached) {
			int err = check_to_create_blob(new_name, ok_if_exists);
			if (err)
				return err;
		}
		if (!patch->new_mode) {
			if (0 < patch->is_new)
				patch->new_mode = S_IFREG | 0644;
			else
				patch->new_mode = patch->old_mode;
		}
	}

	if (new_name && old_name) {
		int same = !strcmp(old_name, new_name);
		if (!patch->new_mode)
			patch->new_mode = patch->old_mode;
		if ((patch->old_mode ^ patch->new_mode) & S_IFMT)
			return error("new mode (%o) of %s does not match old mode (%o)%s%s",
				patch->new_mode, new_name, patch->old_mode,
				same ? "" : " of ", same ? "" : old_name);
	}

	if (apply_data(patch, &st, ce) < 0)
		return error("%s: patch does not apply", name);
	patch->rejected = 0;
	return 0;
}

static int check_patch_list(struct patch *patch)
{
	int err = 0;

	prepare_fn_table(patch);
	while (patch) {
		if (apply_verbosely)
			say_patch_name(stderr,
				       "Checking patch ", patch, "...\n");
		err |= check_patch(patch);
		patch = patch->next;
	}
	return err;
}

/* This function tries to read the sha1 from the current index */
static int get_current_sha1(const char *path, unsigned char *sha1)
{
	int pos;

	if (read_cache() < 0)
		return -1;
	pos = cache_name_pos(path, strlen(path));
	if (pos < 0)
		return -1;
	hashcpy(sha1, active_cache[pos]->sha1);
	return 0;
}

/* Build an index that contains the just the files needed for a 3way merge */
static void build_fake_ancestor(struct patch *list, const char *filename)
{
	struct patch *patch;
	struct index_state result = { NULL };
	int fd;

	/* Once we start supporting the reverse patch, it may be
	 * worth showing the new sha1 prefix, but until then...
	 */
	for (patch = list; patch; patch = patch->next) {
		const unsigned char *sha1_ptr;
		unsigned char sha1[20];
		struct cache_entry *ce;
		const char *name;

		name = patch->old_name ? patch->old_name : patch->new_name;
		if (0 < patch->is_new)
			continue;
		else if (get_sha1(patch->old_sha1_prefix, sha1))
			/* git diff has no index line for mode/type changes */
			if (!patch->lines_added && !patch->lines_deleted) {
				if (get_current_sha1(patch->new_name, sha1) ||
				    get_current_sha1(patch->old_name, sha1))
					die("mode change for %s, which is not "
						"in current HEAD", name);
				sha1_ptr = sha1;
			} else
				die("sha1 information is lacking or useless "
					"(%s).", name);
		else
			sha1_ptr = sha1;

		ce = make_cache_entry(patch->old_mode, sha1_ptr, name, 0, 0);
		if (!ce)
			die("make_cache_entry failed for path '%s'", name);
		if (add_index_entry(&result, ce, ADD_CACHE_OK_TO_ADD))
			die ("Could not add %s to temporary index", name);
	}

	fd = open(filename, O_WRONLY | O_CREAT, 0666);
	if (fd < 0 || write_index(&result, fd) || close(fd))
		die ("Could not write temporary index to %s", filename);

	discard_index(&result);
}

static void stat_patch_list(struct patch *patch)
{
	int files, adds, dels;

	for (files = adds = dels = 0 ; patch ; patch = patch->next) {
		files++;
		adds += patch->lines_added;
		dels += patch->lines_deleted;
		show_stats(patch);
	}

	printf(" %d files changed, %d insertions(+), %d deletions(-)\n", files, adds, dels);
}

static void numstat_patch_list(struct patch *patch)
{
	for ( ; patch; patch = patch->next) {
		const char *name;
		name = patch->new_name ? patch->new_name : patch->old_name;
		if (patch->is_binary)
			printf("-\t-\t");
		else
			printf("%d\t%d\t", patch->lines_added, patch->lines_deleted);
		write_name_quoted(name, stdout, line_termination);
	}
}

static void show_file_mode_name(const char *newdelete, unsigned int mode, const char *name)
{
	if (mode)
		printf(" %s mode %06o %s\n", newdelete, mode, name);
	else
		printf(" %s %s\n", newdelete, name);
}

static void show_mode_change(struct patch *p, int show_name)
{
	if (p->old_mode && p->new_mode && p->old_mode != p->new_mode) {
		if (show_name)
			printf(" mode change %06o => %06o %s\n",
			       p->old_mode, p->new_mode, p->new_name);
		else
			printf(" mode change %06o => %06o\n",
			       p->old_mode, p->new_mode);
	}
}

static void show_rename_copy(struct patch *p)
{
	const char *renamecopy = p->is_rename ? "rename" : "copy";
	const char *old, *new;

	/* Find common prefix */
	old = p->old_name;
	new = p->new_name;
	while (1) {
		const char *slash_old, *slash_new;
		slash_old = strchr(old, '/');
		slash_new = strchr(new, '/');
		if (!slash_old ||
		    !slash_new ||
		    slash_old - old != slash_new - new ||
		    memcmp(old, new, slash_new - new))
			break;
		old = slash_old + 1;
		new = slash_new + 1;
	}
	/* p->old_name thru old is the common prefix, and old and new
	 * through the end of names are renames
	 */
	if (old != p->old_name)
		printf(" %s %.*s{%s => %s} (%d%%)\n", renamecopy,
		       (int)(old - p->old_name), p->old_name,
		       old, new, p->score);
	else
		printf(" %s %s => %s (%d%%)\n", renamecopy,
		       p->old_name, p->new_name, p->score);
	show_mode_change(p, 0);
}

static void summary_patch_list(struct patch *patch)
{
	struct patch *p;

	for (p = patch; p; p = p->next) {
		if (p->is_new)
			show_file_mode_name("create", p->new_mode, p->new_name);
		else if (p->is_delete)
			show_file_mode_name("delete", p->old_mode, p->old_name);
		else {
			if (p->is_rename || p->is_copy)
				show_rename_copy(p);
			else {
				if (p->score) {
					printf(" rewrite %s (%d%%)\n",
					       p->new_name, p->score);
					show_mode_change(p, 0);
				}
				else
					show_mode_change(p, 1);
			}
		}
	}
}

static void patch_stats(struct patch *patch)
{
	int lines = patch->lines_added + patch->lines_deleted;

	if (lines > max_change)
		max_change = lines;
	if (patch->old_name) {
		int len = quote_c_style(patch->old_name, NULL, NULL, 0);
		if (!len)
			len = strlen(patch->old_name);
		if (len > max_len)
			max_len = len;
	}
	if (patch->new_name) {
		int len = quote_c_style(patch->new_name, NULL, NULL, 0);
		if (!len)
			len = strlen(patch->new_name);
		if (len > max_len)
			max_len = len;
	}
}

static void remove_file(struct patch *patch, int rmdir_empty)
{
	if (update_index) {
		if (remove_file_from_cache(patch->old_name) < 0)
			die("unable to remove %s from index", patch->old_name);
	}
	if (!cached) {
		if (S_ISGITLINK(patch->old_mode)) {
			if (rmdir(patch->old_name))
				warning("unable to remove submodule %s",
					patch->old_name);
		} else if (!unlink_or_warn(patch->old_name) && rmdir_empty) {
			remove_path(patch->old_name);
		}
	}
}

static void add_index_file(const char *path, unsigned mode, void *buf, unsigned long size)
{
	struct stat st;
	struct cache_entry *ce;
	int namelen = strlen(path);
	unsigned ce_size = cache_entry_size(namelen);

	if (!update_index)
		return;

	ce = xcalloc(1, ce_size);
	memcpy(ce->name, path, namelen);
	ce->ce_mode = create_ce_mode(mode);
	ce->ce_flags = namelen;
	if (S_ISGITLINK(mode)) {
		const char *s = buf;

		if (get_sha1_hex(s + strlen("Subproject commit "), ce->sha1))
			die("corrupt patch for subproject %s", path);
	} else {
		if (!cached) {
			if (lstat(path, &st) < 0)
				die("unable to stat newly created file %s",
				    path);
			fill_stat_cache_info(ce, &st);
		}
		if (write_sha1_file(buf, size, blob_type, ce->sha1) < 0)
			die("unable to create backing store for newly created file %s", path);
	}
	if (add_cache_entry(ce, ADD_CACHE_OK_TO_ADD) < 0)
		die("unable to add cache entry for %s", path);
}

static int try_create_file(const char *path, unsigned int mode, const char *buf, unsigned long size)
{
	int fd;
	struct strbuf nbuf = STRBUF_INIT;

	if (S_ISGITLINK(mode)) {
		struct stat st;
		if (!lstat(path, &st) && S_ISDIR(st.st_mode))
			return 0;
		return mkdir(path, 0777);
	}

	if (has_symlinks && S_ISLNK(mode))
		/* Although buf:size is counted string, it also is NUL
		 * terminated.
		 */
		return symlink(buf, path);

	fd = open(path, O_CREAT | O_EXCL | O_WRONLY, (mode & 0100) ? 0777 : 0666);
	if (fd < 0)
		return -1;

	if (convert_to_working_tree(path, buf, size, &nbuf)) {
		size = nbuf.len;
		buf  = nbuf.buf;
	}
	write_or_die(fd, buf, size);
	strbuf_release(&nbuf);

	if (close(fd) < 0)
		die("closing file %s: %s", path, strerror(errno));
	return 0;
}

/*
 * We optimistically assume that the directories exist,
 * which is true 99% of the time anyway. If they don't,
 * we create them and try again.
 */
static void create_one_file(char *path, unsigned mode, const char *buf, unsigned long size)
{
	if (cached)
		return;
	if (!try_create_file(path, mode, buf, size))
		return;

	if (errno == ENOENT) {
		if (safe_create_leading_directories(path))
			return;
		if (!try_create_file(path, mode, buf, size))
			return;
	}

	if (errno == EEXIST || errno == EACCES) {
		/* We may be trying to create a file where a directory
		 * used to be.
		 */
		struct stat st;
		if (!lstat(path, &st) && (!S_ISDIR(st.st_mode) || !rmdir(path)))
			errno = EEXIST;
	}

	if (errno == EEXIST) {
		unsigned int nr = getpid();

		for (;;) {
			char newpath[PATH_MAX];
			mksnpath(newpath, sizeof(newpath), "%s~%u", path, nr);
			if (!try_create_file(newpath, mode, buf, size)) {
				if (!rename(newpath, path))
					return;
				unlink_or_warn(newpath);
				break;
			}
			if (errno != EEXIST)
				break;
			++nr;
		}
	}
	die("unable to write file %s mode %o", path, mode);
}

static void create_file(struct patch *patch)
{
	char *path = patch->new_name;
	unsigned mode = patch->new_mode;
	unsigned long size = patch->resultsize;
	char *buf = patch->result;

	if (!mode)
		mode = S_IFREG | 0644;
	create_one_file(path, mode, buf, size);
	add_index_file(path, mode, buf, size);
}

/* phase zero is to remove, phase one is to create */
static void write_out_one_result(struct patch *patch, int phase)
{
	if (patch->is_delete > 0) {
		if (phase == 0)
			remove_file(patch, 1);
		return;
	}
	if (patch->is_new > 0 || patch->is_copy) {
		if (phase == 1)
			create_file(patch);
		return;
	}
	/*
	 * Rename or modification boils down to the same
	 * thing: remove the old, write the new
	 */
	if (phase == 0)
		remove_file(patch, patch->is_rename);
	if (phase == 1)
		create_file(patch);
}

static int write_out_one_reject(struct patch *patch)
{
	FILE *rej;
	char namebuf[PATH_MAX];
	struct fragment *frag;
	int cnt = 0;

	for (cnt = 0, frag = patch->fragments; frag; frag = frag->next) {
		if (!frag->rejected)
			continue;
		cnt++;
	}

	if (!cnt) {
		if (apply_verbosely)
			say_patch_name(stderr,
				       "Applied patch ", patch, " cleanly.\n");
		return 0;
	}

	/* This should not happen, because a removal patch that leaves
	 * contents are marked "rejected" at the patch level.
	 */
	if (!patch->new_name)
		die("internal error");

	/* Say this even without --verbose */
	say_patch_name(stderr, "Applying patch ", patch, " with");
	fprintf(stderr, " %d rejects...\n", cnt);

	cnt = strlen(patch->new_name);
	if (ARRAY_SIZE(namebuf) <= cnt + 5) {
		cnt = ARRAY_SIZE(namebuf) - 5;
		warning("truncating .rej filename to %.*s.rej",
			cnt - 1, patch->new_name);
	}
	memcpy(namebuf, patch->new_name, cnt);
	memcpy(namebuf + cnt, ".rej", 5);

	rej = fopen(namebuf, "w");
	if (!rej)
		return error("cannot open %s: %s", namebuf, strerror(errno));

	/* Normal git tools never deal with .rej, so do not pretend
	 * this is a git patch by saying --git nor give extended
	 * headers.  While at it, maybe please "kompare" that wants
	 * the trailing TAB and some garbage at the end of line ;-).
	 */
	fprintf(rej, "diff a/%s b/%s\t(rejected hunks)\n",
		patch->new_name, patch->new_name);
	for (cnt = 1, frag = patch->fragments;
	     frag;
	     cnt++, frag = frag->next) {
		if (!frag->rejected) {
			fprintf(stderr, "Hunk #%d applied cleanly.\n", cnt);
			continue;
		}
		fprintf(stderr, "Rejected hunk #%d.\n", cnt);
		fprintf(rej, "%.*s", frag->size, frag->patch);
		if (frag->patch[frag->size-1] != '\n')
			fputc('\n', rej);
	}
	fclose(rej);
	return -1;
}

static int write_out_results(struct patch *list, int skipped_patch)
{
	int phase;
	int errs = 0;
	struct patch *l;

	if (!list && !skipped_patch)
		return error("No changes");

	for (phase = 0; phase < 2; phase++) {
		l = list;
		while (l) {
			if (l->rejected)
				errs = 1;
			else {
				write_out_one_result(l, phase);
				if (phase == 1 && write_out_one_reject(l))
					errs = 1;
			}
			l = l->next;
		}
	}
	return errs;
}

static struct lock_file lock_file;

static struct string_list limit_by_name;
static int has_include;
static void add_name_limit(const char *name, int exclude)
{
	struct string_list_item *it;

	it = string_list_append(name, &limit_by_name);
	it->util = exclude ? NULL : (void *) 1;
}

static int use_patch(struct patch *p)
{
	const char *pathname = p->new_name ? p->new_name : p->old_name;
	int i;

	/* Paths outside are not touched regardless of "--include" */
	if (0 < prefix_length) {
		int pathlen = strlen(pathname);
		if (pathlen <= prefix_length ||
		    memcmp(prefix, pathname, prefix_length))
			return 0;
	}

	/* See if it matches any of exclude/include rule */
	for (i = 0; i < limit_by_name.nr; i++) {
		struct string_list_item *it = &limit_by_name.items[i];
		if (!fnmatch(it->string, pathname, 0))
			return (it->util != NULL);
	}

	/*
	 * If we had any include, a path that does not match any rule is
	 * not used.  Otherwise, we saw bunch of exclude rules (or none)
	 * and such a path is used.
	 */
	return !has_include;
}


static void prefix_one(char **name)
{
	char *old_name = *name;
	if (!old_name)
		return;
	*name = xstrdup(prefix_filename(prefix, prefix_length, *name));
	free(old_name);
}

static void prefix_patches(struct patch *p)
{
	if (!prefix || p->is_toplevel_relative)
		return;
	for ( ; p; p = p->next) {
		if (p->new_name == p->old_name) {
			char *prefixed = p->new_name;
			prefix_one(&prefixed);
			p->new_name = p->old_name = prefixed;
		}
		else {
			prefix_one(&p->new_name);
			prefix_one(&p->old_name);
		}
	}
}

#define INACCURATE_EOF	(1<<0)
#define RECOUNT		(1<<1)

static int apply_patch(int fd, const char *filename, int options)
{
	size_t offset;
	struct strbuf buf = STRBUF_INIT;
	struct patch *list = NULL, **listp = &list;
	int skipped_patch = 0;

	/* FIXME - memory leak when using multiple patch files as inputs */
	memset(&fn_table, 0, sizeof(struct string_list));
	patch_input_file = filename;
	read_patch_file(&buf, fd);
	offset = 0;
	while (offset < buf.len) {
		struct patch *patch;
		int nr;

		patch = xcalloc(1, sizeof(*patch));
		patch->inaccurate_eof = !!(options & INACCURATE_EOF);
		patch->recount =  !!(options & RECOUNT);
		nr = parse_chunk(buf.buf + offset, buf.len - offset, patch);
		if (nr < 0)
			break;
		if (apply_in_reverse)
			reverse_patches(patch);
		if (prefix)
			prefix_patches(patch);
		if (use_patch(patch)) {
			patch_stats(patch);
			*listp = patch;
			listp = &patch->next;
		}
		else {
			/* perhaps free it a bit better? */
			free(patch);
			skipped_patch++;
		}
		offset += nr;
	}

	if (whitespace_error && (ws_error_action == die_on_ws_error))
		apply = 0;

	update_index = check_index && apply;
	if (update_index && newfd < 0)
		newfd = hold_locked_index(&lock_file, 1);

	if (check_index) {
		if (read_cache() < 0)
			die("unable to read index file");
	}

	if ((check || apply) &&
	    check_patch_list(list) < 0 &&
	    !apply_with_reject)
		exit(1);

	if (apply && write_out_results(list, skipped_patch))
		exit(1);

	if (fake_ancestor)
		build_fake_ancestor(list, fake_ancestor);

	if (diffstat)
		stat_patch_list(list);

	if (numstat)
		numstat_patch_list(list);

	if (summary)
		summary_patch_list(list);

	strbuf_release(&buf);
	return 0;
}

static int git_apply_config(const char *var, const char *value, void *cb)
{
	if (!strcmp(var, "apply.whitespace"))
		return git_config_string(&apply_default_whitespace, var, value);
	return git_default_config(var, value, cb);
}

static int option_parse_exclude(const struct option *opt,
				const char *arg, int unset)
{
	add_name_limit(arg, 1);
	return 0;
}

static int option_parse_include(const struct option *opt,
				const char *arg, int unset)
{
	add_name_limit(arg, 0);
	has_include = 1;
	return 0;
}

static int option_parse_p(const struct option *opt,
			  const char *arg, int unset)
{
	p_value = atoi(arg);
	p_value_known = 1;
	return 0;
}

static int option_parse_z(const struct option *opt,
			  const char *arg, int unset)
{
	if (unset)
		line_termination = '\n';
	else
		line_termination = 0;
	return 0;
}

static int option_parse_whitespace(const struct option *opt,
				   const char *arg, int unset)
{
	const char **whitespace_option = opt->value;

	*whitespace_option = arg;
	parse_whitespace_option(arg);
	return 0;
}

static int option_parse_directory(const struct option *opt,
				  const char *arg, int unset)
{
	root_len = strlen(arg);
	if (root_len && arg[root_len - 1] != '/') {
		char *new_root;
		root = new_root = xmalloc(root_len + 2);
		strcpy(new_root, arg);
		strcpy(new_root + root_len++, "/");
	} else
		root = arg;
	return 0;
}

int cmd_apply(int argc, const char **argv, const char *unused_prefix)
{
	int i;
	int errs = 0;
	int is_not_gitdir;
	int binary;
	int force_apply = 0;

	const char *whitespace_option = NULL;

	struct option builtin_apply_options[] = {
		{ OPTION_CALLBACK, 0, "exclude", NULL, "path",
			"don't apply changes matching the given path",
			0, option_parse_exclude },
		{ OPTION_CALLBACK, 0, "include", NULL, "path",
			"apply changes matching the given path",
			0, option_parse_include },
		{ OPTION_CALLBACK, 'p', NULL, NULL, "num",
			"remove <num> leading slashes from traditional diff paths",
			0, option_parse_p },
		OPT_BOOLEAN(0, "no-add", &no_add,
			"ignore additions made by the patch"),
		OPT_BOOLEAN(0, "stat", &diffstat,
			"instead of applying the patch, output diffstat for the input"),
		{ OPTION_BOOLEAN, 0, "allow-binary-replacement", &binary,
		  NULL, "old option, now no-op",
		  PARSE_OPT_HIDDEN | PARSE_OPT_NOARG },
		{ OPTION_BOOLEAN, 0, "binary", &binary,
		  NULL, "old option, now no-op",
		  PARSE_OPT_HIDDEN | PARSE_OPT_NOARG },
		OPT_BOOLEAN(0, "numstat", &numstat,
			"shows number of added and deleted lines in decimal notation"),
		OPT_BOOLEAN(0, "summary", &summary,
			"instead of applying the patch, output a summary for the input"),
		OPT_BOOLEAN(0, "check", &check,
			"instead of applying the patch, see if the patch is applicable"),
		OPT_BOOLEAN(0, "index", &check_index,
			"make sure the patch is applicable to the current index"),
		OPT_BOOLEAN(0, "cached", &cached,
			"apply a patch without touching the working tree"),
		OPT_BOOLEAN(0, "apply", &force_apply,
			"also apply the patch (use with --stat/--summary/--check)"),
		OPT_FILENAME(0, "build-fake-ancestor", &fake_ancestor,
			"build a temporary index based on embedded index information"),
		{ OPTION_CALLBACK, 'z', NULL, NULL, NULL,
			"paths are separated with NUL character",
			PARSE_OPT_NOARG, option_parse_z },
		OPT_INTEGER('C', NULL, &p_context,
				"ensure at least <n> lines of context match"),
		{ OPTION_CALLBACK, 0, "whitespace", &whitespace_option, "action",
			"detect new or modified lines that have whitespace errors",
			0, option_parse_whitespace },
		OPT_BOOLEAN('R', "reverse", &apply_in_reverse,
			"apply the patch in reverse"),
		OPT_BOOLEAN(0, "unidiff-zero", &unidiff_zero,
			"don't expect at least one line of context"),
		OPT_BOOLEAN(0, "reject", &apply_with_reject,
			"leave the rejected hunks in corresponding *.rej files"),
		OPT__VERBOSE(&apply_verbosely),
		OPT_BIT(0, "inaccurate-eof", &options,
			"tolerate incorrectly detected missing new-line at the end of file",
			INACCURATE_EOF),
		OPT_BIT(0, "recount", &options,
			"do not trust the line counts in the hunk headers",
			RECOUNT),
		{ OPTION_CALLBACK, 0, "directory", NULL, "root",
			"prepend <root> to all filenames",
			0, option_parse_directory },
		OPT_END()
	};

	prefix = setup_git_directory_gently(&is_not_gitdir);
	prefix_length = prefix ? strlen(prefix) : 0;
	git_config(git_apply_config, NULL);
	if (apply_default_whitespace)
		parse_whitespace_option(apply_default_whitespace);

	argc = parse_options(argc, argv, prefix, builtin_apply_options,
			apply_usage, 0);

	if (apply_with_reject)
		apply = apply_verbosely = 1;
	if (!force_apply && (diffstat || numstat || summary || check || fake_ancestor))
		apply = 0;
	if (check_index && is_not_gitdir)
		die("--index outside a repository");
	if (cached) {
		if (is_not_gitdir)
			die("--cached outside a repository");
		check_index = 1;
	}
	for (i = 0; i < argc; i++) {
		const char *arg = argv[i];
		int fd;

		if (!strcmp(arg, "-")) {
			errs |= apply_patch(0, "<stdin>", options);
			read_stdin = 0;
			continue;
		} else if (0 < prefix_length)
			arg = prefix_filename(prefix, prefix_length, arg);

		fd = open(arg, O_RDONLY);
		if (fd < 0)
			die("can't open patch '%s': %s", arg, strerror(errno));
		read_stdin = 0;
		set_default_whitespace_mode(whitespace_option);
		errs |= apply_patch(fd, arg, options);
		close(fd);
	}
	set_default_whitespace_mode(whitespace_option);
	if (read_stdin)
		errs |= apply_patch(0, "<stdin>", options);
	if (whitespace_error) {
		if (squelch_whitespace_errors &&
		    squelch_whitespace_errors < whitespace_error) {
			int squelched =
				whitespace_error - squelch_whitespace_errors;
			warning("squelched %d "
				"whitespace error%s",
				squelched,
				squelched == 1 ? "" : "s");
		}
		if (ws_error_action == die_on_ws_error)
			die("%d line%s add%s whitespace errors.",
			    whitespace_error,
			    whitespace_error == 1 ? "" : "s",
			    whitespace_error == 1 ? "s" : "");
		if (applied_after_fixing_ws && apply)
			warning("%d line%s applied after"
				" fixing whitespace errors.",
				applied_after_fixing_ws,
				applied_after_fixing_ws == 1 ? "" : "s");
		else if (whitespace_error)
			warning("%d line%s add%s whitespace errors.",
				whitespace_error,
				whitespace_error == 1 ? "" : "s",
				whitespace_error == 1 ? "s" : "");
	}

	if (update_index) {
		if (write_cache(newfd, active_cache, active_nr) ||
		    commit_locked_index(&lock_file))
			die("Unable to write new index file");
	}

	return !!errs;
}
