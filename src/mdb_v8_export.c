/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */

/*
 * mdb_v8_export.c: implementations of functions used for postmortem export
 *
 * The hierarchy of functions here resembles the jsobj_print_() family of
 * functions.
 */

#include <assert.h>

#include "v8dbg.h"
#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"

static int jsexport_double(pmx_stream_t *, double);
static int jsexport_string(pmx_stream_t *, uint8_t, uintptr_t);

static int jsexport_heapnumber(pmx_stream_t *, uintptr_t);
static int jsexport_oddball(pmx_stream_t *, uintptr_t);
static int jsexport_object(pmx_stream_t *, uintptr_t);
static int jsexport_array(pmx_stream_t *, uintptr_t);
static int jsexport_typedarray(pmx_stream_t *, uintptr_t);
static int jsexport_function(pmx_stream_t *, uintptr_t);
static int jsexport_date(pmx_stream_t *, uintptr_t);
static int jsexport_regexp(pmx_stream_t *, uintptr_t);

/*
 * Currently, the export process is driven by findjsobjects.  This could
 * potentially be much faster if we leveraged the fact that we've already
 * enumerated the properties of the object and used the in-memory structure for
 * that.  However, it would be less general-purpose (it's kind of nice to be
 * able to take an arbitrary address and export it), and we'd have to record a
 * bit more information during findjsobjects than we currently do (e.g., the V8
 * type of the object) so that we could figure out whether we have to traverse
 * this as an array, a typedarray, an object, or the like.
 *
 * XXX This should probably be commonized with jsobj_print().
 */
int
jsexport_value(pmx_stream_t *pmxp, v8propvalue_t *valp)
{
	uintptr_t addr;
	uint8_t type;
	int (*func)(pmx_stream_t *, uintptr_t);

	if (valp->v8v_isboxeddouble) {
		return (jsexport_double(pmxp, valp->v8v_u.v8vu_double));
	}

	addr = valp->v8v_u.v8vu_addr;
	if (V8_IS_SMI(addr)) {
		/*
		 * SMI values do not need to be included in the postmortem
		 * export because the consumer is expected to identify a
		 * reference to an SMI as containing the value itself.
		 */
		return (0);
	}

	if (!V8_IS_HEAPOBJECT(addr)) {
		v8_warn("jsexport_value: %p: not a heap object\n", addr);
		return (-1);
	}

	if (read_typebyte(&type, addr) != 0) {
		return (-1);
	}

	if (V8_TYPE_STRING(type)) {
		return (jsexport_string(pmxp, addr, type));
	}

	if (type == V8_TYPE_MUTABLEHEAPNUMBER || type == V8_TYPE_HEAPNUMBER) {
		func = jsexport_heapnumber;
	} else if (type == V8_TYPE_ODDBALL) {
		func = jsexport_oddball; 	
	} else if (type == V8_TYPE_JSOBJECT) {
		func = jsexport_object;		
	} else if (type == V8_TYPE_JSARRAY) {
		func = jsexport_array;		
	} else if (type == V8_TYPE_JSTYPEDARRAY) {
		func = jsexport_typedarray;	
	} else if (type == V8_TYPE_JSFUNCTION) {
		func = jsexport_function; 	
	} else if (type == V8_TYPE_JSDATE) {
		func = jsexport_date; 		
	} else if (type == V8_TYPE_JSREGEXP) {
		func = jsexport_regexp; 	
	} else {
		v8_warn("jsexport_value: %p: unknown type");
		return (-1);
	}

	return (func(pmxp, addr));
}

static int
jsexport_double(pmx_stream_t *pmxp, double d)
{
	/*
	 * TODO This is not currently expressable with the postmortem export
	 * format because it doesn't know about boxed doubles.
	 */
	return (0);
}

static int
jsexport_string(pmx_stream_t *pmxp, uint8_t type, uintptr_t addr)
{
	/* XXX complicated */
	return (0);
}

static int
jsexport_heapnumber(pmx_stream_t *pmxp, uintptr_t addr)
{
	/* XXX this should be pulled into a first-class interface */
	double numval;
	if (read_heap_double(&numval, addr, V8_OFF_HEAPNUMBER_VALUE) == -1) {
		return (-1);
	}

	pmx_emit_node_heapnumber(pmxp, addr, numval);
	return (0);
}

static int
jsexport_oddball(pmx_stream_t *pmxp, uintptr_t addr)
{
	/* XXX this should be pulled into a first-class interface */
	uintptr_t strptr;
	mdbv8_strbuf_t strbuf;
	v8string_t *v8strp;
	char buf[32];

	mdbv8_strbuf_init(&strbuf, buf, sizeof (buf));

	if (read_heap_ptr(&strptr, addr, V8_OFF_ODDBALL_TO_STRING) != 0) {
		return (-1);
	}

	v8strp = v8string_load(strptr, UM_SLEEP);
	if (v8string_write(v8strp, &strbuf, MSF_ASCIIONLY, JSSTR_NUDE) != 0) {
		return (-1);
	}

	if (strcmp(buf, "undefined") == 0) {
		pmx_emit_node_undefined(pmxp, addr, strptr);
	} else if (strcmp(buf, "the_hole") == 0) {
		pmx_emit_node_hole(pmxp, addr, strptr);
	} else if (strcmp(buf, "true") == 0) {
		pmx_emit_node_boolean(pmxp, addr, PB_TRUE, strptr);
	} else if (strcmp(buf, "false") == 0) {
		pmx_emit_node_boolean(pmxp, addr, PB_FALSE, strptr);
	} else if (strcmp(buf, "null") == 0) {
		pmx_emit_node_null(pmxp, addr, strptr);
	}

	v8string_free(v8strp);
	return (0);
}

static int
jsexport_object(pmx_stream_t *pmxp, uintptr_t addr) { return (0); }
static int
jsexport_array(pmx_stream_t *pmxp, uintptr_t addr) { return (0); }
static int
jsexport_typedarray(pmx_stream_t *pmxp, uintptr_t addr) { return (0); }
static int
jsexport_function(pmx_stream_t *pmxp, uintptr_t addr) { return (0); }
static int
jsexport_date(pmx_stream_t *pmxp, uintptr_t addr) { return (0); }
static int
jsexport_regexp(pmx_stream_t *pmxp, uintptr_t addr) { return (0); }
