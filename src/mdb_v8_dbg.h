/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * mdb_v8_dbg.h: interface for working with V8 objects in a debugger.
 *
 * This file should contain types and functions useful for debugging Node.js
 * programs.  These functions may currently be implemented in terms of the MDB
 * module API, but this interface should not include any MDB-specific
 * functionality.  The expectation is that this could be implemented by another
 * backend, and that it could be used to implement a different user interface.
 *
 *
 * GENERAL NOTES
 *
 * Addresses in the target program are represented as "uintptr_t".  Most of
 * these are either V8 small integers (see V8_IS_SMI() and V8_SMI_VALUE()) or
 * other V8 heap objects.  A number of functions exists to inspect and dump
 * these, but they have not yet been abstracted here.
 *
 * Functions here fall into one of two categories: functions that return "int"
 * (or a pointer that may be NULL) can generally fail because of a validation
 * problem or a failure to read information from the target's address space.
 * Other functions cannot fail because it's assumed that whatever conditions
 * they depend on have already been validated.  They typically assert such
 * conditions.  It's critical that such conditions _have_ already been checked
 * (e.g., in v8context_load() or by the caller).  The debugger should not assume
 * that the target's address space is not arbitrarily corrupt.
 */

#ifndef	_MDBV8DBG_H
#define	_MDBV8DBG_H

#include <stdarg.h>
#include <sys/types.h>

/*
 * Basic types
 */

typedef struct {
	char	*ms_buf;	/* full buffer */
	size_t	ms_bufsz;	/* full buffer size */
	char	*ms_curbuf;	/* current position in buffer */
	size_t	ms_curbufsz;	/* current buffer size left */
	size_t	ms_reservesz;	/* bytes reserved */
	int	ms_flags;	/* buffer flags */
	int	ms_memflags;	/* memory allocation flags */
} mdbv8_strbuf_t;

typedef struct v8function v8function_t;

typedef struct v8context v8context_t;
typedef struct v8scopeinfo v8scopeinfo_t;
typedef struct v8scopeinfo_var v8scopeinfo_var_t;

typedef enum {
	V8SV_PARAMS,
	V8SV_STACKLOCALS,
	V8SV_CONTEXTLOCALS
} v8scopeinfo_vartype_t;


/*
 * Working with ASCII strings.
 */

typedef enum {
	MSF_ASCIIONLY	= 0x1,			/* replace non-ASCII */
	MSF_JSON	= MSF_ASCIIONLY | 0x2,	/* partial JSON string */
} mdbv8_strappend_flags_t;

mdbv8_strbuf_t *mdbv8_strbuf_alloc(size_t, int);
void mdbv8_strbuf_free(mdbv8_strbuf_t *);
void mdbv8_strbuf_init(mdbv8_strbuf_t *, char *, size_t);
void mdbv8_strbuf_legacy_update(mdbv8_strbuf_t *, char **, size_t *);

size_t mdbv8_strbuf_bufsz(mdbv8_strbuf_t *);
size_t mdbv8_strbuf_bytesleft(mdbv8_strbuf_t *);

void mdbv8_strbuf_rewind(mdbv8_strbuf_t *);
void mdbv8_strbuf_reserve(mdbv8_strbuf_t *, ssize_t);
void mdbv8_strbuf_appendc(mdbv8_strbuf_t *, uint16_t, mdbv8_strappend_flags_t);
void mdbv8_strbuf_appends(mdbv8_strbuf_t *, const char *,
    mdbv8_strappend_flags_t);
void mdbv8_strbuf_sprintf(mdbv8_strbuf_t *, const char *, ...);
void mdbv8_strbuf_vsprintf(mdbv8_strbuf_t *, const char *, va_list);
const char *mdbv8_strbuf_tocstr(mdbv8_strbuf_t *);


/*
 * Working with JavaScript strings
 */
typedef struct v8string v8string_t;

typedef enum {
	JSSTR_NONE,
	JSSTR_NUDE	= JSSTR_NONE,

	JSSTR_FLAGSHIFT = 16,
	JSSTR_VERBOSE   = (0x1 << JSSTR_FLAGSHIFT),
	JSSTR_QUOTED    = (0x2 << JSSTR_FLAGSHIFT),
	JSSTR_ISASCII   = (0x4 << JSSTR_FLAGSHIFT),

	JSSTR_MAXDEPTH  = 512
} v8string_flags_t;

/*
 * XXX These definitions should move into mdb_v8_string.c when references in
 * mdb_v8.c are removed.
 */
#define	JSSTR_DEPTH(f)		((f) & ((1 << JSSTR_FLAGSHIFT) - 1))
#define	JSSTR_BUMPDEPTH(f)	((f) + 1)

v8string_t *v8string_load(uintptr_t, int);
void v8string_free(v8string_t *);

size_t v8string_length(v8string_t *);
int v8string_write(v8string_t *, mdbv8_strbuf_t *,
    mdbv8_strappend_flags_t, v8string_flags_t);


/*
 * Contexts, closures, and ScopeInfo objects
 *
 * Each JavaScript closure (an instance of the V8 "JSFunction" class) has its
 * own Context (another V8 heap object).  The Context contains values of
 * variables that are accessible from that context.  By looking at the Context
 * associated with a closure, we can see the values of variables accessible in
 * that closure.  (Contexts are also used for other facilities, like "with"
 * expressions, but there is no support here for dealing with other kinds of
 * Contexts.)
 *
 * The information about the layout of a Context is stored in a separate
 * ScopeInfo object.  The ScopeInfo describes, among other things, the names of
 * the variables accessible in that context.  All closures for a given function
 * (in the JavaScript source code) share the same ScopeInfo, and that ScopeInfo
 * is available on the SharedFunctionInfo object referenced by each JSFunction
 * object.  (This makes sense because all closures for a given function (in the
 * source code) share the same set of accessible variable names.)
 *
 * ScopeInfo objects also include information about parameters and stack-local
 * variables, but the values of these are not available from a Contexts.
 *
 * In order to commonize code around reading and validating context information,
 * we require that callers use v8context_load() in order to work with Contexts.
 * Similarly, we provide v8scopeinfo_load() in order to work with ScopeInfo
 * objects.  As a convenient special case, we provide v8context_scopeinfo() to
 * load a scopeinfo_t for a v8context_t.
 *
 * Inside V8, both Context and ScopeInfo objects are implemented as FixedArrays.
 * Both have a few statically-defined slots that describe the object, followed
 * by dynamic slots.  For Contexts, the dynamic slots are described by the
 * corresponding ScopeInfo.  For ScopeInfo objects, the dynamic slots are
 * described by the initial statically-defined slots.
 *
 * For more on Context internals, see src/context.h in the V8 source.  For more
 * information on ScopeInfo internals, see the declaration of the ScopeInfo
 * class in src/objects.h in the V8 source.
 */


/*
 * Working with Contexts
 */

v8context_t *v8context_load(uintptr_t, int);
void v8context_free(v8context_t *);
uintptr_t v8context_closure(v8context_t *);
uintptr_t v8context_prev_context(v8context_t *);
int v8context_var_value(v8context_t *, unsigned int, uintptr_t *);

int v8context_iter_static_slots(v8context_t *,
    int (*)(v8context_t *, const char *, uintptr_t, void *), void *);
int v8context_iter_dynamic_slots(v8context_t *,
    int (*func)(v8context_t *, uint_t, uintptr_t, void *), void *);

/*
 * Working with ScopeInfo objects
 */
v8scopeinfo_t *v8scopeinfo_load(uintptr_t, int);
void v8scopeinfo_free(v8scopeinfo_t *);
v8scopeinfo_t *v8context_scopeinfo(v8context_t *, int);

int v8scopeinfo_iter_vartypes(v8scopeinfo_t *,
    int (*)(v8scopeinfo_t *, v8scopeinfo_vartype_t, void *), void *);
const char *v8scopeinfo_vartype_name(v8scopeinfo_vartype_t);

size_t v8scopeinfo_vartype_nvars(v8scopeinfo_t *, v8scopeinfo_vartype_t);
int v8scopeinfo_iter_vars(v8scopeinfo_t *, v8scopeinfo_vartype_t,
    int (*)(v8scopeinfo_t *, v8scopeinfo_var_t *, void *), void *);
size_t v8scopeinfo_var_idx(v8scopeinfo_t *, v8scopeinfo_var_t *);
uintptr_t v8scopeinfo_var_name(v8scopeinfo_t *, v8scopeinfo_var_t *);

/*
 * Working with JSFunction objects
 *
 * JSFunction objects represent closures, rather than a single instance of the
 * function in the source code.  There may be many JSFunction objects for what
 * programmers would typically call a "function" -- one for each active closure.
 * Most of the JSFunction-related facilities have not yet been folded into this
 * interface.
 */
v8function_t *v8function_load(uintptr_t, int);
void v8function_free(v8function_t *);
v8context_t *v8function_context(v8function_t *, int);
v8scopeinfo_t *v8function_scopeinfo(v8function_t *, int);

typedef struct v8funcinfo v8funcinfo_t;
v8funcinfo_t *v8function_funcinfo(v8function_t *, int);
void v8funcinfo_free(v8funcinfo_t *);

#endif	/* _MDBV8DBG_H */
