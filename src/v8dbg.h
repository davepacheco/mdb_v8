/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

/*
 * v8dbg.h: macros for use by V8 heap inspection tools.  The consumer must
 * define values for various tags and shifts.  The MDB module gets these
 * constants from information encoded in the binary itself.
 */

#ifndef _V8DBG_H
#define	_V8DBG_H

/*
 * Recall that while V8 heap objects are always 4-byte aligned, heap object
 * pointers always have the last bit set.  So when looking for a field nominally
 * at offset X, one must be sure to clear the tag bit first.
 */
#define	V8_OFF_HEAP(x)			((x) - V8_HeapObjectTag)

/*
 * Determine whether a given pointer refers to a SMI, Failure, or HeapObject.
 */
#define	V8_IS_SMI(ptr)		(((ptr) & V8_SmiTagMask) == V8_SmiTag)
#define	V8_IS_FAILURE(ptr)	(V8_FailureTagMask != -1 && \
	V8_FailureTagMask != -1 && \
	((ptr) & V8_FailureTagMask) == V8_FailureTag)

#define	V8_IS_HEAPOBJECT(ptr)	\
	(((ptr) & V8_HeapObjectTagMask) == V8_HeapObjectTag)

/*
 * Extract the value of a SMI "pointer".  Recall that small integers are stored
 * using the upper 31 bits.
 */
#define	V8_SMI_VALUE(smi)	((smi) >> (V8_SmiValueShift + V8_SmiShiftSize))
#define	V8_VALUE_SMI(value)	\
	((value) << (V8_SmiValueShift + V8_SmiShiftSize))

/*
 * Check compiler hints, which hang off of SharedFunctionInfo objects.
 */
#define	V8_HINT_ISSET(hints, whichbit) \
	(((hints) & (1 << (whichbit))) != 0)
#define	V8_HINT_BOUND(hints) \
	(V8_HINT_ISSET((hints), V8_CompilerHints_BoundFunction))

/*
 * Determine the encoding and representation of a V8 string.
 */
#define	V8_TYPE_STRING(type)	(((type) & V8_IsNotStringMask) == V8_StringTag)

#define	V8_STRENC_ASCII(type)	\
	((V8_AsciiStringTag != -1 && \
		((type) & V8_StringEncodingMask) == V8_AsciiStringTag) || \
	(V8_OneByteStringTag != -1 && \
		((type) & V8_StringEncodingMask) == V8_OneByteStringTag))

#define	V8_STRREP_SEQ(type)	\
	(((type) & V8_StringRepresentationMask) == V8_SeqStringTag)
#define	V8_STRREP_CONS(type)	\
	(((type) & V8_StringRepresentationMask) == V8_ConsStringTag)
#define	V8_STRREP_SLICED(type)	\
	(((type) & V8_StringRepresentationMask) == V8_SlicedStringTag)
#define	V8_STRREP_EXT(type)	\
	(((type) & V8_StringRepresentationMask) == V8_ExternalStringTag)

/*
 * Several of the following constants and transformations are hardcoded in V8 as
 * well, so there's no way to extract them programmatically from the binary.
 */
#define	V8_DESC_KEYIDX(x)		((x) + V8_PROP_IDX_FIRST)
#define	V8_DESC_VALIDX(x)		((x) << 1)
#define	V8_DESC_DETIDX(x)		(((x) << 1) + 1)

#define	V8_DESC_ISFIELD(x)		\
	((V8_SMI_VALUE(x) & V8_PROP_TYPE_MASK) == V8_PROP_TYPE_FIELD)

#define	V8_PROP_FIELDINDEX(value)	\
	((V8_SMI_VALUE(value) & V8_PROPINDEX_MASK) >> V8_PROPINDEX_SHIFT)

#endif /* _V8DBG_H */
