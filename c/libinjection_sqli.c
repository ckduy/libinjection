/**
 * Copyright 2012,2013  Nick Galbreath
 * nickg@client9.com
 * BSD License -- see COPYING.txt for details
 *
 * (setq-default indent-tabs-mode nil)
 * (setq c-default-style "k&r"
 *     c-basic-offset 4)
 *  indent -kr -nut
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#if 0
#define FOLD_DEBUG printf("%d \t more=%d  pos=%d left=%d\n", __LINE__, more, (int)pos, (int)left);
#else
#define FOLD_DEBUG
#endif

#include "libinjection_sqli_data.h"

/*
 * not making public just yet
 */
typedef enum {
    TYPE_NONE        = 0,
    TYPE_KEYWORD     = (int)'k',
    TYPE_UNION       = (int)'U',
    TYPE_EXPRESSION  = (int)'E',
    TYPE_SQLTYPE     = (int)'t',
    TYPE_FUNCTION    = (int)'f',
    TYPE_BAREWORD    = (int)'n',
    TYPE_NUMBER      = (int)'1',
    TYPE_VARIABLE    = (int)'v',
    TYPE_STRING      = (int)'s',
    TYPE_OPERATOR    = (int)'o',
    TYPE_LOGIC_OPERATOR = (int)'&',
    TYPE_COMMENT     = (int)'c',
    TYPE_LEFTPARENS  = (int)'(',
    TYPE_RIGHTPARENS = (int)')',  /* not used? */
    TYPE_COMMA       = (int)',',
    TYPE_COLON       = (int)':',
    TYPE_UNKNOWN     = (int)'?',
    TYPE_FINGERPRINT = (int)'X',
} sqli_token_types;

/* memchr2 finds a string of 2 characters inside another string
 * This a specialized version of "memmem" or "memchr".
 * 'memmem' doesn't exist on all platforms
 *
 * Porting notes: this is just a special version of
 *    astring.find("AB")
 *
 */
static const char *
memchr2(const char *haystack, size_t haystack_len, char c0, char c1)
{
    const char *cur = haystack;
    const char *last = haystack + haystack_len - 1;

    if (haystack_len < 2) {
        return NULL;
    }

    while (cur < last) {
        if (cur[0] == c0) {
            if (cur[1] == c1) {
                return cur;
            } else {
                cur += 2; //(c0 == c1) ? 1 : 2;
            }
        } else {
            cur += 1;
        }
    }

    return NULL;
}

/**
 */
static const char *
my_memmem(const char* haystack, size_t hlen, const char* needle, size_t nlen)
{
    assert(haystack);
    assert(needle);
    assert(nlen > 1);
    const char* cur;
    const char* last =  haystack + hlen - nlen;
    for (cur = haystack; cur <= last; ++cur) {
        if (cur[0] == needle[0] && memcmp(cur, needle, nlen) == 0) {
            return cur;
        }
    }
    return NULL;
}

/** Find largest string containing certain characters.
 *
 * C Standard library 'strspn' only works for 'c-strings' (null terminated)
 * This works on arbitrary length.
 *
 * Performance notes:
 *   not critical
 *
 * Porting notes:
 *   if accept is 'ABC', then this function would be similar to
 *   a_regexp.match(a_str, '[ABC]*'),
 */
static size_t
strlenspn(const char *s, size_t len, const char *accept)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        /* likely we can do better by inlining this function
         * but this works for now
         */
        if (strchr(accept, s[i]) == NULL) {
            return i;
        }
    }
    return len;
}

static size_t
strlencspn(const char *s, size_t len, const char *accept)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        /* likely we can do better by inlining this function
         * but this works for now
         */
        if (strchr(accept, s[i]) != NULL) {
            return i;
        }
    }
    return len;
}
static int char_is_white(char ch) {
    /* ' '  space is 0x32
       '\t  0x09 \011 horizontal tab
       '\n' 0x0a \012 new line
       '\v' 0x0b \013 verical tab
       '\f' 0x0c \014 new page
       '\r' 0x0d \015 carriage return
            0xa0 \240 is latin1
    */
    return strchr(" \t\n\v\f\r\240", ch) != NULL;
}

/* DANGER DANGER
 * This is -very specialized function-
 *
 * this compares a ALL_UPPER CASE C STRING
 * with a *arbitrary memory* + length
 *
 * Sane people would just make a copy, up-case
 * and use a hash table.
 *
 * Required since libc version uses the current locale
 * and is much slower.
 */
static int cstrcasecmp(const char *a, const char *b, size_t n)
{
    char cb;

    for (; n > 0; a++, b++, n--) {
        cb = *b;
        if (cb >= 'a' && cb <= 'z') {
            cb -= 0x20;
        }
        if (*a != cb) {
            return *a - cb;
        } else if (*a == '\0') {
            return -1;
        }
    }
    //printf("off the edge\n");
    return (*a == 0) ? 0 : 1;
}

/**
 * Case sensitive string compare.
 *  Here only to make code more readable
 */
static int streq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/**
 *
 *
 *
 * Porting Notes:
 *  given a mapping/hash of string to char
 *  this is just
 *    typecode = mapping[key.upper()]
 */

static char bsearch_keyword_type(const char *key, size_t len,
                                 const keyword_t * keywords, size_t numb)
{
    size_t pos;
    size_t left = 0;
    size_t right = numb - 1;

    while (left < right) {
        pos = (left + right) >> 1;

        /* arg0 = upper case only, arg1 = mixed case */
        if (cstrcasecmp(keywords[pos].word, key, len) < 0) {
            left = pos + 1;
        } else {
            right = pos;
        }
    }
    if ((left == right) && cstrcasecmp(keywords[left].word, key, len) == 0) {
        return keywords[left].type;
    } else {
        return CHAR_NULL;
    }
}

static char is_keyword(const char* key, size_t len)
{
    return bsearch_keyword_type(key, len, sql_keywords, sql_keywords_sz);
}

/* st_token methods
 *
 * The following functions manipulates the stoken_t type
 *
 *
 */

static void st_clear(stoken_t * st)
{
    memset(st, 0, sizeof(stoken_t));
}

static void st_assign_char(stoken_t * st, const char stype, size_t pos, size_t len,
                           const char value)
{
    st->type = (char) stype;
    st->pos = pos;
    st->len = len;
    st->val[0] = value;
    st->val[1] = CHAR_NULL;
}

static void st_assign(stoken_t * st, const char stype,
                      size_t pos, size_t len, const char* value)
{
    size_t last = len < ST_MAX_SIZE ? len : (ST_MAX_SIZE - 1);
    st->type = (char) stype;
    st->pos = pos;
    st->len = len;
    memcpy(st->val, value, last);
    st->val[last] = CHAR_NULL;
}

static void st_copy(stoken_t * dest, const stoken_t * src)
{
    memcpy(dest, src, sizeof(stoken_t));
}

static int st_is_unary_op(const stoken_t * st)
{
    const char* str = st->val;
    const size_t len = st->len;

    if (st->type != TYPE_OPERATOR) {
        return FALSE;
    }

    switch (len) {
    case 1:
        return *str == '+' || *str == '-' || *str == '!' || *str == '~';
    case 2:
        return str[0] == '!' && str[1] == '!';
    case 3:
        return cstrcasecmp("NOT", str, len) == 0;
    default:
        return FALSE;
    }
}

/* Parsers
 *
 *
 */

static size_t parse_white(sfilter * sf)
{
    return sf->pos + 1;
}

static size_t parse_operator1(sfilter * sf)
{
    const char *cs = sf->s;
    size_t pos = sf->pos;

    st_assign_char(sf->current, TYPE_OPERATOR, pos, 1, cs[pos]);
    return pos + 1;
}

static size_t parse_other(sfilter * sf)
{
    const char *cs = sf->s;
    size_t pos = sf->pos;

    st_assign_char(sf->current, TYPE_UNKNOWN, pos, 1, cs[pos]);
    return pos + 1;
}

static size_t parse_char(sfilter * sf)
{
    const char *cs = sf->s;
    size_t pos = sf->pos;

    st_assign_char(sf->current, cs[pos], pos, 1, cs[pos]);
    return pos + 1;
}

static size_t parse_eol_comment(sfilter * sf)
{
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos;

    const char *endpos =
        (const char *) memchr((const void *) (cs + pos), '\n', slen - pos);
    if (endpos == NULL) {
        st_assign(sf->current, TYPE_COMMENT, pos, slen - pos, cs + pos);
        return slen;
    } else {
        st_assign(sf->current, TYPE_COMMENT, pos, endpos - cs - pos, cs + pos);
        return (endpos - cs) + 1;
    }
}

/** In Ansi mode, hash is an operator
 *  In MYSQL mode, it's a EOL comment like '--'
 */
static size_t parse_hash(sfilter * sf)
{
    sf->stats_comment_hash += 1;
    if (sf->comment_style == COMMENTS_ANSI) {
        st_assign_char(sf->current, TYPE_OPERATOR, sf->pos, 1, '#');
        return sf->pos + 1;
    } else {
        sf->stats_comment_hash += 1;
        return parse_eol_comment(sf);
    }
}

static size_t parse_dash(sfilter * sf)
{
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos;

    /*
     * five cases
     * 1) --[white]  this is always a SQL comment
     * 2) --[EOF]    this is a comment
     * 3) --[notwhite] in MySQL this is NOT a comment but two unary operators
     * 4) --[notwhite] everyone else thinks this is a comment
     * 5) -[not dash]  '-' is a unary operator
     */

    if (pos + 2 < slen && cs[pos + 1] == '-' && char_is_white(cs[pos+2]) ) {
        return parse_eol_comment(sf);
    } else if (pos +2 == slen && cs[pos + 1] == '-') {
        return parse_eol_comment(sf);
    } else if (pos + 1 < slen && cs[pos + 1] == '-' && sf->comment_style == COMMENTS_ANSI) {
        /* --[not-white] not-white case:
         *
         */
        sf->stats_comment_ddx += 1;
        return parse_eol_comment(sf);
    } else {
        st_assign_char(sf->current, TYPE_OPERATOR, pos, 1, '-');
        return pos + 1;
    }
}


/** This only parses MySQL 5 "versioned comments" in
 * the form of /x![anything]x/ or /x!12345[anything] x/
 *
 * Mysql 3 (maybe 4), allowed this:
 *    /x!0selectx/ 1;
 * where 0 could be any number.
 *
 * The last version of MySQL 3 was in 2003.

 * It is unclear if the MySQL 3 syntax was allowed
 * in MySQL 4.  The last version of MySQL 4 was in 2008
 *
 * Both are EOL, but one can ban all forms of MySQL
 * comments by inspecting the sfilter object.
 * If stats_comment_mysql > 0, we've parsed on (perhaps
 * incorrectly using mysql3 rules) and you can explcity
 * ban it.
 *
 */
static size_t is_mysql_comment(const char *cs, const size_t len, size_t pos)
{
    size_t i;

    /* so far...
     * cs[pos] == '/' && cs[pos+1] == '*'
     */

    if (pos + 2 >= len) {
        /* not a mysql comment */
        return 0;
    }

    if (cs[pos + 2] != '!') {
        /* not a mysql comment */
        return 0;
    }

    /*
     * this is a mysql comment
     *  got "/x!"
     */

    if (pos + 3 >= len) {
        return 3;
    }

    /* ok here is where it gets interesting
     * if the next 5 characters are all numbers
     * ignore them (there are a mysql version number)
     * and start using the 6th char.
     * select 1, /x!123456x/ = "1,6" !!
     *
     * If the next 5 character are NOT all numbers
     * then do nothing special
     * select 1, /x!123,456x/ = "1,123,456"
     *
     */

    /* /x!34567..... */
    if (len < pos + 8) {
        return 3;
    }

    for (i = pos + 3; i < pos + 8; ++i) {
        if (!isdigit(cs[i])) {
            return 3;
        }
    }

    /* we got /x!34567?...
     * skip over the first 7 characters and start using the 8th
     */
    return 8;
}

static size_t parse_slash(sfilter * sf)
{
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos;
    const char* cur = cs + pos;
    size_t inc = 0;

    size_t pos1 = pos + 1;
    if (pos1 == slen || cs[pos1] != '*') {
        return parse_operator1(sf);
    }

    /* check if this looks like a mysql comment */
    inc = is_mysql_comment(cs, slen, pos);

    if (inc > 0) {
        /* yes, mark it */
        sf->stats_comment_mysql += 1;

        if (sf->comment_style == COMMENTS_MYSQL) {
            /*
             * MySQL Comment (which is actually not a comment)
             */
            sf->in_comment = TRUE;
            st_clear(sf->current);
            return pos + inc;
        }
    }

    /* we didn't find a mysql comment or we don't care */
    if (1) {

        /*
         * skip over initial '/x'
         */
        const char *ptr = memchr2(cur + 2, slen - (pos + 2), '*', '/');
        if (ptr == NULL) {
            /*
             * unterminated comment
             */
            st_assign(sf->current, TYPE_COMMENT, pos, slen - pos, cs + pos);
            return slen;
        } else {
            /*
             * postgresql allows nested comments which makes
             * this is incompatible with parsing so
             * if we find a '/x' inside the coment, then
             * make a new token.
             */
            char ctype = TYPE_COMMENT;
            const size_t clen = (ptr + 2) - (cur);
            if (memchr2(cur + 2, ptr - (cur + 1), '/', '*') !=  NULL) {
                ctype = 'X';
            }
            st_assign(sf->current, ctype, pos, clen, cs + pos);

            return pos + clen;
        }
    }
}

static size_t parse_backslash(sfilter * sf)
{
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos;

    /*
     * Weird MySQL alias for NULL, "\N" (capital N only)
     */
    if (pos + 1 < slen && cs[pos + 1] == 'N') {
        st_assign(sf->current, TYPE_NUMBER, pos, 2, cs + pos);
        return pos + 2;
    } else {
        return parse_other(sf);
    }
}

static size_t parse_operator2(sfilter * sf)
{
    char ch;
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos;

    if (pos + 1 >= slen) {
        return parse_operator1(sf);
    }

    if (pos + 2 < slen &&
        cs[pos] == '<' &&
        cs[pos + 1] == '=' &&
        cs[pos + 2] == '>') {
        /*
         * special 3-char operator
         */
        st_assign(sf->current, TYPE_OPERATOR, pos, 3, cs + pos);
        return pos + 3;
    }

    ch = is_keyword(cs + pos, 2);
    if (ch != CHAR_NULL) {
        st_assign(sf->current, ch, pos, 2, cs+pos);
        return pos + 2;
    }

    /*
     * not an operator.. what to do with the two
     * characters we got?
     */

    /*
     * Special Hack for MYSQL style comments
     *  instead of turning:
     * /x! FOO x/  into FOO by rewriting the string, we
     * turn it into FOO x/ and ignore the ending comment
     */
    if (sf->in_comment && cs[pos] == '*' && cs[pos+1] == '/') {
        sf->in_comment = FALSE;
        st_clear(sf->current);
        return pos + 2;
    } else if (cs[pos] == ':') {
        /* ':' is not an operator */
        st_assign(sf->current, TYPE_COLON, pos, 1, cs+pos);
        return pos + 1;
    } else {
        /*
         * must be a single char operator
         */
        return parse_operator1(sf);
    }
}

/*
 * Ok!   "  \"   "  one backslash = escaped!
 *       " \\"   "  two backslash = not escaped!
 *       "\\\"   "  three backslash = escaped!
 */
static int is_backslash_escaped(const char* end, const char* start)
{
    const char* ptr;
    for (ptr = end; ptr >= start; ptr--) {
        if (*ptr != '\\') {
            break;
        }
    }
    /* if number of backslashes is odd, it is escaped */

    return (end - ptr) & 1;
}

static size_t is_double_delim_escaped(const char* cur,  const char* end)
{
    return  ((cur + 1) < end) && *(cur+1) == *cur;
}

/* Look forward for doubling of deliminter
 *
 * case 'foo''bar' --> foo''bar
 *
 * ending quote isn't duplicated (i.e. escaped)
 * since it's the wrong char or EOL
 *
 */
static size_t parse_string_core(const char *cs, const size_t len, size_t pos,
                                stoken_t * st, char delim, size_t offset)
{
    /*
     * offset is to skip the perhaps first quote char
     */
    const char *qpos =
        (const char *) memchr((const void *) (cs + pos + offset), delim,
                              len - pos - offset);

    /*
     * then keep string open/close info
     */
    if (offset == 1) {
        /*
         * this is real quote
         */
        st->str_open = delim;
    } else {
        /*
         * this was a simulated quote
         */
        st->str_open = CHAR_NULL;
    }

    while (TRUE) {
        if (qpos == NULL) {
            /*
             * string ended with no trailing quote
             * assign what we have
             */
            st_assign(st, TYPE_STRING, pos + offset, len - pos - offset, cs + pos + offset);
            st->str_close = CHAR_NULL;
            return len;
        } else if ( is_backslash_escaped(qpos - 1, cs + pos + offset)) {
            /* keep going, move ahead one character */
            qpos =
                (const char *) memchr((const void *) (qpos + 1), delim,
                                      (cs + len) - (qpos + 1));
            continue;
        } else if (is_double_delim_escaped(qpos, cs + len)) {
            /* keep going, move ahead two characters */
            qpos =
                (const char *) memchr((const void *) (qpos + 2), delim,
                                      (cs + len) - (qpos + 2));
            continue;
        } else {
            /* hey it's a normal string */
            st_assign(st, TYPE_STRING, pos + offset,
                      qpos - (cs + pos + offset), cs + pos + offset);
            st->str_close = delim;
            return qpos - cs + 1;
        }
    }
}

/**
 * Used when first char is a ' or "
 */
static size_t parse_string(sfilter * sf)
{
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos;

    /*
     * assert cs[pos] == single or double quote
     */
    return parse_string_core(cs, slen, pos, sf->current, cs[pos], 1);
}

/** MySQL ad-hoc character encoding
 *
 * if something starts with a underscore
 * check to see if it's in this form
 * _[a-z0-9] and if it's a character encoding
 * If not, let the normal 'word parser'
 * handle it.
 */
static size_t parse_underscore(sfilter *sf)
{
    const char *cs = sf->s;
    size_t slen = sf->slen;
    size_t pos = sf->pos;
    char ch;

    size_t xlen = strlenspn(cs + pos + 1, slen - pos - 1,
                              "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (xlen == 0) {
        return parse_word(sf);
    }
    st_assign(sf->current, TYPE_BAREWORD, pos, xlen, cs + pos);
    ch = is_keyword(sf->current->val, sf->current->len);
    if (ch == TYPE_SQLTYPE) {
        sf->current->type = TYPE_SQLTYPE;
        return xlen + 1;
    }
    return parse_word(sf);
}

static size_t parse_ustring(sfilter * sf)
{
    const char *cs = sf->s;
    size_t slen = sf->slen;
    size_t pos = sf->pos;

    if (pos + 2 < slen && cs[pos+1] == '&' && cs[pos+2] == '\'') {
        sf->pos += 2;
        pos = parse_string(sf);
        sf->current->str_open = 'u';
        if (sf->current->str_close == '\'') {
            sf->current->str_close = 'u';
        }
        return pos;
    } else {
        return parse_word(sf);
    }
}

static size_t parse_qstring_core(sfilter * sf, int offset)
{
    char ch;
    const char *strend;
    const char *cs = sf->s;
    size_t slen = sf->slen;
    size_t pos = sf->pos + offset;

    /* if we are already at end of string..
       if current char is not q or Q
       if we don't have 2 more chars
       if char2 != a single quote
       then, just treat as word
    */
    if (pos >= slen ||
        (cs[pos] != 'q' && cs[pos] != 'Q') ||
        pos + 2 >= slen ||
        cs[pos + 1] != '\'') {
        return parse_word(sf);
    }

    ch = cs[pos + 2];
    if (ch < 33 && ch > 127) {
        return parse_word(sf);
    }
    switch (ch) {
    case '(' : ch = ')'; break;
    case '[' : ch = ']'; break;
    case '{' : ch = '}'; break;
    case '<' : ch = '>'; break;
    }

    strend = memchr2(cs + pos + 3, slen - pos - 3, ch, '\'');
    if (strend == NULL) {
        st_assign(sf->current, TYPE_STRING, pos + 3, slen - pos - 3, cs + pos + 3);
        sf->current->str_open = 'q';
        sf->current->str_close = CHAR_NULL;
        return slen;
    } else {
        st_assign(sf->current, TYPE_STRING, pos + 3, strend - cs - pos -  3, cs + pos + 3);
        sf->current->str_open = 'q';
        sf->current->str_close = 'q';
        return (strend - cs) + 2;
    }
}

/*
 * Oracle's q string
 */
static size_t parse_qstring(sfilter * sf)
{
    return parse_qstring_core(sf, 0);
}

/*
 * Oracle's nq string
 */
static size_t parse_nqstring(sfilter * sf)
{
    return parse_qstring_core(sf, 1);
}

static size_t parse_word(sfilter * sf)
{
    char ch;
    char delim;
    size_t i;
    const char *cs = sf->s;
    size_t pos = sf->pos;
    size_t wlen = strlencspn(cs + pos, sf->slen - pos,
                             " <>:\\?=@!#~+-*/&|^%(),';\t\n\v\f\r\"");

    st_assign(sf->current, TYPE_BAREWORD, pos, wlen, cs + pos);

    /* now we need to look inside what we good for "." and "`"
     * and see if what is before is a keyword or not
     */
    for (i =0; i < sf->current->len; ++i) {
        delim = sf->current->val[i];
        if (delim == '.' || delim == '`') {
            ch = is_keyword(sf->current->val, i);
            if (ch == TYPE_KEYWORD || ch == TYPE_OPERATOR || ch == TYPE_EXPRESSION) {
                /* needed for swig */
                st_clear(sf->current);
                /*
                 * we got something like "SELECT.1"
                 * or SELECT`column`
                 */
                st_assign(sf->current, ch, pos, i, cs + pos);
                return pos + i;
            }
        }
    }

    /*
     * do normal lookup with word including '.'
     */
    if (wlen < ST_MAX_SIZE) {

        ch = is_keyword(sf->current->val, wlen);

        if (ch == CHAR_NULL) {
            ch = TYPE_BAREWORD;
        }
        sf->current->type = ch;
    }
    return pos + wlen;
}

/* MySQL backticks are a cross between string and
 * and a bare word.
 *
 */
static size_t parse_tick(sfilter* sf)
{
    size_t pos =  parse_string_core(sf->s, sf->slen, sf->pos, sf->current, '`', 1);

    /* we could check to see if start and end of
     * of string are both "`", i.e. make sure we have
     * matching set.  `foo` vs. `foo
     * but I don't think it matters much
     */

    /* check value of string to see if it's a keyword,
     * function, operator, etc
     */
    char ch = is_keyword(sf->current->val, sf->current->len);
    if (ch == TYPE_FUNCTION) {
        /* if it's a function, then convert token */
        sf->current->type = TYPE_FUNCTION;
    } else {
        /* otherwise it's a 'n' type -- mysql treats
         * everything as a bare word
         */
        sf->current->type = TYPE_BAREWORD;
    }
    return pos;
}

static size_t parse_var(sfilter * sf)
{
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos + 1;
    size_t xlen;

    /*
     * var_count is only used to reconstruct
     * the input.  It counts the number of '@'
     * seen 0 in the case of NULL, 1 or 2
     */

    /*
     * move past optional other '@'
     */
    if (pos < slen && cs[pos] == '@') {
        pos += 1;
        sf->current->count = 2;
    } else {
        sf->current->count = 1;
    }

    /*
     * MySQL allows @@`version`
     */
    if (pos < slen) {
        if (cs[pos] == '`') {
            sf->pos = pos;
            pos = parse_tick(sf);
            sf->current->type = TYPE_VARIABLE;
            return pos;
        } else if (cs[pos] == CHAR_SINGLE || cs[pos] == CHAR_DOUBLE) {
            sf->pos = pos;
            pos = parse_string(sf);
            sf->current->type = TYPE_VARIABLE;
            return pos;
        }
    }


    xlen = strlencspn(cs + pos, slen - pos,
                     " <>:\\?=@!#~+-*/&|^%(),';\t\n\v\f\r'`\"");
    if (xlen == 0) {
        st_assign(sf->current, TYPE_VARIABLE, pos, 0, cs + pos);
        return pos;
    } else {
        st_assign(sf->current, TYPE_VARIABLE, pos, xlen, cs + pos);
        return pos + xlen;
    }
}

static size_t parse_money(sfilter *sf)
{
    const char* strend;
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos;
    size_t xlen;

    if (pos + 1 == slen) {
        /* end of line */
        st_assign_char(sf->current, TYPE_BAREWORD, pos, 1, '$');
        return slen;
    }

    /*
     * $1,000.00 or $1.000,00 ok!
     * This also parses $....,,,111 but that's ok
     */

    xlen = strlenspn(cs + pos + 1, slen - pos - 1, "0123456789.,");
    if (xlen == 0) {
        if (cs[pos + 1] == '$') {
            /* we have $$ .. find ending $$ and make string */
            strend = memchr2(cs + pos + 2, slen - pos -2, '$', '$');
            if (strend == NULL) {
                /* fell off edge */
                st_assign(sf->current, TYPE_STRING, pos + 2, slen - (pos + 2), cs + pos + 2);
                sf->current->str_open = '$';
                sf->current->str_close = CHAR_NULL;
                return slen;
            } else {
                st_assign(sf->current, TYPE_STRING, pos + 2, strend - (cs + pos + 2), cs + pos + 2);
                sf->current->str_open = '$';
                sf->current->str_close = '$';
                return strend - cs + 2;
            }
        } else {
            /* ok it's not a number or '$$', but maybe it's pgsql "$ quoted strings" */
            xlen = strlenspn(cs + pos + 1, slen - pos - 1, "abcdefghjiklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
            if (xlen == 0) {
                /* hmm it's "$" _something_ .. just add $ and keep going*/
                st_assign_char(sf->current, TYPE_BAREWORD, pos, 1, '$');
                return pos + 1;
            }
            /* we have $foobar????? */
            /* is it $foobar$ */
            if (pos + xlen + 1 == slen || cs[pos+xlen+1] != '$') {
                /* not $foobar$, or fell off edge */
                st_assign_char(sf->current, TYPE_BAREWORD, pos, 1, '$');
                return pos + 1;
            }

            /* we have $foobar$ ... find it again */
            strend = my_memmem(cs+xlen+2, slen - (pos+xlen+2), cs + pos, xlen+2);

            if (strend == NULL) {
                /* fell off edge */
                st_assign(sf->current, TYPE_STRING, pos+xlen+2, slen - pos - xlen - 2, cs+pos+xlen+2);
                sf->current->str_open = '$';
                sf->current->str_close = CHAR_NULL;
                return slen;
            } else {
                /* got one */
                st_assign(sf->current, TYPE_STRING, pos+xlen+2, strend - (cs + pos + xlen + 2), cs+pos+xlen+2);
                sf->current->str_open = '$';
                sf->current->str_close = '$';
                return (strend + xlen + 2) - cs;
            }
        }
    } else {
        st_assign(sf->current, TYPE_NUMBER, pos, 1 + xlen, cs + pos);
        return pos + 1 + xlen;
    }
}

static size_t parse_number(sfilter * sf)
{
    const char *cs = sf->s;
    const size_t slen = sf->slen;
    size_t pos = sf->pos;
    size_t xlen;
    size_t start;

    if (pos + 1 < slen && cs[pos] == '0' && (cs[pos + 1] == 'X' || cs[pos + 1] == 'x')) {
        /*
         * TBD compare if isxdigit
         */
        xlen =
            strlenspn(cs + pos + 2, slen - pos - 2, "0123456789ABCDEFabcdef");
        if (xlen == 0) {
            st_assign(sf->current, TYPE_BAREWORD, pos, 2, cs + pos);
            return pos + 2;
        } else {
            st_assign(sf->current, TYPE_NUMBER, pos, 2 + xlen, cs + pos);
            return pos + 2 + xlen;
        }
    }

    start = pos;
    while (pos < slen && isdigit(cs[pos])) {
        pos += 1;
    }
    if (pos < slen && cs[pos] == '.') {
        pos += 1;
        while (pos < slen && isdigit(cs[pos])) {
            pos += 1;
        }
        if (pos - start == 1) {
            st_assign_char(sf->current, TYPE_BAREWORD, start, 1, '.');
            return pos;
        }
    }

    if (pos < slen) {
        if (cs[pos] == 'E' || cs[pos] == 'e') {
            pos += 1;
            if (pos < slen && (cs[pos] == '+' || cs[pos] == '-')) {
                pos += 1;
            }
            while (pos < slen && isdigit(cs[pos])) {
                pos += 1;
            }
        } else if (isalpha(cs[pos])) {
            /*
             * oh no, we have something like '6FOO'
             * use microsoft style parsing and take just
             * the number part and leave the rest to be
             * parsed later
             */
            st_assign(sf->current, TYPE_NUMBER, start, pos - start, cs + start);
            return pos;
        }
    }

    st_assign(sf->current, TYPE_NUMBER, start, pos - start, cs + start);
    return pos;
}

int libinjection_sqli_tokenize(sfilter * sf, stoken_t *current)
{
    const char *s = sf->s;
    const size_t slen = sf->slen;
    size_t *pos = &sf->pos;
    pt2Function fnptr;

    if (slen == 0) {
        return FALSE;
    }

    st_clear(current);
    sf->current = current;

    /*
     * if we are at beginning of string
     *  and in single-quote or double quote mode
     *  then pretend the input starts with a quote
     */
    if (*pos == 0 && sf->delim != CHAR_NULL) {
        *pos = parse_string_core(s, slen, 0, current, sf->delim, 0);
        return TRUE;
    }

    while (*pos < slen) {
        /*
         * get current character
         */
        const unsigned ch = (unsigned int) (s[*pos]);

        /*
         * if not ascii, then continue...
         *   actually probably need to just assuming
         *   it's a string
         */
        if (ch > 127) {
            fnptr = parse_word;
        } else {

        /*
         * look up the parser, and call it
         *
         * Porting Note: this is mapping of char to function
         *   charparsers[ch]()
         */
        fnptr = char_parse_map[ch];
        }
        *pos = (*fnptr) (sf);

        /*
         *
         */
        if (current->type != CHAR_NULL) {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * Initializes parsing state
 *
 */
void libinjection_sqli_init(sfilter * sf, const char *s, size_t len, char delim, int comment_style)
{
    memset(sf, 0, sizeof(sfilter));
    sf->s = s;
    sf->slen = len;
    sf->delim = delim;
    sf->comment_style = comment_style;
}

/** See if two tokens can be merged since they are compound SQL phrases.
 *
 * This takes two tokens, and, if they are the right type,
 * merges their values together.  Then checks to see if the
 * new value is special using the PHRASES mapping.
 *
 * Example: "UNION" + "ALL" ==> "UNION ALL"
 *
 * C Security Notes: this is safe to use C-strings (null-terminated)
 *  since the types involved by definition do not have embedded nulls
 *  (e.g. there is no keyword with embedded null)
 *
 * Porting Notes: since this is C, it's oddly complicated.
 *  This is just:  multikeywords[token.value + ' ' + token2.value]
 *
 */
static int syntax_merge_words(stoken_t * a, stoken_t * b)
{
    size_t sz1;
    size_t sz2;
    size_t sz3;
    char tmp[ST_MAX_SIZE];
    char ch;

    /* first token is of right type? */
    if (!
        (a->type == TYPE_KEYWORD || a->type == TYPE_BAREWORD || a->type == TYPE_OPERATOR
         || a->type == TYPE_UNION || a->type == TYPE_EXPRESSION || a->type == TYPE_SQLTYPE)) {
        return CHAR_NULL;
    }

    if (b->type != TYPE_KEYWORD  && b->type != TYPE_BAREWORD &&
        b->type != TYPE_OPERATOR && b->type != TYPE_SQLTYPE &&
        b->type != TYPE_UNION    && b->type != TYPE_EXPRESSION) {
        return CHAR_NULL;
    }

    sz1 = a->len;
    sz2 = b->len;
    sz3 = sz1 + sz2 + 1; /* +1 for space in the middle */
    if (sz3 >= ST_MAX_SIZE) { /* make sure there is room for ending null */
        return FALSE;
    }
    /*
     * oddly annoying  last.val + ' ' + current.val
     */
    memcpy(tmp, a->val, sz1);
    tmp[sz1] = ' ';
    memcpy(tmp + sz1 + 1, b->val, sz2);
    tmp[sz3] = CHAR_NULL;

    ch = is_keyword(tmp, sz3);
    if (ch != CHAR_NULL) {
        st_assign(a, ch, a->pos, sz3, tmp);
        return TRUE;
    } else {
        return FALSE;
    }
}


/*
 * My apologies, this code is a mess
 */
int filter_fold(sfilter * sf)
{
    stoken_t last_comment;

    stoken_t * current;

    /* POS is the positive of where the NEXT token goes */
    size_t pos = 0;

    /* LEFT is a count of how many tokens that are already
       folded or processed (i.e. part of the fingerprint) */
    size_t left =  0;

    int more = 1;

    st_clear(&last_comment);

    /* Skip all initial comments, right-parens ( and unary operators
     *
     */
    current = &(sf->tokenvec[0]);
    while (more) {
        more = libinjection_sqli_tokenize(sf, current);
        if ( ! (current->type == TYPE_COMMENT || current->type == TYPE_LEFTPARENS || st_is_unary_op(current))) {
            break;
        }
    }

    if (! more) {
        /* If input was only comments, unary or (, then exit */
        return 0;
    } else {
        /* it's some other token */
        pos += 1;
    }

    while (1) {
        FOLD_DEBUG
        /* get up to two tokens */
        while (more && pos <= MAX_TOKENS && (pos - left) < 2) {
            current = &(sf->tokenvec[pos]);
            more = libinjection_sqli_tokenize(sf, current);
            if (more) {
                if (current->type == TYPE_COMMENT) {
                    st_copy(&last_comment, current);
                } else {
                    last_comment.type = CHAR_NULL;
                    pos += 1;
                }
            }
        }
        FOLD_DEBUG
        /* did we get 2 tokens? if not then we are done */
        if (pos - left != 2) {
            left = pos;
            break;
        }

        /* FOLD: "ss" -> "s"
         * "foo" "bar" is valid SQL
         * just ignore second string
         */
        if (sf->tokenvec[left].type == TYPE_STRING && sf->tokenvec[left+1].type == TYPE_STRING) {
            pos -= 1;
            sf->stats_folds += 1;
            continue;
        } else if (sf->tokenvec[left].type ==TYPE_OPERATOR && st_is_unary_op(&sf->tokenvec[left+1])) {
            pos -= 1;
            sf->stats_folds += 1;
            if (left > 0) {
                left -= 1;
            }
            continue;
        } else if (sf->tokenvec[left].type ==TYPE_LEFTPARENS && st_is_unary_op(&sf->tokenvec[left+1])) {
            pos -= 1;
            sf->stats_folds += 1;
            if (left > 0) {
                left -= 1;
            }
            continue;
        } else if (syntax_merge_words(&sf->tokenvec[left], &sf->tokenvec[left+1])) {
            pos -= 1;
            continue;
        } else if (sf->tokenvec[left].type == TYPE_BAREWORD &&
                   sf->tokenvec[left+1].type == TYPE_LEFTPARENS && (
                       cstrcasecmp("IN", sf->tokenvec[left].val, sf->tokenvec[left].len) == 0 ||
                       cstrcasecmp("DATABASE", sf->tokenvec[left].val, sf->tokenvec[left].len) == 0 ||
                       cstrcasecmp("USER", sf->tokenvec[left].val, sf->tokenvec[left].len) == 0 ||
                       cstrcasecmp("PASSWORD", sf->tokenvec[left].val, sf->tokenvec[left].len) == 0
                       )) {

            // pos is the same
            // other conversions need to go here... for instance
            // password CAN be a function, coalese CAN be a function
            sf->tokenvec[left].type = TYPE_FUNCTION;
            continue;
#if 0
        } else if (sf->tokenvec[left].type == TYPE_OPERATOR && cstrcasecmp("LIKE", sf->tokenvec[left].val) == 0
                   && sf->tokenvec[left+1].type == TYPE_LEFTPARENS) {
            // two use cases   "foo" LIKE "BAR" (normal operator)
            // "foo" = LIKE(1,2)
            sf->tokenvec[left].type = TYPE_FUNCTION;
            continue;
#endif
        } else if (sf->tokenvec[left].type == TYPE_SQLTYPE &&
                   (sf->tokenvec[left+1].type == TYPE_BAREWORD || sf->tokenvec[left+1].type == TYPE_NUMBER ||
                    sf->tokenvec[left+1].type == TYPE_VARIABLE || sf->tokenvec[left+1].type == TYPE_STRING))  {
            st_copy(&sf->tokenvec[left], &sf->tokenvec[left+1]);
            pos -= 1;
            sf->stats_folds += 1;
            continue;
        }

        /* all cases of handing 2 tokens is done
           and nothing matched.  Get one more token
        */
        FOLD_DEBUG
        while (more && pos <= MAX_TOKENS && pos - left < 3) {
            current = &(sf->tokenvec[pos]);
            more = libinjection_sqli_tokenize(sf, current);
            if (more) {
                if (current->type == TYPE_COMMENT) {
                    st_copy(&last_comment, current);
                } else {
                    last_comment.type = CHAR_NULL;
                    pos += 1;
                }
            }
        }

        /* do we have three tokens? If not then we are done */
        if (pos -left != 3) {
            left = pos;
            break;
        }

        /*
         * now look for three token folding
         */
        if (sf->tokenvec[left].type == TYPE_NUMBER &&
            sf->tokenvec[left+1].type == TYPE_OPERATOR &&
            sf->tokenvec[left+2].type == TYPE_NUMBER) {
            pos -= 2;
            continue;
        } else if (sf->tokenvec[left].type == TYPE_OPERATOR &&
                   sf->tokenvec[left+1].type != TYPE_LEFTPARENS &&
                   sf->tokenvec[left+2].type == TYPE_OPERATOR) {
            if (left > 0) {
                left -= 1;
            }
            pos -= 2;
            continue;
        } else if (sf->tokenvec[left].type == TYPE_LOGIC_OPERATOR &&
                   sf->tokenvec[left+2].type == TYPE_LOGIC_OPERATOR) {
            pos -= 2;
            continue;
        } else if ((sf->tokenvec[left].type == TYPE_BAREWORD || sf->tokenvec[left].type == TYPE_NUMBER ) &&
                   sf->tokenvec[left+1].type == TYPE_OPERATOR &&
                   (sf->tokenvec[left+2].type == TYPE_NUMBER || sf->tokenvec[left+2].type == TYPE_BAREWORD)) {
            pos -= 2;
            continue;
        } else if ((sf->tokenvec[left].type == TYPE_BAREWORD || sf->tokenvec[left].type == TYPE_NUMBER ||
                    sf->tokenvec[left].type == TYPE_VARIABLE || sf->tokenvec[left].type == TYPE_STRING) &&
                   sf->tokenvec[left+1].type == TYPE_OPERATOR &&
                   sf->tokenvec[left+2].type == TYPE_SQLTYPE) {
            pos -= 2;
            sf->stats_folds += 2;
            continue;
        } else if ((sf->tokenvec[left].type == TYPE_BAREWORD || sf->tokenvec[left].type == TYPE_NUMBER || sf->tokenvec[left].type == TYPE_STRING) &&
                   sf->tokenvec[left+1].type == TYPE_COMMA &&
                   (sf->tokenvec[left+2].type == TYPE_NUMBER || sf->tokenvec[left+2].type == TYPE_BAREWORD || sf->tokenvec[left+2].type == TYPE_STRING)) {
            pos -= 2;
            continue;
        } else if ((sf->tokenvec[left].type == TYPE_KEYWORD || sf->tokenvec[left].type == TYPE_EXPRESSION) &&
                   st_is_unary_op(&sf->tokenvec[left+1]) &&
                   (sf->tokenvec[left+2].type == TYPE_NUMBER || sf->tokenvec[left+2].type == TYPE_BAREWORD || sf->tokenvec[left+2].type == TYPE_VARIABLE || sf->tokenvec[left+2].type == TYPE_STRING || sf->tokenvec[left+2].type == TYPE_FUNCTION )) {
            // remove unary operators
            // select - 1
            st_copy(&sf->tokenvec[left+1], &sf->tokenvec[left+2]);
            pos -= 1;
        } else if (sf->tokenvec[left].type == TYPE_BAREWORD &&
                   sf->tokenvec[left+1].type == TYPE_BAREWORD  && sf->tokenvec[left+1].val[0] == '.' &&
                   sf->tokenvec[left+2].type == TYPE_BAREWORD) {
            /* ignore the '.n'
             * typically is this dabasename.table
             */
            pos -= 2;
            continue;
        }


        /* no folding -- assume left-most token is
           is good, now use the existing 2 tokens --
           do not get another
        */

        left += 1;

    } /* while(1) */

    /* if we have 4 or less tokens, and we had a comment token
     * at the end, add it back
     */

    if (left < MAX_TOKENS && last_comment.type == TYPE_COMMENT) {
        st_copy(&sf->tokenvec[left], &last_comment);
        left += 1;
    }

    /* sometimes we grab a 6th token to help
       determine the type of token 5.
    */
    if (left > MAX_TOKENS) {
        left = MAX_TOKENS;
    }

    return (int)left;
}

/* secondary api: detects SQLi in a string, GIVEN a context.
 *
 * A context can be:
 *   *  CHAR_NULL (\0), process as is
 *   *  CHAR_SINGLE ('), process pretending input started with a
 *          single quote.
 *   *  CHAR_DOUBLE ("), process pretending input started with a
 *          double quote.
 *
 */
const char*
libinjection_sqli_fingerprint(sfilter * sql_state,
                              const char *s, size_t slen,
                              char delim, int comment_style)
{
    int i;
    int tlen = 0;

    libinjection_sqli_init(sql_state, s, slen, delim, comment_style);

    tlen = filter_fold(sql_state);
    for (i = 0; i < tlen; ++i) {
        sql_state->pat[i] = sql_state->tokenvec[i].type;
    }

    /*
     * make the fingerprint pattern a c-string (null delimited)
     */
    sql_state->pat[tlen] = CHAR_NULL;

    /*
     * check for 'X' in pattern, and then
     * clear out all tokens
     *
     * this means parsing could not be done
     * accurately due to pgsql's double comments
     * or other syntax that isn't consistent.
     * Should be very rare false positive
     */
    if (strchr(sql_state->pat, 'X')) {
        /*  needed for SWIG */
        memset((void*)sql_state->pat, 0, MAX_TOKENS + 1);
        sql_state->pat[0] = 'X';

        sql_state->tokenvec[0].type = 'X';
        sql_state->tokenvec[0].val[0] = 'X';
        sql_state->tokenvec[0].val[1] = '\0';
        sql_state->tokenvec[1].type = CHAR_NULL;
    }

    return sql_state->pat;
}


/**
 *
 */
#define UNUSED(x) (void)(x)

int libinjection_sqli_check_fingerprint(sfilter* sql_state, void* callbackarg)
{
    UNUSED(callbackarg);

    return libinjection_sqli_blacklist(sql_state) &&
        libinjection_sqli_not_whitelist(sql_state);
}

int libinjection_sqli_blacklist(sfilter* sql_state)
{
    char fp2[MAX_TOKENS + 2];
    char ch;
    size_t i;
    size_t len = strlen(sql_state->pat);

    if (len < 1) {
        sql_state->reason = __LINE__;
        return FALSE;
    }

    /*
      to keep everything compatible, convert the
      v0 fingerprint pattern to v1
      v0: up to 5 chars, mixed case
      v1: 1 char is '0', up to 5 more chars, upper case
    */

    fp2[0] = '0';
    for (i = 0; i < len; ++i) {
        ch = sql_state->pat[i];
        if (ch >= 'a' && ch <= 'z') {
            ch -= 0x20;
        }
        fp2[i+1] = ch;
    }
    fp2[i+1] = '\0';

    int patmatch = is_keyword(fp2, len + 1) == TYPE_FINGERPRINT;

    /*
     * No match.
     *
     * Set sql_state->reason to current line number
     * only for debugging purposes.
     */
    if (!patmatch) {
        sql_state->reason = __LINE__;
        return FALSE;
    }

    return TRUE;
}

/*
 * return TRUE if sqli, false is benign
 */
int libinjection_sqli_not_whitelist(sfilter* sql_state)
{
    /*
     * We assume we got a SQLi match
     * This next part just helps reduce false positives.
     *
     */
    char ch;
    size_t tlen = strlen(sql_state->pat);

    switch (tlen) {
    case 2:{
        /*
         * case 2 are "very small SQLi" which make them
         * hard to tell from normal input...
         */

        /*
         * if 'comment' is '#' ignore.. too many FP
         */
        if (sql_state->tokenvec[1].val[0] == '#') {
            sql_state->reason = __LINE__;
            return FALSE;
        }

        /*
         * for fingerprint like 'nc', only comments of /x are treated
         * as SQL... ending comments of "--" and "#" are not sqli
         */
        if (sql_state->tokenvec[0].type == TYPE_BAREWORD &&
            sql_state->tokenvec[1].type == TYPE_COMMENT &&
            sql_state->tokenvec[1].val[0] != '/') {
                sql_state->reason = __LINE__;
                return FALSE;
        }

        /*
         * if '1c' ends with '/x' then it's sqli
         */
        if (sql_state->tokenvec[0].type == TYPE_NUMBER &&
            sql_state->tokenvec[1].type == TYPE_COMMENT &&
            sql_state->tokenvec[1].val[0] == '/') {
            return TRUE;
        }

        /*
         * if 'oc' then input must be 'CASE/x'
         * used in HPP attack
         */
        if (sql_state->tokenvec[0].type == TYPE_OPERATOR &&
            sql_state->tokenvec[1].type == TYPE_COMMENT &&
            sql_state->tokenvec[1].val[0] == '/' &&
            cstrcasecmp("CASE", sql_state->tokenvec[0].val, sql_state->tokenvec[0].len) != 0)
        {
            sql_state->reason = __LINE__;
            return FALSE;
        }

        /**
         * there are some odd base64-looking query string values
         * 1234-ABCDEFEhfhihwuefi--
         * which evaluate to "1c"... these are not SQLi
         * but 1234-- probably is.
         * Make sure the "1" in "1c" is actually a true decimal number
         *
         * Need to check -original- string since the folding step
         * may have merged tokens, e.g. "1+FOO" is folded into "1"
         *
         * Note: evasion: 1*1--
         */
        if (sql_state->tokenvec[0].type == TYPE_NUMBER &&
            sql_state->tokenvec[1].type == TYPE_COMMENT) {
            /*
             * we check that next character after the number is either whitespace,
             * or '/' or a '-' ==> sqli.
             */
            ch = sql_state->s[sql_state->tokenvec[0].len];
            if ( ch <= 32 ) {
                /* next char was whitespace,e.g. "1234 --"
                 * this isn't exactly correct.. ideally we should skip over all whitespace
                 * but this seems to be ok for now
                 */
                return TRUE;
            }
            if (ch == '/' && sql_state->s[sql_state->tokenvec[0].len + 1] == '*') {
                return TRUE;
            }
            if (ch == '-' && sql_state->s[sql_state->tokenvec[0].len + 1] == '-') {
                return TRUE;
            }

            sql_state->reason = __LINE__;
            return FALSE;
        }

        /*
         * detect obvious sqli scans.. many people put '--' in plain text
         * so only detect if input ends with '--', e.g. 1-- but not 1-- foo
         */
        if ((sql_state->tokenvec[1].len > 2)
            && sql_state->tokenvec[1].val[0] == '-') {
            sql_state->reason = __LINE__;
            return FALSE;
        }

        break;
    } /* case 2 */
    case 3:{
        /*
         * ...foo' + 'bar...
         * no opening quote, no closing quote
         * and each string has data
         */
        if (streq(sql_state->pat, "sos")
            || streq(sql_state->pat, "s&s")) {
                if ((sql_state->tokenvec[0].str_open == CHAR_NULL)
                    && (sql_state->tokenvec[2].str_close == CHAR_NULL)
                    && (sql_state->tokenvec[0].str_close == sql_state->tokenvec[2].str_open)) {
                    /*
                     * if ....foo" + "bar....
                     */
                    return TRUE;
                } else {
                    /*
                     * not sqli
                     */
                    sql_state->reason = __LINE__;
                    return FALSE;
                }
        } else if (streq(sql_state->pat, "so1")) {
            if (sql_state->tokenvec[0].str_open != CHAR_NULL) {
                /* "foo" -1 is ok, foo"-1 is not */
                sql_state->reason = __LINE__;
                return FALSE;
            }
        } else if ((sql_state->tokenvec[1].type == TYPE_KEYWORD) &&
                   (sql_state->tokenvec[1].len > 5) &&
                   cstrcasecmp("INTO", sql_state->tokenvec[1].val, 4)) {
            /* if it's not "INTO OUTFILE", or "INTO DUMPFILE" (MySQL)
             * then treat as safe
             */
            sql_state->reason = __LINE__;
            return FALSE;
        }
        break;
    }  /* case 3 */
    case 5: {
        /* nothing right now */
        break;
    } /* case 5 */
    } /* end switch */

    return TRUE;
}

/**  Main API, detects SQLi in an input.
 *
 *
 */
static int reparse_as_mysql(sfilter * sql_state)
{
    return sql_state->stats_comment_ddx ||
        sql_state->stats_comment_mysql ||
        sql_state->stats_comment_hash;
}

int libinjection_is_sqli(sfilter * sql_state, const char *s, size_t slen,
                         ptr_fingerprints_fn fn, void* callbackarg)
{
    /*
     * no input? not sqli
     */
    if (slen == 0) {
        return FALSE;
    }

    if (fn == NULL) {
        fn = libinjection_sqli_check_fingerprint;
    }

    /*
     * test input "as-is"
     */
    libinjection_sqli_fingerprint(sql_state, s, slen, CHAR_NULL, COMMENTS_ANSI);
    if (fn(sql_state, callbackarg)) {
        return TRUE;
    } else if (reparse_as_mysql(sql_state)) {
      libinjection_sqli_fingerprint(sql_state, s, slen, CHAR_NULL, COMMENTS_MYSQL);
      if (fn(sql_state, callbackarg)) {
        return TRUE;
      }
    }
    /*
     * if input has a single_quote, then
     * test as if input was actually '
     * example: if input if "1' = 1", then pretend it's
     *   "'1' = 1"
     * Porting Notes: example the same as doing
     *   is_string_sqli(sql_state, "'" + s, slen+1, NULL, fn, arg)
     *
     */
    if (memchr(s, CHAR_SINGLE, slen)) {
      libinjection_sqli_fingerprint(sql_state, s, slen, CHAR_SINGLE, COMMENTS_ANSI);
      if (fn(sql_state, callbackarg)) {
        return TRUE;
      } else if (reparse_as_mysql(sql_state)) {
        libinjection_sqli_fingerprint(sql_state, s, slen, CHAR_SINGLE, COMMENTS_MYSQL);
        if (fn(sql_state, callbackarg)) {
          return TRUE;
        }
      }
    }

    /*
     * same as above but with a double-quote "
     */
    if (memchr(s, CHAR_DOUBLE, slen)) {
      libinjection_sqli_fingerprint(sql_state, s, slen, CHAR_DOUBLE, COMMENTS_MYSQL);
        if (fn(sql_state, callbackarg)) {
            return TRUE;
        }
    }

    /*
     * Hurray, input is not SQLi
     */
    return FALSE;
}
