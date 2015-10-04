/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * mdb_v8_string.c: interface for working with V8 (JavaScript) string values.
 * This differs from mdb_v8_strbuf.[hc], which is a general-purpose interface
 * within mdb_v8 for working with C strings.
 */

#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"
#include "v8dbg.h"

#include <assert.h>
#include <ctype.h>

struct v8string {
	uintptr_t	v8s_addr;
	size_t		v8s_len;
	uint8_t		v8s_type;
	int		v8s_memflags;
	union		{
		struct {
			uintptr_t	v8s_cons_p1;
			uintptr_t	v8s_cons_p2;
		} v8s_consinfo;

		struct {
			uintptr_t	v8s_sliced_parent;
			uintptr_t	v8s_sliced_offset;
		} v8s_slicedinfo;

		struct {
			uintptr_t	v8s_external_data;
			uintptr_t	v8s_external_nodedata;
		} v8s_external;
	} v8s_info;
};

static int v8string_write_seq(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t, size_t, ssize_t);
static int v8string_write_cons(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t);
static int v8string_write_ext(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t);
static int v8string_write_sliced(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t);

v8string_t *
v8string_load(uintptr_t addr, int memflags)
{
	uint8_t type;
	uintptr_t length;
	v8string_t *strp;

	if (read_typebyte(&type, addr) != 0) {
		v8_warn("could not read type for string: %p\n", addr);
		return (NULL);
	}

	if (!V8_TYPE_STRING(type)) {
		v8_warn("not a string: %p\n", addr);
		return (NULL);
	}

	if (!V8_STRREP_SEQ(type) && !V8_STRREP_CONS(type) &&
	    !V8_STRREP_EXT(type) && !V8_STRREP_SLICED(type)) {
		v8_warn("unsupported string representation: %p\n", addr);
		return (NULL);
	}

	if (read_heap_smi(&length, addr, V8_OFF_STRING_LENGTH) != 0) {
		v8_warn("failed to read string length: %p\n", addr);
		return (NULL);
	}

	if ((strp = mdb_zalloc(sizeof (*strp), memflags)) == NULL) {
		return (NULL);
	}

	strp->v8s_addr = addr;
	strp->v8s_len = length;
	strp->v8s_type = type;
	strp->v8s_memflags = memflags;

	if (V8_STRREP_CONS(type)) {
		if (read_heap_ptr(&strp->v8s_info.v8s_consinfo.v8s_cons_p1,
		    addr, V8_OFF_CONSSTRING_FIRST) != 0 ||
		    read_heap_ptr(&strp->v8s_info.v8s_consinfo.v8s_cons_p2,
		    addr, V8_OFF_CONSSTRING_SECOND) != 0) {
			v8_warn("failed to read cons ptrs: %p\n", addr);
			goto fail;
		}
	} else if (V8_STRREP_SLICED(type)) {
		if (read_heap_ptr(
		    &strp->v8s_info.v8s_slicedinfo.v8s_sliced_parent,
		    addr, V8_OFF_SLICEDSTRING_PARENT) != 0 ||
		    read_heap_smi(
		    &strp->v8s_info.v8s_slicedinfo.v8s_sliced_offset,
		    addr, V8_OFF_SLICEDSTRING_OFFSET) != 0) {
			v8_warn("failed to read slice info: %p\n", addr);
			goto fail;
		}
	} else if (V8_STRREP_EXT(type)) {
		if (read_heap_ptr(
		    &strp->v8s_info.v8s_external.v8s_external_data,
		    addr, V8_OFF_EXTERNALSTRING_RESOURCE) != 0 ||
		    read_heap_ptr(
		    &strp->v8s_info.v8s_external.v8s_external_nodedata,
		    strp->v8s_info.v8s_external.v8s_external_data,
		    NODE_OFF_EXTSTR_DATA) != 0) {
			v8_warn("failed to read node string: %p\n", addr);
			goto fail;
		}
	}

	return (strp);

fail:
	v8string_free(strp);
	return (NULL);
}

void
v8string_free(v8string_t *strp)
{
	if (strp == NULL) {
		return;
	}

	maybefree(strp, sizeof (*strp), strp->v8s_memflags);
}

size_t
v8string_length(v8string_t *strp)
{
	return (strp->v8s_len);
}

/*
 * XXX
 * rewrite jsstr_print() in terms of these functions.
 */
int
v8string_write(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	int err;
	uint8_t type;
	boolean_t quoted;

	/*
	 * XXX For verbose, need to write obj_jstype() replacement that uses
	 * mdbv8_strbuf_t.
	 */
	if (JSSTR_DEPTH(v8flags) > JSSTR_MAXDEPTH) {
		mdbv8_strbuf_sprintf(strb, "<maximum depth exceeded>");
		return (-1);
	}

	type = strp->v8s_type;
	if (V8_STRENC_ASCII(type))
		v8flags |= JSSTR_ISASCII;
	else
		v8flags &= ~JSSTR_ISASCII;

	quoted = (v8flags & JSSTR_QUOTED) != 0;
	if (quoted) {
		mdbv8_strbuf_appendc(strb, '"', strflags);
		v8flags &= ~JSSTR_QUOTED;
		mdbv8_strbuf_reserve(strb, 1);
	}

	v8flags = JSSTR_BUMPDEPTH(v8flags) & (~JSSTR_QUOTED);
	if (V8_STRREP_SEQ(type)) {
		err = v8string_write_seq(strp, strb, strflags, v8flags, 0, -1);
	} else if (V8_STRREP_CONS(type)) {
		err = v8string_write_cons(strp, strb, strflags, v8flags);
	} else if (V8_STRREP_EXT(type)) {
		err = v8string_write_ext(strp, strb, strflags, v8flags);
	} else {
		/* Types are checked in v8string_load(). */
		assert(V8_STRREP_SLICED(type));
		err = v8string_write_sliced(strp, strb, strflags, v8flags);
	}

	if (quoted) {
		mdbv8_strbuf_reserve(strb, -1);
		mdbv8_strbuf_appendc(strb, '"', strflags);
	}

	return (err);
}

static int
v8string_write_seq(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags,
    size_t usliceoffset, ssize_t uslicelen)
{
	size_t sliceoffset;	/* actual slice offset */
	size_t slicelen;	/* actual slice length */
	size_t nstrchrs;	/* characters in the string */
	size_t nreadoffset;	/* offset (in bytes) from start of string */
	size_t nreadchrs;	/* total number of characters to read */
	size_t bytesperchar;	/* bytes per character */
	uintptr_t charsp;	/* start of string */
	size_t bufsz;		/* internal buffer size */
	char buf[8192];		/* internal buffer */

	bufsz = sizeof (buf);
	nstrchrs = v8string_length(strp);

	/*
	 * This function operates on a slice of the string, identified by
	 * initial offset ("sliceoffset") and length ("slicelen").  The special
	 * length value "-1" denotes the range from "sliceoffset" to the end of
	 * the string.  Thus, to denote the entire string, the caller would
	 * specify "sliceoffset" 0 and "slicelen" -1.  We normalize the slice
	 * offset and length here and store these values separately from the
	 * caller-provided values for debugging purposes.
	 */
	if (usliceoffset > nstrchrs) {
		sliceoffset = nstrchrs;
	} else {
		sliceoffset = usliceoffset;
	}

	if (uslicelen == -1) {
		/*
		 * The caller asked for everything from the offset to the end of
		 * the string.  Calculate the actual value here.
		 */
		slicelen = nstrchrs - sliceoffset;
	} else if (uslicelen > nstrchrs - sliceoffset) {
		/*
		 * The caller specified a length that would run past the end of
		 * the string.  Truncate it to the end of the string.
		 */
		slicelen = nstrchrs - sliceoffset;
	} else {
		slicelen = uslicelen;
	}

	assert(sliceoffset <= nstrchrs);
	assert(slicelen <= nstrchrs);
	assert(sliceoffset + slicelen <= nstrchrs);

	if ((v8flags & JSSTR_VERBOSE) != 0) {
		mdb_printf("str %p: length %d chars, slice %d length %d "
		    "(actually %d length %d)\n", strp->v8s_addr, nstrchrs,
		    usliceoffset, uslicelen, sliceoffset, slicelen);
	}

	/*
	 * We're going to read through the string's raw data, starting at the
	 * requested offset.  The specific addresses depend on whether we're
	 * looking at an ASCII or "two-byte" string.
	 */
	if ((v8flags & JSSTR_ISASCII) != 0) {
		bytesperchar = 1;
		charsp = strp->v8s_addr + V8_OFF_SEQASCIISTR_CHARS;
	} else {
		bytesperchar = 2;
		charsp = strp->v8s_addr + V8_OFF_SEQTWOBYTESTR_CHARS;
	}

	/*
	 * There's a lot of potential optimization in the loops below, but the
	 * semantics are tricky, so let's gather data before assuming there's a
	 * performance issue.
	 */
	nreadoffset = sliceoffset * bytesperchar;
	nreadchrs = 0;
	while (nreadchrs < slicelen) {
		size_t i, bufbytesleft, toread;
		uint16_t chrval;

		toread = MIN(bufsz, bytesperchar * (slicelen - nreadchrs));
		if (mdb_vread(buf, toread, charsp + nreadoffset) == -1) {
			v8_warn("failed to read SeqString data");
			return (-1);
		}

		nreadoffset += toread;
		i = 0;

		while (nreadchrs < slicelen && i < toread) {
			/*
			 * If we're low on space in the buffer, then try to
			 * leave enough space for an ellipsis.  Note that we
			 * can't calculate this once outside the loop (by
			 * comparing the slice length to the space left in the
			 * buffer) because some of the characters in the string
			 * may be escaped when written out, in which case they
			 * will expand to more than one byte.
			 */
			bufbytesleft = mdbv8_strbuf_bytesleft(strb);
			if (bufbytesleft <= sizeof ("[...]") - 1) {
				mdbv8_strbuf_appends(strb, "[...]", strflags);
				/*
				 * XXX It would be nice if callers could know
				 * whether the string was truncated or not.
				 * Maybe this whole interface would be cleaner
				 * if we first calculated the number of bytes
				 * required to store the result of this string.
				 * Then we _could_ calculate ahead of time
				 * how many of the string's characters to print.
				 * And if we had that interface, callers could
				 * make sure the buffer was large enough or know
				 * that the string was truncated.  However, that
				 * will require two passes, each of which
				 * requires a bunch of mdb_vreads().
				 * XXX that applies to the similar block in
				 * v8string_write_ext() too.
				 */
				return (0);
			}

			if ((v8flags & JSSTR_ISASCII) != 0) {
				mdbv8_strbuf_appendc(strb, buf[i], strflags);
			} else {
				assert(i % 2 == 0);
				chrval = *((uint16_t *)(buf + i));
				mdbv8_strbuf_appendc(strb, chrval, strflags);
			}

			nreadchrs++;
			i += bytesperchar;
		}
	}

	return (0);
}

static int
v8string_write_cons(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	/* XXX */
	v8_warn("not yet supported: cons strings\n");
	return (-1);
}

static int
v8string_write_sliced(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	/* XXX */
	v8_warn("not yet supported: sliced strings\n");
	return (-1);
}

static int
v8string_write_ext(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	char buf[8192];
	size_t bufsz;
	size_t nread, ntotal;
	uintptr_t charsp;

	bufsz = sizeof (buf);
	charsp = strp->v8s_info.v8s_external.v8s_external_nodedata;
	ntotal = v8string_length(strp);
	nread = 0;

	if ((v8flags & JSSTR_VERBOSE) != 0) {
		mdbv8_strbuf_sprintf(strb,
		    "external string: %p "
		    "(assuming node.js string (length %d))\n",
		    strp->v8s_addr, ntotal);
	}

	if ((v8flags & JSSTR_ISASCII) == 0) {
		mdbv8_strbuf_sprintf(strb, "<external two-byte string>");
		return (0);
	}

	while (nread < ntotal) {
		size_t i, ntoread;

		ntoread = MIN(bufsz, ntotal - nread);
		if (mdb_vread(buf, ntoread, charsp) == -1) {
			mdbv8_strbuf_sprintf(strb,
			    "<failed to read external string data>");
			return (-1);
		}

		if (nread == 0 && buf[0] != '\0' && !isascii(buf[0])) {
			mdbv8_strbuf_sprintf(strb,
			    "<found non-ASCII external string data>");
			return (-1);
		}

		nread += ntoread;
		charsp += ntoread;
		for (i = 0; i < ntoread; i++) {
			/*
			 * See v8string_write_seq().
			 */
			if (mdbv8_strbuf_bytesleft(strb) <=
			    sizeof ("[...]") - 1) {
				mdbv8_strbuf_appends(strb, "[...]", strflags);
				return (0);
			}

			mdbv8_strbuf_appendc(strb, buf[i], strflags);
		}
	}

	return (0);
}
