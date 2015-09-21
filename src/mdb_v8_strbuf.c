/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * mdb_v8_strbuf.c: implementations of string functions.
 */

#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"

typedef enum {
	MSB_NOALLOC	= 0x1,	/* stack-allocated strbuf */
} mdbv8_strbuf_flags_t;

mdbv8_strbuf_t *
mdbv8_strbuf_alloc(size_t nbytes, int memflags)
{
	mdbv8_strbuf_t *strb;

	if ((strb = mdb_zalloc(sizeof (*strb), memflags)) == NULL ||
	    (strb->ms_buf = mdb_zalloc(nbytes, memflags)) == NULL) {
		maybefree(strb);
		return (NULL);
	}

	strb->ms_bufsz = nbytes;
	strb->ms_memflags = memflags;
	mdbv8_strbuf_rewind(strb);
	return (strb);
}

void
mdbv8_strbuf_free(mdbv8_strbuf_t *strb)
{
	if (strb == NULL || (strb->ms_flags & MSB_NOALLOC) != 0) {
		return;
	}

	maybefree(strb->ms_buf);
	maybefree(strb);
}

void
mdbv8_strbuf_init(mdbv8_strbuf_t *strb, char *buf, size_t bufsz)
{
	bzero(&strb, sizeof (*strb));
	strb->ms_buf = buf;
	strb->ms_bufsz = bufsz;
	strb->ms_flags = MSB_NOALLOC;
	strb->ms_memflags = 0;
	mdbv8_strbuf_rewind(strb);
}

size_t
mdbv8_strbuf_bufsz(mdbv8_strbuf_t *strb)
{
	return (strb->ms_bufsz);
}

void
mdbv8_strbuf_rewind(mdbv8_strbuf_t *strb)
{
	strb->ms_curbuf = strb->ms_buf;
	strb->ms_curbufsz = strb->ms_bufsz;
	strb->ms_curbuf[0] = '\0';
}

void
mdbv8_strbuf_appendc(mdbv8_strbuf_t *strb, uint16_t c,
    mdbv8_strappend_flags_t flags)
{
	if ((flags & MSF_ASCIIONLY) != 0 && !isascii(c)) {
		c = '?';
	}

	if ((flags & MSF_JSON) == MSF_JSON) {
		/* XXX validate this with JSON spec */
		switch (c) {
		case '\b':
			mdbv8_strbuf_sprintf(strb, "\\b");
			return;

		case '\n':
			mdbv8_strbuf_sprintf(strb, "\\n");
			return;

		case '\r':
			mdbv8_strbuf_sprintf(strb, "\\r");
			return;

		case '\\':
			mdbv8_strbuf_sprintf(strb, "\\\\");
			return;

		case '"':
			mdbv8_strbuf_sprintf(strb, "\"");
			return;

		default:
			if (iscntrl(c)) {
				mdbv8_strbuf_sprintf(strb, "?");
				return;
			}
			break;
		}
	}

	mdbv8_strbuf_sprintf("%c", c);
}

void
mdbv8_strbuf_sprintf(mdbv8_strbuf_t *strb, const char *format, ...)
{
	va_list alist;
	size_t rv;

	va_start(alist, format);
	rv = mdbv8_strbuf_vsprintf(strb, format, alist);
	va_end(alist);

	return (rv);
}

void
mdbv8_strbuf_vsprintf(mdbv8_strbuf_t *strb, const char *format, va_list alist)
{
	size_t rv, len;

	rv = vsnprintf(strb->ms_curbuf, strb->ms_curbufsz, format, alist);
	len = MIN(rv, strb->ms_curbufsz);
	strb->ms_curbufsz -= len;
	strb->ms_curbuf += len;
}

const char *
mdbv8_strbuf_tocstr(mdbv8_strbuf_t *strb)
{
	return (strb->ms_buf);
}

void
mdbv8_strbuf_appends(mdbv8_strbuf_t *strb, const char *src,
    mdbv8_strappend_flags_t flags)
{
	size_t i, len;

	len = strlen(src);
	for (i = 0; i <= len; i++) {
		mdbv8_strbuf_appendc(strb, src[i], flags);
	}
}
