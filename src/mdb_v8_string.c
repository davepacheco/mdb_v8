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
			struct v8string *v8s_sliced_parent;
			int		v8s_sliced_offset;
		} v8s_slicedinfo;

		struct {
			uintptr_t	v8s_external_data;
		} v8s_external;
		/* XXX working here */
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
	int length;
	v8_string_t *strp;

	if (read_typebyte(&type, addr) != 0) {
		v8_warn("could not read type for string: %p\n", addr);
		return (NULL);
	}

	if (!V8_TYPE_STRING(typebyte)) {
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

	return (strp);
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
	boolean_t verbose = (v8flags & JSSTR_VERBOSE) != 0;
	int err;
	uint8_t type;

	/*
	 * XXX For verbose, need to write obj_jstype() replacement that uses
	 * mdbv8_strbuf_t.
	 */
	if (JSSTR_DEPTH(v8flags) > JSSTR_MAXDEPTH) {
		mdbv8_strbuf_sprintf("<maximum depth exceeded>");
		return (-1);
	}

	type = v8s->v8s_type;
	if (V8_STRENC_ASCII(type))
		v8flags |= JSSTR_ISASCII;
	else
		v8flags &= ~JSSTR_ISASCII;

	v8flags = JSSTR_BUMPDEPTH(v8flags);
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

	return (err);
}

static int
v8string_write_seq(v8string_t *strp, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags,
    size_t sliceoffset, ssize_t slicelen)
{
	size_t nstrchrs, nstrbytes;
	size_t nreadoffset, nreadchrs, nreadbytes;
	boolean_t verbose;
	uintptr_t charsp;
	size_t blen, bufsz;
	char buf[8192];

	verbose = (v8flags & JSSTR_VERBOSE) != 0;
	if (slicelen != -1) {
		nstrchrs = slicelen;
	}

	bufsz = sizeof (buf);
	nstrchrs = v8string_length(strp);
	nreadchrs = (slicelen == -1 ? slicelen : nstrchrs) - sliceoffset;
	if (nreadchrs <= 0) {
		if (verbose) {
			mdb_printf("str %p: length %d chars (%d bytes), "
			    "slice %d to %d: 0 chars\n", strp->v8s_addr,
			    nstrchrs, nstrbytes, sliceoffset, slicelen);
		}

		/* XXX quoted */
		return (0);
	}

	if ((v8flags & JSSTR_ISASCII) != 0) {
		nstrbytes = nstrchrs;
		nreadoffset = sliceoffset;
		nreadbytes = nreadchrs;
		charsp = addr + V8_OFF_SEQASCIISTR_CHARS;
	} else {
		nstrbytes = 2 * nstrchrs;
		nreadoffset = 2 * sliceoffset;
		nreadbytes = nreadchrs;
		charsp = addr + V8_OFF_SEQTWOBYTESTR_CHARS;
	}

	if (verbose) {
		mdb_printf("str %p: length %d chars (%d bytes), slice %d "
		    "to %d, internal buffer size %d",
		    strp->v8s_addr, nstrchrs, nstrbytes, sliceoffset,
		    slicelen, bufsz);
	}

	/* XXX working here */
	v8_warn("not yet supported: sequential strings\n");
	return (-1);
}

static int
v8string_write_cons(v8string_t *strp, mdbv8_strbuf_t *strp,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	/* XXX */
	v8_warn("not yet supported: cons strings\n");
	return (-1);
}

static int
v8string_write_sliced(v8string_t *strp, mdbv8_strbuf_t *strp,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	/* XXX */
	v8_warn("not yet supported: sliced strings\n");
	return (-1);
}

static int
v8string_write_ext(v8string_t *strp, mdbv8_strbuf_t *strp,
    mdbv8_strappend_flags_t strflags, v8string_flags_t v8flags)
{
	uintptr_t ptr1, ptr2;

	if ((v8flags & JSSTR_ISASCII) == 0) {
		mdbv8_strbuf_sprintf(strp, "<external two-byte string>");
		return (0);
	}

	/* XXX */
	v8_warn("not yet supported: external strings\n");
	return (-1);
}
