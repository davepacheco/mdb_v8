/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * mdb_v8_context.c: implementations of functions used for working with Contexts
 * and ScopeInfos.  See mdb_v8_dbg.h for details.
 */

#include <assert.h>
#include <strings.h>

#include "v8dbg.h"
#include "mdb_v8_dbg.h"
#include "mdb_v8_impl.h"

struct v8context {
	uintptr_t	v8ctx_addr;	/* context address in target process */
	uintptr_t	*v8ctx_elts;	/* copied-in array of context slots */
	size_t		v8ctx_nelts;	/* count of context slots */
	int		v8ctx_memflags;	/* memory allocation flags */
};

/*
 * This structure and array describe the statically-defined fields stored inside
 * each Context.  This is mainly useful for debugger tools that want to dump
 * everything inside the context.
 */
typedef struct {
	const char	*v8ctxf_label;	/* name of field */
	intptr_t	*v8ctxf_idxp;	/* ptr to index into context (array) */
} v8context_field_t;

static v8context_field_t v8context_fields[] = {
	{ "closure function",	&V8_CONTEXT_IDX_CLOSURE },
	{ "previous context",	&V8_CONTEXT_IDX_PREV },
	{ "extension",		&V8_CONTEXT_IDX_EXT },
	{ "global object",	&V8_CONTEXT_IDX_GLOBAL },
};

static size_t v8context_nfields =
    sizeof (v8context_fields) / sizeof (v8context_fields[0]);

struct v8scopeinfo {
	uintptr_t	v8si_addr;	/* ScopeInfo address in target proc */
	uintptr_t	*v8si_elts;	/* copied-in array of slots */
	size_t		v8si_nelts;	/* count of slots */
	int		v8si_memflags;	/* memory allocation flags */
};

/*
 * This structure and array describe the layout of a ScopeInfo.  Each
 * vartype_info describes a certain kind of variable, and the structures below
 * include pointers to the field (inside a ScopeInfo) that stores the count of
 * that kind of variable.
 */
struct v8scopeinfo_var {
	size_t	v8siv_which;
	size_t	v8siv_realidx;
};

typedef struct {
	v8scopeinfo_vartype_t	v8vti_vartype;
	const char		*v8vti_label;
	intptr_t		*v8vti_idx_countp;
	intptr_t		*v8vti_offset;
} v8scopeinfo_vartype_info_t;

static v8scopeinfo_vartype_info_t v8scopeinfo_vartypes[] = {
	{ V8SV_PARAMS, "parameter", &V8_SCOPEINFO_IDX_NPARAMS },
	{ V8SV_STACKLOCALS, "stack local variable",
	    &V8_SCOPEINFO_IDX_NSTACKLOCALS, &V8_SCOPEINFO_OFFSET_STACK_LOCALS },
	{ V8SV_CONTEXTLOCALS, "context local variable",
	    &V8_SCOPEINFO_IDX_NCONTEXTLOCALS },
};

static size_t v8scopeinfo_nvartypes =
    sizeof (v8scopeinfo_vartypes) / sizeof (v8scopeinfo_vartypes[0]);


struct v8function {
	uintptr_t	v8func_addr;		/* address in target proc */
	int		v8func_memflags;	/* allocation flags */
	uintptr_t	v8func_shared;		/* SharedFunctionInfo */
};

struct v8funcinfo {
	uintptr_t	v8fi_script;		/* script object */
	uintptr_t	v8fi_funcname;		/* function name (string) */
	uintptr_t	v8fi_inferred_name;	/* inferred func name */
	uintptr_t	v8fi_scriptpath;	/* script file name (string) */
	uintptr_t	v8fi_tokenpos;		/* "function" token position */
};

/*
 * Local utility function declarations.
 */

static uintptr_t v8context_elt(v8context_t *, unsigned int);
static v8scopeinfo_vartype_info_t *v8scopeinfo_vartype_lookup(
    v8scopeinfo_vartype_t);


/*
 * Context functions
 */

/*
 * Given a V8 Context in "addr", load it into "ctxp".  This will validate basic
 * properties of the context.  "memflags" are used for memory allocation.
 * Returns a context on success and NULL on failure.
 */
v8context_t *
v8context_load(uintptr_t addr, int memflags)
{
	v8context_t *ctxp;

	if ((ctxp = mdb_zalloc(sizeof (*ctxp), memflags)) == NULL) {
		return (NULL);
	}

	ctxp->v8ctx_addr = addr;
	if (read_heap_array(addr, &ctxp->v8ctx_elts,
	    &ctxp->v8ctx_nelts, memflags) != 0) {
		goto err;
	}

	if (ctxp->v8ctx_nelts < V8_CONTEXT_NCOMMON) {
		v8_warn("%p: context array is too short\n", addr);
		goto err;
	}

	return (ctxp);

err:
	v8context_free(ctxp);
	return (NULL);
}

void
v8context_free(v8context_t *ctxp)
{
	if (ctxp == NULL) {
		return;
	}

	maybefree(ctxp->v8ctx_elts,
	    ctxp->v8ctx_nelts * sizeof (ctxp->v8ctx_elts[0]),
	    ctxp->v8ctx_memflags);
	maybefree(ctxp, sizeof (*ctxp), ctxp->v8ctx_memflags);
}

/*
 * Returns the address of the closure associated with this context.
 *
 * The closure is a JSFunction object.
 */
uintptr_t
v8context_closure(v8context_t *ctxp)
{
	return (v8context_elt(ctxp, V8_CONTEXT_IDX_CLOSURE));
}

/*
 * Returns the "previous" context for this context.
 */
uintptr_t
v8context_prev_context(v8context_t *ctxp)
{
	return (v8context_elt(ctxp, V8_CONTEXT_IDX_PREV));
}

/*
 * Returns in "*valptr" the value of JavaScript variable "i" in this context.
 * ("i" is an index, described by the context's ScopeInfo.)
 */
int
v8context_var_value(v8context_t *ctxp, unsigned int i, uintptr_t *valptr)
{
	unsigned int idx;

	idx = i + V8_CONTEXT_NCOMMON;
	if (i >= ctxp->v8ctx_nelts) {
		v8_warn("context %p: variable index %d is out of range\n",
		    ctxp->v8ctx_addr, i);
		return (-1);
	}

	*valptr = v8context_elt(ctxp, idx);
	return (0);
}

/*
 * Load scope information for this context into "sip".  See v8scopeinfo_load()
 * for "memflags".
 */
v8scopeinfo_t *
v8context_scopeinfo(v8context_t *ctxp, int memflags)
{
	uintptr_t closure;
	v8function_t *funcp;
	v8scopeinfo_t *sip;

	closure = v8context_closure(ctxp);
	funcp = v8function_load(closure, memflags);
	if (funcp == NULL) {
		return (NULL);
	}
	sip = v8function_scopeinfo(funcp, memflags);
	v8function_free(funcp);
	return (sip);
}

/*
 * Private, low-level function for accessing individual slots of the underlying
 * array.
 */
static uintptr_t
v8context_elt(v8context_t *ctxp, unsigned int i)
{
	assert(i < ctxp->v8ctx_nelts);
	return (ctxp->v8ctx_elts[i]);
}


/*
 * Low-level context structure
 *
 * Iterate the statically-defined slots in this context.  These should
 * correspond to the four fields described above.  With each slot, the caller
 * gets the slot label and the value in that slot.
 */
int
v8context_iter_static_slots(v8context_t *ctxp,
    int (*func)(v8context_t *, const char *, uintptr_t, void *), void *arg)
{
	unsigned int i;
	intptr_t idx;
	uintptr_t value;
	v8context_field_t *fp;
	int rv;

	rv = 0;
	for (i = 0; i < v8context_nfields; i++) {
		fp = &v8context_fields[i];
		idx = *(fp->v8ctxf_idxp);
		value = v8context_elt(ctxp, idx);
		rv = func(ctxp, fp->v8ctxf_label, value, arg);
		if (rv != 0) {
			break;
		}
	}

	return (rv);
}

/*
 * Iterate the dynamically-defined slots in this context.  These correspond to
 * the values described in the context's ScopeInfo.  With each slot, the caller
 * gets the integer index of the slot (relative to the start of the dynamic
 * slots) and the value in that slot.  (This function does not assume that the
 * scope information has been loaded, so it only provides values by the integer
 * index.)
 */
int
v8context_iter_dynamic_slots(v8context_t *ctxp,
    int (*func)(v8context_t *, uint_t, uintptr_t, void *), void *arg)
{
	unsigned int nslots, i;
	int rv = 0;

	nslots = V8_CONTEXT_NCOMMON;
	for (i = nslots; i < ctxp->v8ctx_nelts; i++) {
		rv = func(ctxp, i - nslots, v8context_elt(ctxp, i), arg);
		if (rv != 0) {
			break;
		}
	}

	return (rv);
}


/*
 * ScopeInfo functions
 */

/*
 * Given a V8 ScopeInfo in "addr", load it into "sip".  This will validate
 * basic properties of the ScopeInfo.  "memflags" are used for memory
 * allocation.
 *
 * Returns 0 on success and -1 on failure.  On failure, the specifed scope info
 * must not be used for anything.
 */
v8scopeinfo_t *
v8scopeinfo_load(uintptr_t addr, int memflags)
{
	v8scopeinfo_t *sip;

	if ((sip = mdb_zalloc(sizeof (*sip), memflags)) == NULL) {
		return (NULL);
	}

	sip->v8si_memflags = memflags;
	sip->v8si_addr = addr;
	if (read_heap_array(addr,
	    &sip->v8si_elts, &sip->v8si_nelts, memflags) != 0) {
		goto err;
	}

	if (sip->v8si_nelts < V8_SCOPEINFO_IDX_FIRST_VARS) {
		v8_warn("array too short to be a ScopeInfo\n");
		goto err;
	}

	if (!V8_IS_SMI(sip->v8si_elts[V8_SCOPEINFO_IDX_NPARAMS]) ||
	    !V8_IS_SMI(sip->v8si_elts[V8_SCOPEINFO_IDX_NSTACKLOCALS]) ||
	    !V8_IS_SMI(sip->v8si_elts[V8_SCOPEINFO_IDX_NCONTEXTLOCALS])) {
		v8_warn("static ScopeInfo fields do not look like SMIs\n");
		goto err;
	}

	return (sip);

err:
	v8scopeinfo_free(sip);
	return (NULL);
}

void
v8scopeinfo_free(v8scopeinfo_t *sip)
{
	if (sip == NULL) {
		return;
	}

	maybefree(sip->v8si_elts,
	    sip->v8si_nelts * sizeof (sip->v8si_elts[0]), sip->v8si_memflags);
	maybefree(sip, sizeof (*sip), sip->v8si_memflags);
}

/*
 * Iterate the vartypes in a ScopeInfo, which correspond to different kinds of
 * variable (e.g., "parameter", "stack-local variable", or "context-local
 * variable").  The caller gets an enum describing the vartype, which can be
 * used to get the vartype name and iterate variables of this type.
 */
int
v8scopeinfo_iter_vartypes(v8scopeinfo_t *sip,
    int (*func)(v8scopeinfo_t *, v8scopeinfo_vartype_t, void *), void *arg)
{
	int i, rv;
	v8scopeinfo_vartype_info_t *vtip;

	rv = 0;

	for (i = 0; i < v8scopeinfo_nvartypes; i++) {
		vtip = &v8scopeinfo_vartypes[i];
		rv = func(sip, vtip->v8vti_vartype, arg);
		if (rv != 0)
			break;
	}

	return (rv);
}

/*
 * Returns a human-readable label for a given kind of scope variable.  The scope
 * variable must be valid.
 */
const char *
v8scopeinfo_vartype_name(v8scopeinfo_vartype_t scopevartype)
{
	v8scopeinfo_vartype_info_t *vtip;

	vtip = v8scopeinfo_vartype_lookup(scopevartype);
	assert(vtip != NULL);
	return (vtip->v8vti_label);
}

/*
 * Returns the number of variables of this kind (e.g., the number of
 * context-local variables, when scopevartype is V8SV_CONTEXTLOCALS).
 */
size_t
v8scopeinfo_vartype_nvars(v8scopeinfo_t *sip,
    v8scopeinfo_vartype_t scopevartype)
{
	v8scopeinfo_vartype_info_t *vtip;
	uintptr_t value;

	vtip = v8scopeinfo_vartype_lookup(scopevartype);
	assert(vtip != NULL);
	value = sip->v8si_elts[*(vtip->v8vti_idx_countp)];
	assert(V8_IS_SMI(value));
	return (V8_SMI_VALUE(value));
}

/*
 * Iterate the variables of the kind specified by "scopevartype" (e.g.,
 * context-local variables, when scopevartype is V8SV_CONTEXTLOCALS).  With each
 * variable, the caller gets an opaque pointer that can be used to get the
 * variable's name and an index for retrieving its value from a given context.
 */
int
v8scopeinfo_iter_vars(v8scopeinfo_t *sip,
    v8scopeinfo_vartype_t scopevartype,
    int (*func)(v8scopeinfo_t *, v8scopeinfo_var_t *, void *), void *arg)
{
	int rv;
	size_t i, nvars, nskip, idx;
	v8scopeinfo_vartype_info_t *vtip, *ogrp;
	v8scopeinfo_var_t var;

	vtip = v8scopeinfo_vartype_lookup(scopevartype);
	assert(vtip != NULL);
	nvars = v8scopeinfo_vartype_nvars(sip, scopevartype);

	/*
	 * Skip to the start of the ScopeInfo's dynamic part. See mdb_v8_db.h
	 * for more details on the layout of ScopeInfo objects.
	 */
	nskip = V8_SCOPEINFO_IDX_FIRST_VARS;

	/*
	 * Iterate over variable types so that we can add the offset from the
	 * beginning of the actual data (the dynamic part) to the region of the
	 * dynamic part that is specific to the variable type we're interested
	 * in.
	 */
	for (i = 0; i < v8scopeinfo_nvartypes; i++) {
		ogrp = &v8scopeinfo_vartypes[i];

		/*
		 * In the variable/dynamic part of a ScopeInfo layout, some
		 * variable types have static metadata, e.g stack local entries
		 * have a StackLocalFirstSlot, before the actual data. Add that
		 * offset for each variable type, including for the one we're
		 * interested in.
		 */
		if (v8scopeinfo_vartypes[i].v8vti_offset != NULL &&
		    *(v8scopeinfo_vartypes[i].v8vti_offset) != -1) {
			nskip += *(v8scopeinfo_vartypes[i].v8vti_offset);
		}

		/*
		 * If the current variable type is the one we're interested in,
		 * do not add anything to the offset. We're done.
		 */
		if (*(ogrp->v8vti_idx_countp) == *(vtip->v8vti_idx_countp)) {
			break;
		}

		/*
		 * The data for the current variable type is before the one
		 * we're interested in in the variable part of the ScopeInfo
		 * layout. Add the number of entries for this variable type to
		 * the offset.
		 */
		nskip += v8scopeinfo_vartype_nvars(sip, ogrp->v8vti_vartype);
	}

	rv = 0;
	for (i = 0; i < nvars; i++) {
		idx = nskip + i;
		if (idx >= sip->v8si_nelts) {
			v8_warn("v8scopeinfo_iter_vars: short scopeinfo\n");
			return (-1);
		}

		var.v8siv_which = i;
		var.v8siv_realidx = idx;
		rv = func(sip, &var, arg);
		if (rv != 0) {
			break;
		}
	}

	return (rv);
}

/*
 * Returns the integer index for this variable.  This is used to extract the
 * value out of a context with this scope.
 */
size_t
v8scopeinfo_var_idx(v8scopeinfo_t *sip, v8scopeinfo_var_t *sivp)
{
	return (sivp->v8siv_which);
}

/*
 * Returns the name of of this variable (as a heap string).
 */
uintptr_t
v8scopeinfo_var_name(v8scopeinfo_t *sip, v8scopeinfo_var_t *sivp)
{
	assert(sivp->v8siv_realidx < sip->v8si_nelts);
	return (sip->v8si_elts[sivp->v8siv_realidx]);
}

/*
 * Look up our internal metadata for this vartype.
 */
static v8scopeinfo_vartype_info_t *
v8scopeinfo_vartype_lookup(v8scopeinfo_vartype_t scopevartype)
{
	int i;
	v8scopeinfo_vartype_info_t *vtip;

	for (i = 0; i < v8scopeinfo_nvartypes; i++) {
		vtip = &v8scopeinfo_vartypes[i];
		if (scopevartype == vtip->v8vti_vartype)
			return (vtip);
	}

	return (NULL);
}


/*
 * JSFunction functions
 */

/*
 * Given a JSFunction pointer in "addr", validates the pointer and returns a
 * v8function_t that can be used for working with the function.
 */
v8function_t *
v8function_load(uintptr_t addr, int memflags)
{
	uint8_t type;
	uintptr_t shared;
	v8function_t *funcp;

	if (!V8_IS_HEAPOBJECT(addr) || read_typebyte(&type, addr) != 0) {
		v8_warn("%p: not a heap object\n", addr);
		return (NULL);
	}

	if (type != V8_TYPE_JSFUNCTION) {
		v8_warn("%p: not a JSFunction\n", addr);
		return (NULL);
	}

	if (read_heap_ptr(&shared, addr, V8_OFF_JSFUNCTION_SHARED) != 0) {
		v8_warn("%p: no SharedFunctionInfo\n", addr);
		return (NULL);
	}

	if ((funcp = mdb_zalloc(sizeof (*funcp), memflags)) == NULL) {
		return (NULL);
	}

	funcp->v8func_addr = addr;
	funcp->v8func_memflags = memflags;
	funcp->v8func_shared = shared;
	return (funcp);
}

/*
 * Given a JSFunction in "funcp", load into "ctxp" the context associated with
 * this function.  This is a convenience function that finds the context and
 * calls v8context_load(), so see the notes about that function.
 */
v8context_t *
v8function_context(v8function_t *funcp, int memflags)
{
	uintptr_t addr, context;

	addr = funcp->v8func_addr;
	if (read_heap_ptr(&context, addr, V8_OFF_JSFUNCTION_CONTEXT) != 0) {
		v8_warn("%p: failed to read context\n", addr);
		return (NULL);
	}

	return (v8context_load(context, memflags));
}

/*
 * Given a JSFunction in "funcp", load the ScopeInfo associated with this
 * function.  This is a convenience function that ultimately calls
 * v8scopeinfo_load(), so see the notes about that function.
 *
 * Note that this returns the ScopeInfo that's effectively defined by this
 * function.  Contexts created _within_ this function (e.g., nested functions)
 * use this ScopeInfo.  This function itself has a context with its own
 * ScopeInfo, and that's not the same as this one.  (For that, use
 * v8function_context() and then v8context_scopeinfo().)
 */
v8scopeinfo_t *
v8function_scopeinfo(v8function_t *funcp, int memflags)
{
	uintptr_t closure, shared, scopeinfo;

	if (V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO == -1) {
		v8_warn("could not find \"scope_info\"");
		return (NULL);
	}

	closure = funcp->v8func_addr;
	if (read_heap_ptr(&shared, closure, V8_OFF_JSFUNCTION_SHARED) != 0 ||
	    read_heap_ptr(&scopeinfo, shared,
	    V8_OFF_SHAREDFUNCTIONINFO_SCOPE_INFO) != 0) {
		return (NULL);
	}

	return (v8scopeinfo_load(scopeinfo, memflags));
}

void
v8function_free(v8function_t *funcp)
{
	maybefree(funcp, sizeof (*funcp), funcp->v8func_memflags);
}


v8funcinfo_t *
v8function_funcinfo(v8function_t *funcp, int memflags)
{
	v8funcinfo_t *fip;
	uintptr_t funcinfo, script, name, inferred_name;
	uintptr_t scriptpath, lineends, tokenpos;

	funcinfo = funcp->v8func_shared;
	if (read_heap_maybesmi(&tokenpos, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_FUNCTION_TOKEN_POSITION) != 0 ||
	    read_heap_ptr(&name, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_NAME) != 0 ||
	    read_heap_ptr(&script, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_SCRIPT) != 0 ||
	    read_heap_ptr(&scriptpath, script, V8_OFF_SCRIPT_NAME) != 0 ||
	    read_heap_ptr(&lineends, script, V8_OFF_SCRIPT_LINE_ENDS) != 0) {
		return (NULL);
	}

	if (read_heap_ptr(&inferred_name, funcinfo,
	    V8_OFF_SHAREDFUNCTIONINFO_INFERRED_NAME) != 0) {
		inferred_name = 0;
	}

	/*
	 * The token position is normally a SMI, so read_heap_maybesmi() will
	 * interpret the value for us.  However, this code uses its SMI-encoded
	 * value, so convert it back here.
	 */
	tokenpos = V8_VALUE_SMI(tokenpos);

	fip = mdb_zalloc(sizeof (*fip), memflags);
	if (fip == NULL) {
		return (NULL);
	}

	fip->v8fi_script = script;
	fip->v8fi_funcname = name;
	fip->v8fi_inferred_name = inferred_name;
	fip->v8fi_scriptpath = scriptpath;
	fip->v8fi_tokenpos = tokenpos;

	return (fip);
}

int
v8funcinfo_funcname(v8funcinfo_t *fip, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t flags)
{
	/* XXX want to use inferred name if this is empty */
	/* XXX want v8string_load(), and what if that fails? */
	// return (v8string_print(fip->v8fi_funcname, strb, flags));
	return (-1);
}

int
v8funcinfo_scriptpath(v8funcinfo_t *fip, mdbv8_strbuf_t *strb,
    mdbv8_strappend_flags_t flags)
{
	/* XXX */
	return (-1);
	// return (v8string_print(fip->v8fi_scriptpath, strb, flags));
}
