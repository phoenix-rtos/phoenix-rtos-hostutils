/*
 * Phoenix-RTOS
 *
 * psu script parsing
 *
 * Copyright 2020 Phoenix Systems
 *
 * Author: Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "hostutils-common/script.h"

#define LOG_ERROR(...)  fprintf(stderr, __VA_ARGS__)
#define LOG(...)        fprintf(stdout, __VA_ARGS__)

#define IS_SPACE(c)     ((c) == ' '  || (c) == '\t')
#define IS_NEWLINE(c)   ((c) == '\n' || (c) == '\r')
#define IS_DIGIT(c)     ((c) >= '0'  && (c) <= '9' )
#define IS_ALPHA_LOW(c) ((c) >= 'a'  && (c) <= 'z' )
#define IS_ALPHA_UP(c)  ((c) >= 'A'  && (c) <= 'Z' )
#define IS_ALPHA(c)     (IS_ALPHA_LOW(c) || IS_ALPHA_UP(c))
#define IS_QUOTE(c)     ((c) == '\"' || (c) == '\'')


/* initialize parser, and load psu script file */
int script_load(script_t *s, const char *fname)
{
	int fd;
	void *ptr;
	struct stat statbuf = { 0 };

	if ((fd = open(fname, O_RDONLY)) < 0) {
		LOG_ERROR("Unable to open '%s' script file.\n", fname);
		return SCRIPT_ERROR;
	}

	if (fstat(fd, &statbuf) < 0) {
		LOG_ERROR("fstat() failed.\n");
		close(fd);
		return SCRIPT_ERROR;
	}

	if (statbuf.st_size > 1024*100) {
		LOG_ERROR("Script file too big (100kB limit).\n");
		close(fd);
		return SCRIPT_ERROR;
	}

	ptr = mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

	close(fd);

	if (ptr == MAP_FAILED) {
		LOG_ERROR("Unable to allocate memory.\n");
		return SCRIPT_ERROR;
	}

	memset(s, 0, sizeof(*s));
	s->ptr = ptr;
	s->buf.ptr = ptr;
	s->buf.end = ptr + statbuf.st_size;

	return SCRIPT_OK;
}


/* free parser context */
void script_close(script_t *s)
{
	if (s->buf.ptr == NULL || s->buf.ptr == MAP_FAILED)
		return;

	munmap(s->buf.ptr, s->buf.end - s->buf.ptr);
}


static void script_skip_to_space(script_t *s, script_blob_t *line, int allow_ws)
{
	while (line->end < s->buf.end) {
		if (IS_NEWLINE(*line->end) || (allow_ws && IS_SPACE(*line->end)))
			break;

		line->end++;
	}
}


static int script_line_range(script_t *s, char *start_ptr, script_blob_t *line)
{
	if (start_ptr >= s->buf.end) {
		return SCRIPT_ERROR;
	}

	line->ptr = start_ptr;
	line->end = start_ptr;

	script_skip_to_space(s, line, 0);

	return SCRIPT_OK;
}


/* go forward to next non-space character */
static int script_skip_space(script_t *s)
{
	for (; s->ptr < s->buf.end; s->ptr++) {
		if (!IS_SPACE(*s->ptr))
			return SCRIPT_OK;
	}

	return SCRIPT_ERROR;
}


/* convert tokens between quote marks to string type token */
static int script_get_string(script_t *s, script_token_t *token)
{
	char quote = *s->ptr;

	token->str.ptr = ++s->ptr;

	while (s->ptr < s->buf.end) {
		if (*s->ptr == quote)
			break;

		s->ptr++;
	}

	if (!IS_QUOTE(*s->ptr))
		return SCRIPT_ERROR;

	token->str.end = s->ptr++;
	token->typ = script_tok_string;

	return SCRIPT_OK;
}


/* get identifier type token */
static int script_get_identifier(script_t *s, script_token_t *token)
{
	token->str.ptr = s->ptr;

	while (s->ptr < s->buf.end) {
		if (IS_SPACE(*s->ptr) || IS_NEWLINE(*s->ptr))
			break;

		if (!(IS_ALPHA(*s->ptr) || IS_DIGIT(*s->ptr) || *s->ptr == '_'))
			return SCRIPT_ERROR;

		s->ptr++;
	}

	token->str.end = s->ptr;
	token->typ = script_tok_identifier;

	return SCRIPT_OK;
}


/* get integer type token */
static int script_get_integer(script_t *s, script_token_t *token)
{
	token->str.ptr = s->ptr;

	token->num = strtoll(token->str.ptr, &s->ptr, 0);

	if (s->ptr == token->str.ptr || s->ptr > s->buf.end)
		return SCRIPT_ERROR;

	token->str.end = s->ptr;
	token->typ = script_tok_integer;

	return SCRIPT_OK;
}


/* get end-of-line token or all empty lines as token */
static int script_get_lines(script_t *s, script_token_t *token)
{
	char first = *s->ptr;

	token->str.ptr = s->ptr;

	while (s->ptr < s->buf.end) {
		if (!IS_NEWLINE(*s->ptr))
			break;

		if (!(first ^ *s->ptr))
			token->line_no++;

		s->ptr++;
	}

	token->str.end = s->ptr;
	token->typ = script_tok_nl;

	return SCRIPT_OK;
}


/* get comment token */
static int script_get_comment(script_t *s, script_token_t *token)
{
	script_blob_t comment_line;

	if (script_line_range(s, s->ptr, &comment_line) != SCRIPT_OK)
		return SCRIPT_ERROR;

	token->str.ptr = comment_line.ptr;
	token->str.end = s->ptr = comment_line.end;
	token->typ = script_tok_comment;

	return SCRIPT_OK;
}


static int script_get_token(script_t *s)
{
	s->token = s->next;

	if (script_skip_space(s) != SCRIPT_OK) {
		return SCRIPT_ERROR;
	}
	else if (IS_QUOTE(*s->ptr)) {
		return script_get_string(s, &s->next);
	}
	else if (IS_ALPHA(*s->ptr)) {
		return script_get_identifier(s, &s->next);
	}
	else if (*s->ptr == '-' || IS_DIGIT(*s->ptr)) {
		return script_get_integer(s, &s->next);
	}
	else if (IS_NEWLINE(*s->ptr)) {
		return script_get_lines(s, &s->next);
	}
	else if (*s->ptr == '#') {
		return script_get_comment(s, &s->next);
	}

	return SCRIPT_ERROR;
}


/* sets null terminated list of functions used by script */
int script_set_funcs(script_t *s, const script_funct_t *funcs, void *arg)
{
	s->arg = arg;
	s->pfuncs = funcs;
	s->nfuncs = 0;

	while (funcs[s->nfuncs].name)
		s->nfuncs++;

	return SCRIPT_OK;
}


/* pass token of type `typ` */
int script_accept(script_t *s, enum script_token_e typ)
{
	if (s->next.typ == typ) {
		script_get_token(s);

		return SCRIPT_OK;
	}

	return SCRIPT_ERROR;
}


/* expect valid `typ` token */
int script_expect(script_t *s, enum script_token_e typ, const char *errstr)
{
	if (script_accept(s, typ) == SCRIPT_OK)
		return SCRIPT_OK;

	s->errstr = errstr;

	return SCRIPT_ERROR;
}


/* expect optional token, check `typ` if present */
int script_expect_opt(script_t *s, enum script_token_e typ, const char *errstr)
{
	if (s->next.typ == script_tok_nl || s->next.typ == script_tok_comment)
		return SCRIPT_ERROR;

	return script_expect(s, typ, errstr);
}


/* bsearch compare function */
static int _script_parse_cmp(const void *k, const void *e)
{
	const script_t *key = (const script_t *)k;
	const script_funct_t *elt = (const script_funct_t *)e;

	return strncasecmp(key->token.str.ptr, elt->name, key->token.str.end - key->token.str.ptr);
}


/* main loop of the psu script parser */
int script_parse(script_t *s, int flags)
{
	script_funct_t *p;

	s->errstr = NULL;
	s->flags = flags;
	s->ptr = s->buf.ptr;
	s->next.line_no = 1;

	/* start with the first token */
	script_get_token(s);
	s->token = s->next;

	while (s->ptr < s->buf.end) {
		/* find line range */
		script_line_range(s, s->next.str.ptr, &s->line);

		/* skip empty lines if any */
		if (script_accept(s, script_tok_nl) == SCRIPT_OK)
			continue;

		/* skip line comments if any */
		if (script_accept(s, script_tok_comment) == SCRIPT_OK)
			continue;

		/* expect command and run */
		if (script_expect(s, script_tok_identifier, "Unexpected token, commmand identifier was expected") == SCRIPT_OK) {

			if (s->flags & SCRIPT_F_SHOWLINES)
				LOG("\033[93m%.*s\033[0m\033[0K\n", (int)(s->line.end - s->line.ptr), s->line.ptr);

			/* lookup function associated with function */
			if ((p = bsearch(s, s->pfuncs, s->nfuncs, sizeof(*s->pfuncs), _script_parse_cmp))) {
				int res = SCRIPT_OK;

				if (p->cmd_cb && (res = p->cmd_cb(s)) == SCRIPT_OK) {

					if (script_accept(s, script_tok_comment) == SCRIPT_OK)
						continue;

					if (script_expect(s, script_tok_nl, "End of line expected") == SCRIPT_OK) {
						continue;
					}
				}

				if (!s->errstr) {
					if (res != SCRIPT_OK)
						s->errstr = "Command reported error status or execution timed out.";
				}
			}
			else {
				s->errstr = "Unrecognized command";
			}

		}

		if (s->token.typ == script_tok_invalid)
			s->errstr = "Invalid token";

		if (s->errstr) {
			int column = 1;
			script_blob_t token_name = {s->line.ptr, s->line.ptr};

			if (s->next.str.ptr < s->line.end) {
				column = (int)(s->next.str.ptr - s->line.ptr + 1);
				token_name = s->next.str;
			}
			else {
				script_skip_to_space(s, &token_name, 1);
			}

			LOG_ERROR(
				"Error: %s (token: '%.*s', line: %d, column: %d)\n",
				s->errstr,
				(int)(token_name.end - token_name.ptr),
				token_name.ptr,
				s->token.line_no,
				column
			);

			return SCRIPT_ERROR;
		}

	}

	return SCRIPT_OK;
}
