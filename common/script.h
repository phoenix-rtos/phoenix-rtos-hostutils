/*
 * Phoenix-RTOS
 *
 * psu script parser
 *
 * Copyright 2020 Phoenix Systems
 *
 * Author: Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef SCRIPT_INCLUDED
#define SCRIPT_INCLUDED


/* return codes */
#define SCRIPT_OK          0
#define SCRIPT_ERROR       -1

/* flags */
#define SCRIPT_F_DRYRUN    1
#define SCRIPT_F_SHOWLINES 2

#define SCRIPT_BLOB_EMPTY  ((script_blob_t) { NULL })

struct _script_t;


/* token types */
enum script_token_e {
	script_tok_invalid,
	script_tok_identifier,
	script_tok_integer,
	script_tok_string,
	script_tok_comment,
	script_tok_nl,
};


/* data blob, pointer range */
typedef struct _script_blob_t {
	char *ptr, *end;
} script_blob_t;


typedef struct _script_token_t {
	int line_no;             /* current line */
	enum script_token_e typ; /* token type */
	script_blob_t str;       /* current token as string */
	long long int num;       /* current token as number */
} script_token_t;


/* single element of script function table */
typedef struct _script_funct_t {
	const char *name;
	int (*cmd_cb)(struct _script_t *);
} script_funct_t;


/* context of script parser */
typedef struct _script_t {
	int nfuncs;                   /* functions count */
	const script_funct_t *pfuncs; /* functions list sorted lexically for bsearch */
	script_blob_t buf;            /* whole buffer */
	script_token_t token, next;
	script_blob_t line;           /* current line pointer range */
	int flags;                    /* flags */
	char *ptr;                    /* parser pointer in range of 'buf' */
	const char *errstr;           /* error message if any occured */
	void *arg;                    /* user argument */
} script_t;


/* load script */
int script_load(script_t *s, const char *fname);

/* register script functions (functs must be lexicaly sorted) */
int script_set_funcs(script_t *s, const script_funct_t *functs, void *arg);

/* main loop of the script parser */
int script_parse(script_t *s, int flags);

/* free script */
void script_close(script_t *s);


/* accept token of type `typ` */
int script_accept(script_t *s, enum script_token_e typ);

/* expect valid `typ` token */
int script_expect(script_t *s, enum script_token_e typ, const char *errstr);

/* expect optional token, check `typ` if present */
int script_expect_opt(script_t *s, enum script_token_e typ, const char *errstr);


#endif /* end of SCRIPT_INCLUDED */
