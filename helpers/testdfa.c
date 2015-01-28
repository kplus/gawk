/*
 * testdfa.c --- abstracted from gawk.
 */

/* 
 * Copyright (C) 1986, 1988, 1989, 1991-2013 the Free Software Foundation, Inc.
 * 
 * This file is part of GAWK, the GNU implementation of the
 * AWK Programming Language.
 * 
 * GAWK is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * 
 * GAWK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/types.h>
#include <sys/stat.h>


#undef _Noreturn
#define _Noreturn
#define _GL_ATTRIBUTE_PURE
#include "dfa.h"

const char *regexflags2str(int flags);
char *databuf(int fd);
const char * reflags2str(int flagval);
int parse_escape(const char **string_ptr);
char *setup_pattern(const char *pattern, size_t *len);
char casetable[];

reg_syntax_t syn;

struct flagtab {
	int val;
	const char *name;
};

/* usage --- print an error message and die */

void usage(const char *myname)
{
	fprintf(stderr, "usage: %s [-ipt] 'awk-regex' < file\n", myname);
	exit(EXIT_FAILURE);
}

/* main --- parse options, compile and run the regex */

int main(int argc, char **argv)
{
	int c, ret, try_backref;
	struct re_pattern_buffer pat;
	struct re_registers regs;
	struct dfa *dfareg;
	size_t len;
	const char *pattern;
	const char *rerr;
	char *data;
	reg_syntax_t dfa_syn;
	bool ignorecase = false;
	char save;
	size_t count = 0;
	char *place;

	if (argc < 2)
		usage(argv[0]);

	memset(& pat, 0, sizeof(pat));
	memset(& regs, 0, sizeof(regs));

	/* default syntax */
	syn = RE_SYNTAX_GNU_AWK;

	/* parse options, update syntax, ignorecase */
	while ((c = getopt(argc, argv, "pit")) != -1) {
		switch (c) {
		case 'i':
			ignorecase = true;
			break;
		case 'p':
			syn = RE_SYNTAX_POSIX_AWK;
			break;
		case 't':
			syn = RE_SYNTAX_AWK;
			break;
		case '?':
		default:
			usage(argv[0]);
			break;
		}
	}

	if (optind == argc)
		usage(argv[0]);

	pattern = argv[optind];
	len = strlen(pattern);

	setlocale(LC_CTYPE, "");
	setlocale(LC_COLLATE, "");
	setlocale(LC_MESSAGES, "");
	setlocale(LC_NUMERIC, "");
	setlocale(LC_TIME, "");

	printf("Ignorecase: %s\nSyntax: %s\n",
			(ignorecase ? "true" : "false"),
			reflags2str(syn));
	printf("Pattern: /%s/, len = %d\n", pattern, len);

	pattern = setup_pattern(pattern, & len);
	printf("After setup_pattern(), len = %d\n", len);

	pat.fastmap = (char *) malloc(256);
	if (pat.fastmap == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	printf("MB_CUR_MAX = %d\n", (int) MB_CUR_MAX);

	if (ignorecase) {
		if (MB_CUR_MAX > 1) {
			syn |= RE_ICASE;
			pat.translate = NULL;
		} else {
			syn &= ~RE_ICASE;
			pat.translate = (RE_TRANSLATE_TYPE) casetable;
		}
	} else {
		pat.translate = NULL;
		syn &= ~RE_ICASE;
	}


	dfa_syn = syn;
	if (ignorecase)
		dfa_syn |= RE_ICASE;
	dfasyntax(dfa_syn, ignorecase, '\n');
	re_set_syntax(syn);

	if ((rerr = re_compile_pattern(pattern, len, & pat)) != NULL) {
		fprintf(stderr, "%s: %s: cannot compile pattern '%s'\n",
				argv[0], rerr, pattern);
		exit(EXIT_FAILURE);
	}

	/* gack. this must be done *after* re_compile_pattern */
	pat.newline_anchor = false; /* don't get \n in middle of string */

	dfareg = dfaalloc();
	printf("Calling dfacomp(%s, %d, %p, true)\n",
			pattern, (int) len, dfareg);

	dfacomp(pattern, len, dfareg, true);


	data = databuf(STDIN_FILENO);

	/* run the regex matcher */
	ret = re_search(& pat, data, len, 0, len, NULL);
	printf("re_search returned position %d (%s)\n", ret, (ret >= 0) ? "true" : "false");

	/* run the dfa matcher */
	/*
	 * dfa likes to stick a '\n' right after the matched
	 * text.  So we just save and restore the character.
	 */
	save = data[len];
	place = dfaexec(dfareg, data, data+len, true,
				&count, &try_backref);
	data[len] = save;

	if (place == NULL)
		printf("dfaexec returned NULL\n");
	else
		printf("dfaexec returned %d (%.3s)\n", place - data, place);

	/* release storage */
	regfree(& pat);
	if (regs.start)
		free(regs.start);
	if (regs.end)
		free(regs.end);
	dfafree(dfareg);
	free(dfareg);

	return 0;
}

/* genflags2str --- general routine to convert a flag value to a string */

const char *
genflags2str(int flagval, const struct flagtab *tab)
{
	static char buffer[BUFSIZ];
	char *sp;
	int i, space_left, space_needed;

	sp = buffer;
	space_left = BUFSIZ;
	for (i = 0; tab[i].name != NULL; i++) {
		if ((flagval & tab[i].val) != 0) {
			/*
			 * note the trick, we want 1 or 0 for whether we need
			 * the '|' character.
			 */
			space_needed = (strlen(tab[i].name) + (sp != buffer));
			if (space_left <= space_needed) {
				fprintf(stderr, "buffer overflow in genflags2str");
				exit(EXIT_FAILURE);
			}

			if (sp != buffer) {
				*sp++ = '|';
				space_left--;
			}
			strcpy(sp, tab[i].name);
			/* note ordering! */
			space_left -= strlen(sp);
			sp += strlen(sp);
		}
	}

	*sp = '\0';
	return buffer;
}


/* reflags2str --- make a regex flags value readable */

const char *
reflags2str(int flagval)
{
	static const struct flagtab values[] = {
		{ RE_BACKSLASH_ESCAPE_IN_LISTS, "RE_BACKSLASH_ESCAPE_IN_LISTS" },
		{ RE_BK_PLUS_QM, "RE_BK_PLUS_QM" },
		{ RE_CHAR_CLASSES, "RE_CHAR_CLASSES" },
		{ RE_CONTEXT_INDEP_ANCHORS, "RE_CONTEXT_INDEP_ANCHORS" },
		{ RE_CONTEXT_INDEP_OPS, "RE_CONTEXT_INDEP_OPS" },
		{ RE_CONTEXT_INVALID_OPS, "RE_CONTEXT_INVALID_OPS" },
		{ RE_DOT_NEWLINE, "RE_DOT_NEWLINE" },
		{ RE_DOT_NOT_NULL, "RE_DOT_NOT_NULL" },
		{ RE_HAT_LISTS_NOT_NEWLINE, "RE_HAT_LISTS_NOT_NEWLINE" },
		{ RE_INTERVALS, "RE_INTERVALS" },
		{ RE_LIMITED_OPS, "RE_LIMITED_OPS" },
		{ RE_NEWLINE_ALT, "RE_NEWLINE_ALT" },
		{ RE_NO_BK_BRACES, "RE_NO_BK_BRACES" },
		{ RE_NO_BK_PARENS, "RE_NO_BK_PARENS" },
		{ RE_NO_BK_REFS, "RE_NO_BK_REFS" },
		{ RE_NO_BK_VBAR, "RE_NO_BK_VBAR" },
		{ RE_NO_EMPTY_RANGES, "RE_NO_EMPTY_RANGES" },
		{ RE_UNMATCHED_RIGHT_PAREN_ORD, "RE_UNMATCHED_RIGHT_PAREN_ORD" },
		{ RE_NO_POSIX_BACKTRACKING, "RE_NO_POSIX_BACKTRACKING" },
		{ RE_NO_GNU_OPS, "RE_NO_GNU_OPS" },
		{ RE_INVALID_INTERVAL_ORD, "RE_INVALID_INTERVAL_ORD" },
		{ RE_ICASE, "RE_ICASE" },
		{ RE_CARET_ANCHORS_HERE, "RE_CARET_ANCHORS_HERE" },
		{ RE_CONTEXT_INVALID_DUP, "RE_CONTEXT_INVALID_DUP" },
		{ RE_NO_SUB, "RE_NO_SUB" },
		{ 0,	NULL },
	};

	if (flagval == RE_SYNTAX_EMACS) /* == 0 */
		return "RE_SYNTAX_EMACS";

	return genflags2str(flagval, values);
}

/*
 * dfawarn() is called by the dfa routines whenever a regex is compiled
 * must supply a dfawarn.
 */

void
dfawarn(const char *dfa_warning)
{
	fprintf(stderr, "dfa warning: %s\n", dfa_warning);
}

/* dfaerror --- print an error message for the dfa routines */

void
dfaerror(const char *s)
{
	fprintf(stderr, "dfa-error: %s\n", s);
	exit(EXIT_FAILURE);
}

/* databuf --- read the input file */

char *
databuf(int fd)
{
	char *buf;
	struct stat sbuf;
	ssize_t count;

	if (fstat(fd, & sbuf) < 0)
		return NULL;

	buf = (char *) malloc(sbuf.st_size + 3);
	if (buf == NULL)
		return NULL;

	if ((count = read(fd, buf, sbuf.st_size)) != sbuf.st_size) {
		perror("read");
		return NULL;
	}
	buf[sbuf.st_size] = '\0';

	(void) close(fd);

	return buf;
}

/* xmalloc --- for dfa.c */

void *
xmalloc(size_t bytes)
{
	void *p = malloc(bytes);

	if (p == NULL) {
		fprintf(stderr, "xmalloc: malloc failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	return p;
}

/* r_fatal --- print a fatal error message. also for dfa.c */

void
r_fatal(const char *mesg, ...)
{
	va_list args;
	va_start(args, mesg);
	fprintf(stderr, "fatal: ");
	vfprintf(stderr, mesg, args);
	va_end(args);

	exit(EXIT_FAILURE);
}

/* setup_pattern --- do what gawk does with the pattern string */

char *
setup_pattern(const char *pattern, size_t *len)
{
	size_t is_multibyte = 0;
	int c, c2;
	size_t buflen = 0;
	mbstate_t mbs;
	bool has_anchor = false;
	char *buf = NULL;
	char *dest;
	const char *src, *end;

	memset(& mbs, 0, sizeof(mbs));

	src = pattern;
	end = pattern + *len;

	/* Handle escaped characters first. */

	/*
	 * Build a copy of the string (in buf) with the
	 * escaped characters translated, and generate the regex
	 * from that. 
	 */
	if (buf == NULL) {
		buf = (char *) malloc(*len + 2);
		if (buf == NULL) {
			fprintf(stderr, "%s: malloc failed\n", __func__);
			exit(EXIT_FAILURE);
		}
		buflen = *len;
	} else if (*len > buflen) {
		buf = (char *) realloc(buf, *len + 2);
		if (buf == NULL) {
			fprintf(stderr, "%s: realloc failed\n", __func__);
			exit(EXIT_FAILURE);
		}
		buflen = *len;
	}
	dest = buf;

	while (src < end) {
		if (MB_CUR_MAX > 1 && ! is_multibyte) {
			/* The previous byte is a singlebyte character, or last byte
			   of a multibyte character.  We check the next character.  */
			is_multibyte = mbrlen(src, end - src, &mbs);
			if (   is_multibyte == 1
			    || is_multibyte == (size_t) -1
			    || is_multibyte == (size_t) -2
			    || is_multibyte == 0) {
				/* We treat it as a single-byte character.  */
				is_multibyte = 0;
			}
		}

		/* We skip multibyte character, since it must not be a special
		   character.  */
		if ((MB_CUR_MAX == 1 || ! is_multibyte) &&
		    (*src == '\\')) {
			c = *++src;
			switch (c) {
			case 'a':
			case 'b':
			case 'f':
			case 'n':
			case 'r':
			case 't':
			case 'v':
			case 'x':
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
				c2 = parse_escape(&src);
				if (c2 < 0) {
					fprintf(stderr, "%s: parse_escape failed\n", __func__);
					exit(EXIT_FAILURE);
				}
				/*
				 * Unix awk treats octal (and hex?) chars
				 * literally in re's, so escape regexp
				 * metacharacters.
				 */
				if (syn == RE_SYNTAX_AWK
				    && (isdigit(c) || c == 'x')
				    && strchr("()|*+?.^$\\[]", c2) != NULL)
					*dest++ = '\\';
				*dest++ = (char) c2;
				break;
			case '8':
			case '9':	/* a\9b not valid */
				*dest++ = c;
				src++;
				break;
			case 'y':	/* normally \b */
				/* gnu regex op */
				if (syn == RE_SYNTAX_GNU_AWK) {
					*dest++ = '\\';
					*dest++ = 'b';
					src++;
					break;
				}
				/* else, fall through */
			default:
				*dest++ = '\\';
				*dest++ = (char) c;
				src++;
				break;
			} /* switch */
		} else {
			c = *src;
			if (c == '^' || c == '$')
				has_anchor = true;

			*dest++ = *src++;	/* not '\\' */
		}
		if (MB_CUR_MAX > 1 && is_multibyte)
			is_multibyte--;
	} /* while */

	*dest = '\0';
	*len = dest - buf;

	return buf;
}

/*
 * parse_escape:
 *
 * Parse a C escape sequence.  STRING_PTR points to a variable containing a
 * pointer to the string to parse.  That pointer is updated past the
 * characters we use.  The value of the escape sequence is returned. 
 *
 * A negative value means the sequence \ newline was seen, which is supposed to
 * be equivalent to nothing at all. 
 *
 * If \ is followed by a null character, we return a negative value and leave
 * the string pointer pointing at the null character. 
 *
 * If \ is followed by 000, we return 0 and leave the string pointer after the
 * zeros.  A value of 0 does not mean end of string.  
 *
 * POSIX doesn't allow \x.
 */

int
parse_escape(const char **string_ptr)
{
	int c = *(*string_ptr)++;
	int i;
	int count;
	int j;
	const char *start;

	switch (c) {
	case 'a':
		return '\a';
	case 'b':
		return '\b';
	case 'f':
		return '\f';
	case 'n':
		return '\n';
	case 'r':
		return '\r';
	case 't':
		return '\t';
	case 'v':
		return '\v';
	case '\n':
		return -2;
	case 0:
		(*string_ptr)--;
		return -1;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
		i = c - '0';
		count = 0;
		while (++count < 3) {
			if ((c = *(*string_ptr)++) >= '0' && c <= '7') {
				i *= 8;
				i += c - '0';
			} else {
				(*string_ptr)--;
				break;
			}
		}
		return i;
	case 'x':
		if (! isxdigit((unsigned char) (*string_ptr)[0])) {
			return ('x');
		}
		i = j = 0;
		start = *string_ptr;
		for (;; j++) {
			/* do outside test to avoid multiple side effects */
			c = *(*string_ptr)++;
			if (isxdigit(c)) {
				i *= 16;
				if (isdigit(c))
					i += c - '0';
				else if (isupper(c))
					i += c - 'A' + 10;
				else
					i += c - 'a' + 10;
			} else {
				(*string_ptr)--;
				break;
			}
		}
		return i;
	case '\\':
	case '"':
		return c;
	default:
		return c;
	}
}

/* This rather ugly macro is for VMS C */
#ifdef C
#undef C
#endif
#define C(c) ((char)c)  
/*
 * This table is used by the regexp routines to do case independent
 * matching. Basically, every ascii character maps to itself, except
 * uppercase letters map to lower case ones. This table has 256
 * entries, for ISO 8859-1. Note also that if the system this
 * is compiled on doesn't use 7-bit ascii, casetable[] should not be
 * defined to the linker, so gawk should not load.
 *
 * Do NOT make this array static, it is used in several spots, not
 * just in this file.
 *
 * 6/2004:
 * This table is also used for IGNORECASE for == and !=, and index().
 * Although with GLIBC, we could use tolower() everywhere and RE_ICASE
 * for the regex matcher, precomputing this table once gives us a
 * performance improvement.  I also think it's better for portability
 * to non-GLIBC systems.  All the world is not (yet :-) GNU/Linux.
 */
#if 'a' == 97	/* it's ascii */
char casetable[] = {
	'\000', '\001', '\002', '\003', '\004', '\005', '\006', '\007',
	'\010', '\011', '\012', '\013', '\014', '\015', '\016', '\017',
	'\020', '\021', '\022', '\023', '\024', '\025', '\026', '\027',
	'\030', '\031', '\032', '\033', '\034', '\035', '\036', '\037',
	/* ' '     '!'     '"'     '#'     '$'     '%'     '&'     ''' */
	'\040', '\041', '\042', '\043', '\044', '\045', '\046', '\047',
	/* '('     ')'     '*'     '+'     ','     '-'     '.'     '/' */
	'\050', '\051', '\052', '\053', '\054', '\055', '\056', '\057',
	/* '0'     '1'     '2'     '3'     '4'     '5'     '6'     '7' */
	'\060', '\061', '\062', '\063', '\064', '\065', '\066', '\067',
	/* '8'     '9'     ':'     ';'     '<'     '='     '>'     '?' */
	'\070', '\071', '\072', '\073', '\074', '\075', '\076', '\077',
	/* '@'     'A'     'B'     'C'     'D'     'E'     'F'     'G' */
	'\100', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
	/* 'H'     'I'     'J'     'K'     'L'     'M'     'N'     'O' */
	'\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
	/* 'P'     'Q'     'R'     'S'     'T'     'U'     'V'     'W' */
	'\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
	/* 'X'     'Y'     'Z'     '['     '\'     ']'     '^'     '_' */
	'\170', '\171', '\172', '\133', '\134', '\135', '\136', '\137',
	/* '`'     'a'     'b'     'c'     'd'     'e'     'f'     'g' */
	'\140', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
	/* 'h'     'i'     'j'     'k'     'l'     'm'     'n'     'o' */
	'\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
	/* 'p'     'q'     'r'     's'     't'     'u'     'v'     'w' */
	'\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
	/* 'x'     'y'     'z'     '{'     '|'     '}'     '~' */
	'\170', '\171', '\172', '\173', '\174', '\175', '\176', '\177',

	/* Latin 1: */
	C('\200'), C('\201'), C('\202'), C('\203'), C('\204'), C('\205'), C('\206'), C('\207'),
	C('\210'), C('\211'), C('\212'), C('\213'), C('\214'), C('\215'), C('\216'), C('\217'),
	C('\220'), C('\221'), C('\222'), C('\223'), C('\224'), C('\225'), C('\226'), C('\227'),
	C('\230'), C('\231'), C('\232'), C('\233'), C('\234'), C('\235'), C('\236'), C('\237'),
	C('\240'), C('\241'), C('\242'), C('\243'), C('\244'), C('\245'), C('\246'), C('\247'),
	C('\250'), C('\251'), C('\252'), C('\253'), C('\254'), C('\255'), C('\256'), C('\257'),
	C('\260'), C('\261'), C('\262'), C('\263'), C('\264'), C('\265'), C('\266'), C('\267'),
	C('\270'), C('\271'), C('\272'), C('\273'), C('\274'), C('\275'), C('\276'), C('\277'),
	C('\340'), C('\341'), C('\342'), C('\343'), C('\344'), C('\345'), C('\346'), C('\347'),
	C('\350'), C('\351'), C('\352'), C('\353'), C('\354'), C('\355'), C('\356'), C('\357'),
	C('\360'), C('\361'), C('\362'), C('\363'), C('\364'), C('\365'), C('\366'), C('\327'),
	C('\370'), C('\371'), C('\372'), C('\373'), C('\374'), C('\375'), C('\376'), C('\337'),
	C('\340'), C('\341'), C('\342'), C('\343'), C('\344'), C('\345'), C('\346'), C('\347'),
	C('\350'), C('\351'), C('\352'), C('\353'), C('\354'), C('\355'), C('\356'), C('\357'),
	C('\360'), C('\361'), C('\362'), C('\363'), C('\364'), C('\365'), C('\366'), C('\367'),
	C('\370'), C('\371'), C('\372'), C('\373'), C('\374'), C('\375'), C('\376'), C('\377'),
};
#elif 'a' == 0x81 /* it's EBCDIC */
char casetable[] = {
 /*00  NU    SH    SX    EX    PF    HT    LC    DL */
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
 /*08              SM    VT    FF    CR    SO    SI */
      0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
 /*10  DE    D1    D2    TM    RS    NL    BS    IL */
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
 /*18  CN    EM    CC    C1    FS    GS    RS    US */
      0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
 /*20  DS    SS    FS          BP    LF    EB    EC */
      0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
 /*28              SM    C2    EQ    AK    BL       */
      0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
 /*30              SY          PN    RS    UC    ET */
      0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
 /*38                    C3    D4    NK          SU */
      0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
 /*40  SP                                           */
      0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
 /*48             CENT    .     <     (     +     | */
      0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
 /*50   &                                           */
      0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
 /*58               !     $     *     )     ;     ^ */
      0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
 /*60   -     /                                     */
      0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
 /*68               |     ,     %     _     >     ? */
      0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
 /*70                                               */
      0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
 /*78         `     :     #     @     '     =     " */
      0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
 /*80         a     b     c     d     e     f     g */
      0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
 /*88   h     i           {                         */
      0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
 /*90         j     k     l     m     n     o     p */
      0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
 /*98   q     r           }                         */
      0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
 /*A0         ~     s     t     u     v     w     x */
      0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
 /*A8   y     z                       [             */
      0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
 /*B0                                               */
      0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
 /*B8                                 ]             */
      0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
 /*C0   {     A     B     C     D     E     F     G */
      0xC0, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
 /*C8   H     I                                     */
      0x88, 0x89, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
 /*D0   }     J     K     L     M     N     O     P */
      0xD0, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
 /*D8   Q     R                                     */
      0x98, 0x99, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
 /*E0   \           S     T     U     V     W     X */
      0xE0, 0xE1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
 /*E8   Y     Z                                     */
      0xA8, 0xA9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
 /*F0   0     1     2     3     4     5     6     7 */
      0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
 /*F8   8     9                                     */
      0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
};
#else
#include "You lose. You will need a translation table for your character set."
#endif

#undef C

#ifdef GREP_DFA	/* not needed for gawk */
/* xalloc.h -- malloc with out-of-memory checking

   Copyright (C) 1990, 1991, 1992, 1993, 1994, 1995, 1996, 1997, 1998, 1999,
   2000, 2003, 2004, 2006, 2007, 2008, 2009, 2010 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef XALLOC_H_
# define XALLOC_H_

# include <stddef.h>


# ifdef __cplusplus
extern "C" {
# endif


# ifndef __attribute__
#  if __GNUC__ < 2 || (__GNUC__ == 2 && __GNUC_MINOR__ < 8)
#   define __attribute__(x)
#  endif
# endif

# ifndef ATTRIBUTE_NORETURN
#  define ATTRIBUTE_NORETURN __attribute__ ((__noreturn__))
# endif

# ifndef ATTRIBUTE_MALLOC
#  if __GNUC__ >= 3
#   define ATTRIBUTE_MALLOC __attribute__ ((__malloc__))
#  else
#   define ATTRIBUTE_MALLOC
#  endif
# endif

/* This function is always triggered when memory is exhausted.
   It must be defined by the application, either explicitly
   or by using gnulib's xalloc-die module.  This is the
   function to call when one wants the program to die because of a
   memory allocation failure.  */
extern void xalloc_die (void);

void *xmalloc (size_t s) ATTRIBUTE_MALLOC;
void *xzalloc (size_t s) ATTRIBUTE_MALLOC;
void *xcalloc (size_t n, size_t s) ATTRIBUTE_MALLOC;
void *xrealloc (void *p, size_t s);
void *x2realloc (void *p, size_t *pn);
void *xmemdup (void const *p, size_t s) ATTRIBUTE_MALLOC;
char *xstrdup (char const *str) ATTRIBUTE_MALLOC;

/* Return 1 if an array of N objects, each of size S, cannot exist due
   to size arithmetic overflow.  S must be positive and N must be
   nonnegative.  This is a macro, not an inline function, so that it
   works correctly even when SIZE_MAX < N.

   By gnulib convention, SIZE_MAX represents overflow in size
   calculations, so the conservative dividend to use here is
   SIZE_MAX - 1, since SIZE_MAX might represent an overflowed value.
   However, malloc (SIZE_MAX) fails on all known hosts where
   sizeof (ptrdiff_t) <= sizeof (size_t), so do not bother to test for
   exactly-SIZE_MAX allocations on such hosts; this avoids a test and
   branch when S is known to be 1.  */
# define xalloc_oversized(n, s) \
    ((size_t) (sizeof (ptrdiff_t) <= sizeof (size_t) ? -1 : -2) / (s) < (n))


/* In the following macros, T must be an elementary or structure/union or
   typedef'ed type, or a pointer to such a type.  To apply one of the
   following macros to a function pointer or array type, you need to typedef
   it first and use the typedef name.  */

/* Allocate an object of type T dynamically, with error checking.  */
/* extern t *XMALLOC (typename t); */
# define XMALLOC(t) ((t *) xmalloc (sizeof (t)))

/* Allocate memory for N elements of type T, with error checking.  */
/* extern t *XNMALLOC (size_t n, typename t); */
# define XNMALLOC(n, t) \
    ((t *) (sizeof (t) == 1 ? xmalloc (n) : xnmalloc (n, sizeof (t))))

/* Allocate an object of type T dynamically, with error checking,
   and zero it.  */
/* extern t *XZALLOC (typename t); */
# define XZALLOC(t) ((t *) xzalloc (sizeof (t)))

/* Allocate memory for N elements of type T, with error checking,
   and zero it.  */
/* extern t *XCALLOC (size_t n, typename t); */
# define XCALLOC(n, t) \
    ((t *) (sizeof (t) == 1 ? xzalloc (n) : xcalloc (n, sizeof (t))))

/*
 * Gawk uses this file only to keep dfa.c happy.
 * We're therefore safe in manually defining HAVE_INLINE to
 * make the !@#$%^&*() thing just work.
 */
#ifdef GAWK
#define HAVE_INLINE	1	/* so there. nyah, nyah, nyah. */
#endif

# if HAVE_INLINE
#  define static_inline static inline
# else
void *xnmalloc (size_t n, size_t s) ATTRIBUTE_MALLOC;
void *xnrealloc (void *p, size_t n, size_t s);
void *x2nrealloc (void *p, size_t *pn, size_t s);
char *xcharalloc (size_t n) ATTRIBUTE_MALLOC;
# endif


/* Allocate an array of N objects, each with S bytes of memory,
   dynamically, with error checking.  S must be nonzero.  */

void *
xnmalloc (size_t n, size_t s)
{
  if (xalloc_oversized (n, s))
    xalloc_die ();
  return xmalloc (n * s);
}

/* Allocate an array of N objects, each with S bytes of memory,
   dynamically, with error checking.  S must be nonzero.
   Clear the contents afterwards.  */

void *
xcalloc(size_t nmemb, size_t size)
{
  void *p = xmalloc (nmemb * size);
  memset(p, '\0', nmemb * size);
  return p;
}

/* Reallocate a pointer to a new size, with error checking. */

void *
xrealloc(void *p, size_t size)
{
   void *new_p = realloc(p, size);
   if (new_p ==  0)
     xalloc_die ();

   return new_p;
}

/* xalloc_die --- fatal error message when malloc fails, needed by dfa.c */

void
xalloc_die (void)
{
	r_fatal("xalloc: malloc failed: %s"), strerror(errno);
}

/* Clone an object P of size S, with error checking.  There's no need
   for xnmemdup (P, N, S), since xmemdup (P, N * S) works without any
   need for an arithmetic overflow check.  */

void *
xmemdup (void const *p, size_t s)
{
  return memcpy (xmalloc (s), p, s);
}

/* Change the size of an allocated block of memory P to an array of N
   objects each of S bytes, with error checking.  S must be nonzero.  */

void *
xnrealloc (void *p, size_t n, size_t s)
{
  if (xalloc_oversized (n, s))
    xalloc_die ();
  return xrealloc (p, n * s);
}

/* If P is null, allocate a block of at least *PN such objects;
   otherwise, reallocate P so that it contains more than *PN objects
   each of S bytes.  *PN must be nonzero unless P is null, and S must
   be nonzero.  Set *PN to the new number of objects, and return the
   pointer to the new block.  *PN is never set to zero, and the
   returned pointer is never null.

   Repeated reallocations are guaranteed to make progress, either by
   allocating an initial block with a nonzero size, or by allocating a
   larger block.

   In the following implementation, nonzero sizes are increased by a
   factor of approximately 1.5 so that repeated reallocations have
   O(N) overall cost rather than O(N**2) cost, but the
   specification for this function does not guarantee that rate.

   Here is an example of use:

     int *p = NULL;
     size_t used = 0;
     size_t allocated = 0;

     void
     append_int (int value)
       {
         if (used == allocated)
           p = x2nrealloc (p, &allocated, sizeof *p);
         p[used++] = value;
       }

   This causes x2nrealloc to allocate a block of some nonzero size the
   first time it is called.

   To have finer-grained control over the initial size, set *PN to a
   nonzero value before calling this function with P == NULL.  For
   example:

     int *p = NULL;
     size_t used = 0;
     size_t allocated = 0;
     size_t allocated1 = 1000;

     void
     append_int (int value)
       {
         if (used == allocated)
           {
             p = x2nrealloc (p, &allocated1, sizeof *p);
             allocated = allocated1;
           }
         p[used++] = value;
       }

   */

void *
x2nrealloc (void *p, size_t *pn, size_t s)
{
  size_t n = *pn;

  if (! p)
    {
      if (! n)
        {
          /* The approximate size to use for initial small allocation
             requests, when the invoking code specifies an old size of
             zero.  64 bytes is the largest "small" request for the
             GNU C library malloc.  */
          enum { DEFAULT_MXFAST = 64 };

          n = DEFAULT_MXFAST / s;
          n += !n;
        }
    }
  else
    {
      /* Set N = ceil (1.5 * N) so that progress is made if N == 1.
         Check for overflow, so that N * S stays in size_t range.
         The check is slightly conservative, but an exact check isn't
         worth the trouble.  */
      if ((size_t) -1 / 3 * 2 / s <= n)
        xalloc_die ();
      n += (n + 1) / 2;
    }

  *pn = n;
  return xrealloc (p, n * s);
}

/* Return a pointer to a new buffer of N bytes.  This is like xmalloc,
   except it returns char *.  */

char *
xcharalloc (size_t n)
{
  return XNMALLOC (n, char);
}

/* Allocate S bytes of zeroed memory dynamically, with error checking.
   There's no need for xnzalloc (N, S), since it would be equivalent
   to xcalloc (N, S).  */

void *
xzalloc (size_t s)
{
  return memset (xmalloc (s), 0, s);
}

# endif

# ifdef __cplusplus
}

/* C++ does not allow conversions from void * to other pointer types
   without a cast.  Use templates to work around the problem when
   possible.  */

template <typename T> inline T *
xrealloc (T *p, size_t s)
{
  return (T *) xrealloc ((void *) p, s);
}

template <typename T> inline T *
xnrealloc (T *p, size_t n, size_t s)
{
  return (T *) xnrealloc ((void *) p, n, s);
}

template <typename T> inline T *
x2realloc (T *p, size_t *pn)
{
  return (T *) x2realloc ((void *) p, pn);
}

template <typename T> inline T *
x2nrealloc (T *p, size_t *pn, size_t s)
{
  return (T *) x2nrealloc ((void *) p, pn, s);
}

template <typename T> inline T *
xmemdup (T const *p, size_t s)
{
  return (T *) xmemdup ((void const *) p, s);
}



#endif /* !XALLOC_H_ */
#endif /* GREP_DFA */
