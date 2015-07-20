/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * libv8dbg.h: interface definition for a V8 postmortem debugging library
 *
 * The expectation is that this library is consumed by a program that will take
 * care of operations like reading memory from a _target_ (e.g., a core file or
 * a live process), and this library will take care of interpreting that
 * information.  The consuming program is also responsible for all formatting
 * and user interaction.
 *
 * This library is _not_ thread-safe.  The caller is responsible for serializing
 * operations on a single library handle, though multiple library handles can be
 * used by different threads.
 */

#ifndef	_LIBV8DBG_H_
#define	_LIBV8DBG_H_

#include <stdint.h>
#include <sys/time.h>

#ifndef _HAVE_BOOLEAN_T
typedef enum { B_FALSE, B_TRUE } boolean_t;
#endif /* !_HAVE_BOOLEAN_T */

/* handle for this library */
struct v8dbg;
typedef struct v8dbg v8dbg_t;

typedef struct v8dbg_params {
	int	v8dbg_version_major;
	int	v8dbg_version_minor;
	/*
	 * XXX ops vector:
	 *
	 * - allocate/free memory (and UMEM_GC?)
	 * - lookup symbol in target
	 * - iterate symbols in target
	 * - iterate mappings?
	 * - read memory from target (address, size)
	 * - read memory from target (address, as string)
	 * - emit free-form warning message?
	 * - iterate threads in target
	 * - get registers for thread X in target
	 */
} v8dbg_params_t;

/*
 * tgtaddr_t is an integer type that describes an address in the target's
 * virtual address space.  We use this everywhere to denote a value in the
 * target's virtual memory, including:
 *
 *    o Frame pointers (the same as in any native environment)
 *    o V8 heap objects, which may include:
 *        o V8 small integers
 *          (which are defined to fit inside a pointer-sized value)
 *        o Instances of internal V8 C++ classes, like FixedArray or Map
 *        o Instances of JavaScript objects (which are really instances of V8
 *          C++ classes like JSObject, JSArray, JSDate, and so on).
 *
 * In other words, these values may denote a native ("C") value in the target, a
 * V8-level value, or a JavaScript-level value.
 */
typedef uintptr_t tgtaddr_t;

/* Create and destroy contexts for using this library. */
v8dbg_t *v8dbg_open(const v8dbg_params_t *);
void v8dbg_close(v8dbg_t *);

/*
 * XXX error message reporting and fetching
 */


/*
 * API notes: nearly every function in this API takes "tgtaddr_t" as an argument
 * describing a value in the target address space and returns an integer
 * denoting whether the operation succeeded or failed.  In general, if the
 * function fails, it's because the operation was invalid (e.g., you tried to
 * read memory from the target, but that address was not mapped; or you tried to
 * get the length of a string but the passed object was not a valid string).
 * The user may have tried to print a value that was garbage, or there may be
 * memory corruption.  In any case, usually you want to stop what you're doing
 * and report an error to the user.  There aren't very many errors that are
 * programmatically handleable.
 *
 * Functions that logically return additional information use output parameters,
 * and usually return either native types (e.g., an "int" for a string's
 * length), an opaque handle (e.g., a v8dbg_frame_t for a stack frame), or
 * another tgtaddr_t (e.g., a pointer to a JavaScript value).  Additional
 * functions are provided to get more information about opaque handles and
 * JavaScript values provide additional function
 *
 * As an example, here's some code for processing a stack frame, which makes use
 * of coarse-grained error handling, an opaque type (for the stack frame), a
 * JavaScript type (for the stack frame's JavaScript function), and a native
 * type (for that function's name).
 *
 *     v8dbg_t *vp;
 *     vp = v8dbg_open(...)
 *
 *     ...
 *
 *     v8dbg_frame_t frame;
 *     v8dbg_frame_type_t frametype;
 *     tgtaddr_t framefunc, funcname;
 *     size_t namelen;
 *     char *str = NULL;
 *
 *     //
 *     // Given a frame pointer from the debugger, interpret it and return
 *     // a v8dbg_frame_t.  Then get the frame's type.
 *     //
 *     if (v8dbg_frame(vp, frameptr, V8F_NEXT, &frame) != 0 ||
 *         v8dbg_frame_type(vp, frame, &frametype) != 0)
 *		errx(EXIT_FAILURE, "bad frame");
 *
 *     // Ignore non-JavaScript frames.
 *     if (frametype != V8FT_JAVASCRIPT)
 *     		continue;
 *
 *     //
 *     // Now get the function associated with the stack frame, then the "name"
 *     // associated with the function (as a JavaScript string), then the length
 *     // of the "name" string, then allocate memory for a native string, then
 *     // copy the JavaScript string into a local buffer.  If anything fails,
 *     // tell the user and bail out.
 *     //
 *     if (v8dbg_frame_jsfunc(vp, frame, &framefunc) != 0 ||
 *         v8dbg_jsfunction_name(vp, framefunc, &funcname) != 0 ||
 *         v8dbg_jsstring_length_native(vp, funcname, &namelen) != 0 ||
 *         (str = malloc(namelen + 1)) == NULL ||
 *         v8dbg_jsstring_print(vp, funcname, str, namelen + 1) {
 *     		free(str);
 *     		errx(EXIT_FAILURE, "failed to decode frame");
 *     }
 *
 *     (void) printf("function: %s\n", str);
 */


/*
 * Stack frame information
 *
 * v8dbg_frame_t is an opaque type describing a stack frame.
 */
typedef struct v8dbg_frame *v8dbg_frame_t;

/*
 * Stack frames are identified by frame pointers.  This is natural because each
 * stack frame has exactly one frame pointer, and the purpose of the pointer is
 * to point to a stack frame.  What's confusing is that the pointer stored
 * inside a frame points to the _next_ frame, so the frame pointers are
 * off-by-one from what you might expect.  What follows is an explicit
 * explanation of a critical but tedious point.  If you don't care, skip to the
 * end, and use the default flag value of V8F_NEXT.
 *
 * Recall that a typical x86 stack looks something like this:
 *
 *
 *                           | arguments 2      |
 *                           | return address 2 |       FRAME 2: in func2()
 *                           | frame ptr 2      |<- +
 *                              ...                 |
 *                           | arguments 1      |   |
 *  REGS      points to      | return address 1 |   |   FRAME 1: in func1()
 *    %ebp --------------->  | frame ptr 1      | --+
 *    %eip
 *
 * Recall that the stack grows down.  To walk the stack, we start with %ebp, and
 * we follow pointers up (in terms of memory addresses) to the bottom of the
 * stack.  The call stack represented here has func2() calling func1().  When it
 * does so, it pushes the arguments for func1() and then the return address
 * (which is inside func2()).  func1() immediately pushes the current frame
 * pointer.
 *
 * The result is that the address we called "frame ptr 1" is contained within
 * what we called "frame 1".  But in this API, using the default flag of
 * V8F_NEXT, we use that frame pointer to describe "frame 2" (the "next" frame).
 * The reason is that from "frame ptr 1", we can easily get to the instruction
 * address in "frame 2" of func2(), as well as the arguments for the call to
 * func2().  However, despite being inside the stack frame for func1(), we
 * _cannot_ get to an instruction address that would tell us that we're in
 * func1().
 *
 * It is occasionally useful (mostly for developers of this library) to examine
 * the "immediate" frame -- that is, without dereferencing the current pointer.
 * There's no way to get the function associated with this frame, but it's
 * sometimes useful to print the arguments in that frame.  For these purposes,
 * V8F_IMMEDIATE is provided as an option.
 */
typedef enum {
    V8F_NEXT,		/* default: examine pointed-to frame (more complete) */
    V8F_IMMEDIATE	/* examine current frame, not pointed-to frame */
} v8dbg_frame_flags_t;

/*
 * Interpret the given address as a frame pointer and return a v8dbg_frame_t.
 */
int v8dbg_frame(v8dbg_t *, tgtaddr_t, v8dbg_frame_flags_t, v8dbg_frame_t *);

/*
 * Each stack frame is classified into one of a few kinds:
 *
 *     V8FT_UNKNOWN		the frame could not be classified
 *     V8FT_NATIVE		the frame describes a call to a non-internal
 *     				native ("C") function
 *     V8FT_JAVASCRIPT		the frame describes a call to a JavaScript
 *     				function
 *
 * XXX add other frame types, including "internal", which is a union of several
 * subtypes.
 *
 * Before doing anything else with a stack frame, callers should figure out if a
 * given frame is "native", "javascript", or "internal".
 *
 * To describe the frame, callers should first check the type, and then:
 *
 *     native		Use v8dbg_frame_pc() to fetch the "pc" (program counter)
 *     			for the frame.  Then use a native debugger interface for
 *     			translating this into a symbolic name and for printing
 *     			argument information.
 *
 *     javascript	Use v8dbg_frame_jsfunc() to fetch the javascript
 *     			function associated with the frame.  With this, you can
 *     			fetch the script name and function name.  You can also
 *     			use v8dbg_frame_this() and v8dbg_frame_arg() to fetch
 *     			the value of "this" in the frame and to fetch argument
 *     			values.
 *
 *     internal		You likely want to ignore these frames or just print the
 *     			frame type.
 */
typedef enum {
    V8FT_UNKNOWN,
    V8FT_NATIVE,
    V8FT_JAVASCRIPT
} v8dbg_frame_type_t;

int v8dbg_frame_type(v8dbg_t *, v8dbg_frame_t, v8dbg_frame_type_t *);
const char *v8dbg_frame_type_tostring(v8dbg_frame_type_t);

/*
 * Walk the stack by fetching the next frame (the calling frame).  The result is
 * a frame pointer that needs to be turned into a frame with v8dbg_frame().
 * Note that there's no function for fetching the first frame -- that's a
 * native-debugger-specific function, since it involves fetching registers.
 * (Users may also want to specify their own first frame, if they have a frame
 * pointer from some other analysis.)
 *
 * This function is only appropriate for systems that always push a frame
 * pointer onto the stack (e.g., using "-fno-omit-frame-pointer").  For systems
 * that walk stacks using other means, you'll need to use that mechanism to get
 * the next frame pointer.
 */
int v8dbg_frame_next(v8dbg_t *, v8dbg_frame_t, tgtaddr_t);

/*
 * Native stack frames
 */
/* Fetch the address in memory of the function associated with this frame. */
int v8dbg_frame_pc(v8dbg_t *, v8dbg_frame_t, tgtaddr_t *);

/*
 * JavaScript stack frames
 */
/* Fetch the JavaScript function associated with the frame */
int v8dbg_frame_jsfunc(v8dbg_t *, v8dbg_frame_t, tgtaddr_t *);
/* Fetch the number of arguments in the frame. */
int v8dbg_frame_nargs(v8dbg_t *, v8dbg_frame_t, ssize_t *);
/* Fetch "this" value in the frame. */
int v8dbg_frame_this(v8dbg_t *, v8dbg_frame_t, tgtaddr_t *);
/* Fetch argument N of the frame (1-based, no bounds checking). */
int v8dbg_frame_arg(v8dbg_t *, v8dbg_frame_t, unsigned int, tgtaddr_t *);


/*
 * Inspecting JavaScript-level state
 *
 * Reminder: as described above, these functions take a tgtaddr_t that is
 * assumed to be of a valid type.  They return an error if they're given the
 * wrong type.
 */

typedef enum {
    V8JST_NONE,		/* definitely not a JavaScript value */
    V8JST_JSUNKNOWN,	/* unknown or unsupported JavaScript value type */
    V8JST_JSUNDEFINED,	/* undefined */
    V8JST_JSNULL,	/* null */
    V8JST_JSHOLE,	/* special "hole" value */
    V8JST_JSBOOLEAN,	/* true or false */
    V8JST_JSSMI,	/* small integer (can be converted to int) */
    V8JST_JSHEAPNUMBER, /* non-SMI number (can be converted to double) */
    V8JST_JSDATE,	/* Date instance */
    V8JST_JSREGEXP,	/* regular expression */
    V8JST_JSSTRING,	/* string */
    V8JST_JSARRAY,	/* array */
    V8JST_JSOBJECT,	/* object */
    V8JST_JSFUNCTION	/* function */
} v8dbg_jstype_t;

int v8dbg_jsval_type(v8dbg_t *, tgtaddr_t, v8dbg_jstype_t);
const char *v8dbg_jsval_type_tostring(v8dbg_jstype_t);

/*
 * Primitive values
 *
 * Fetch the raw value for boolean, small number, and heap number types.
 */
int v8dbg_jsboolean_value(v8dbg_t *, tgtaddr_t, boolean_t *);
/* XXX is this the right type? */
int v8dbg_jssmi_value(v8dbg_t *, tgtaddr_t, long *);
int v8dbg_jsheapnumber_value(v8dbg_t *, tgtaddr_t, double *);

/*
 * Dates
 *
 * Get the epoch timestamp (in milliseconds) represented by the given Date
 * object.  The result of this is a value that may be either V8JST_JSSMI or
 * V8JST_JSHEAPNUMBER.
 */
int v8dbg_jsdate_timestamp(v8dbg_t *, tgtaddr_t, tgtaddr_t *);
/* Returns whether the date represented is a valid date. */
int v8dbg_jsdate_valid(v8dbg_t *, tgtaddr_t, boolean_t *);
/*
 * Same as above, but returns the value as a struct timeval, regardless of
 * whether the underlying value is an SMI or HeapNumber.  This returns an error
 * if the Date is not valid.
 */
int v8dbg_jsdate_timeval(v8dbg_t *, tgtaddr_t, struct timeval *);

/*
 * Regular expressions
 *
 * Returns the "source" for the RegExp, which is generally a string.
 */
int v8dbg_jsregexp_source(v8dbg_t *, tgtaddr_t, tgtaddr_t *);

/*
 * Strings
 */
/* Returns the length of the string (which is itself another JS value). */
int v8dbg_jsstring_length(v8dbg_t *, tgtaddr_t, tgtaddr_t *);
/* Returns the length of the string as a native integer. */
int v8dbg_jsstring_length_native(v8dbg_t *, tgtaddr_t, long *);
/*
 * Print the contents of the JavaScript string to the given buffer.  The result
 * is always NULL-terminated.
 */
int v8dbg_jsstring_print(v8dbg_t *, tgtaddr_t, char *, size_t);
/* Same, but allocates memory. */
int v8dbg_jsstring_copy(v8dbg_t *, tgtaddr_t, char **);
int v8dbg_jsstring_free(v8dbg_t *, char *);

/*
 * Arrays
 */
/* Returns the length of the array (which is itself another JS value). */
int v8dbg_jsarray_length(v8dbg_t *, tgtaddr_t, tgtaddr_t *);
/* Returns the length of the array as a native integer. */
int v8dbg_jsarray_length_native(v8dbg_t *, tgtaddr_t, long *);
/* Fetches item i from the array (0-indexed) */
int v8dbg_jsarray_item(v8dbg_t *, tgtaddr_t, unsigned int, tgtaddr_t *);

/*
 * Objects
 */
typedef enum {
	V8IP_DEFAULT
} v8dbg_iter_prop_flags_t;
typedef struct v8dbg_prop *v8dbg_prop_t;
int v8dbg_jsobj_iter_properties(v8dbg_t *, tgtaddr_t, v8dbg_iter_prop_flags_t,
    int (*)(v8dbg_t, tgtaddr_t, v8dbg_prop_t *, void *), void *);
int v8dbg_prop_name(v8dbg_prop_t *, tgtaddr_t *);
int v8dbg_prop_value(v8dbg_prop_t *, tgtaddr_t *);

typedef enum {
	V8P_DEFAULT
} v8dbg_prop_flags_t;
int v8dbg_jsobject_property_value(v8dbg_prop_t *, tgtaddr_t, v8dbg_prop_flags_t,
    const char *, tgtaddr_t *);
int v8dbg_jsobject_property_pluck(v8dbg_prop_t *, tgtaddr_t, v8dbg_prop_flags_t,
    const char *, char, tgtaddr_t *);

/* Return the constructor (a jsfunction) */
int v8dbg_jsobject_constructor(v8dbg_t *, tgtaddr_t, tgtaddr_t *);


/*
 * Functions
 */
int v8dbg_jsfunction_name(v8dbg_t *, tgtaddr_t, tgtaddr_t *);
int v8dbg_jsfunction_script_path(v8dbg_t *, tgtaddr_t, tgtaddr_t *);
int v8dbg_jsfunction_source(v8dbg_t *, tgtaddr_t, unsigned int /* nlines */,
    tgtaddr_t *,	/* return string */
    unsigned int *,	/* start position in string */
    unsigned int *);	/* end position in string */
int v8dbg_jsfunction_script_source(v8dbg_t *, tgtaddr_t, tgtaddr_t *);
int v8dbg_jsfunction_code_start(v8dbg_t *, tgtaddr_t, tgtaddr_t *);
int v8dbg_jsfunction_code_end(v8dbg_t *, tgtaddr_t, tgtaddr_t *);


/*
 * Inspecting Node-level state
 */
int v8dbg_nodeobject_buffer_addr(v8dbg_t *, tgtaddr_t, tgtaddr_t *);


/*
 * Inspecting V8-level state
 *
 * These are all only intended for engineers debugging V8-level issues
 * (including this library itself).  These are not the appropriate level of
 * abstraction for JavaScript developers.  As a result, these interfaces are
 * less general, and more intended for human consumption.
 */

/* Return a human-readable summary of this value. */
int v8dbg_v8obj_label(v8dbg_t *, tgtaddr_t, char *, size_t);

/* Return whether the object looks like a reasonably well-formed V8 value. */
int v8dbg_v8obj_maybe_garbage(v8dbg_t *, tgtaddr_t, boolean_t *);

/*
 * V8 object types: as described above, this interface is primarily for humans,
 * so it does not expose anything close to the complete list of V8 types, but
 * rather the types that are useful for developers of debugger modules.  The
 * more V8 types are encoded here, the more brittle this library becomes with
 * changes to V8.
 */
typedef enum {
	V8T_UNKNOWN,		/* unrecognized or unsupported type */
	V8T_SMI,		/* small integer */
	V8T_ARRAY,		/* FixedArray */
	V8T_STRING,		/* some kind of string */
	V8T_OTHER		/* other supported type */
} v8dbg_v8type_t;

int v8dbg_v8obj_type(v8dbg_t *, tgtaddr_t, v8dbg_v8type_t *);

int v8dbg_v8smi_value(v8dbg_t *, tgtaddr_t, unsigned long /* XXX */);
int v8dbg_v8obj_class(v8dbg_t *, tgtaddr_t, char *, size_t);
int v8dbg_v8obj_field_named(v8dbg_t *, tgtaddr_t, const char *, const char *,
    tgtaddr_t *);
int v8dbg_v8obj_field_internal(v8dbg_t *, tgtaddr_t, unsigned int,
    tgtaddr_t *);
int v8dbg_v8str_length(v8dbg_t *, tgtaddr_t, size_t *);

typedef enum {
	V8E_ASCII,
	V8E_TWOBYTE
} v8dbg_encoding_t;
int v8dbg_v8str_encoding(v8dbg_t *, tgtaddr_t, v8dbg_encoding_t);

typedef enum {
	V8R_SEQ,
	V8R_CONS,
	V8R_EXTERNAL,
	V8R_SLICED
} v8dbg_representation_t;
int v8dbg_v8str_representation(v8dbg_t *, tgtaddr_t, v8dbg_representation_t);
int v8dbg_v8str_seq_print(v8dbg_t *, char, size_t);
int v8dbg_v8str_cons_parts(v8dbg_t *, tgtaddr_t, tgtaddr_t *, tgtaddr_t *);
int v8dbg_v8str_external_addr(v8dbg_t *, tgtaddr_t, tgtaddr_t *);
int v8dbg_v8str_sliced_str(v8dbg_t *, tgtaddr_t);
int v8dbg_v8str_sliced_offset(v8dbg_t *, tgtaddr_t, tgtaddr_t *);

int v8dbg_v8array_length(v8dbg_t *, tgtaddr_t, size_t *);
int v8dbg_v8array_copyin(v8dbg_t *, tgtaddr_t, tgtaddr_t **, size_t *);
int v8dbg_v8array_free(v8dbg_t *, tgtaddr_t *, size_t);

/*
 * Reporting V8 configuration
 */
int v8dbg_cfg_iter_classes(v8dbg_t *,
    int (*)(v8dbg_t *, const char *, const char *, void *), void *);
int v8dbg_cfg_iter_fields(v8dbg_t *, const char *,
    int (*)(v8dbg_t *, const char *, ptrdiff_t, void *), void *);
int v8dbg_cfg_iter_frametypes(v8dbg_t *,
    int (*)(v8dbg_t *, const char *, tgtaddr_t));

/*
 * Updating V8 configuration
 */
int v8dbg_cfg_configure(v8dbg_t *);
int v8dbg_cfg_load(v8dbg_t *, const char *);
int v8dbg_cfg_define_field(v8dbg_t *, const char *, const char *, tgtaddr_t);


/*
 * findjsobjects low-level interface: iterate every possible V8 value in the
 * target's address space.
 */
typedef struct v8dbg_mapping v8dbg_mapping_t;
typedef enum {
	V8AS_DEFAULT
} v8dbg_iter_as_flags_t;
int v8dbg_iter_as(v8dbg_t *, v8dbg_iter_as_flags_t,
    int (*)(v8dbg_t *, tgtaddr_t *, void *), void *);


/*
 * findjsobjects high-level interface: bucketize all found values by shape.
 */
int v8dbg_jsheap_scan(v8dbg_t *);
int v8dbg_jsheap_scan_reset(v8dbg_t *);

typedef struct {
	/* XXX */
	int dummy;
} v8dbg_heapstat_t;
int v8dbg_jsheap_scan_stats(v8dbg_t *, v8dbg_heapstat_t);

/*
 * findjsobjects querying interfaces
 */
int v8dbg_jsheap_iter_buckets(v8dbg_t *,
    int (*)(v8dbg_t *, tgtaddr_t, void *), void *);
int v8dbg_jsheap_iter_bucket(v8dbg_t *,
    int (*)(v8dbg_t *, tgtaddr_t, tgtaddr_t, void *), void *);

#endif /* !_LIBV8DBG_H_ */
