#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

#
# Copyright (c) 2015, Joyent, Inc.
#

#
# Working Makefile for V8 debugging library.  This should eventually be folded
# into the top-level Makefile.
#


BUILDDIR		 = build
PROGNAME		 = $(BUILDDIR)/v8dbg_driver

SRCDIR			 = src
CSOURCES		 = v8dbg_driver.c

CFLAGS			+= -Werror -Wall -Wextra -fPIC -fno-omit-frame-pointer
CPPFLAGS		+= -I./include
LDFLAGS			+= -lavl

# Try to keep this code as portable as possible.
CPPFLAGS		+= -D_XOPEN_SOURCE=600 -std=c99 -pedantic

CSTYLE			 = ../tools/cstyle.pl
CSTYLE_FLAGS		+= -cCp
CSTYLE_SOURCES	 	 = $(wildcard include/*.h src/*.h src/*.c)

CATEST			 = ../tools/catest

JS_FILES		 =
JSL_FILES_NODE		 = $(JS_FILES)
JSSTYLE_FILES		 = $(JS_FILES)
JSL_CONF_NODE		 = ../tools/jsl.node.conf

#BUILD_AMD64=amd64
ifeq ($(BUILD_AMD64),amd64)
	CFLAGS	+= -m64
	LDFLAGS	+= -m64
endif

# This is deeply regrettable.
ifeq ($(shell uname -s),SunOS)
	CFLAGS += -D_HAVE_BOOLEAN_T
endif

MKDIRP			 = mkdir -p $@
COMPILE.c		 = $(CC) -o $@ -c $(CFLAGS) $(CPPFLAGS) $^
MAKESO	 		 = $(CC) -o $@ -shared $(SOFLAGS) $(LDFLAGS) $^
MAKEPROG		 = $(CC) -o $@ $(LDFLAGS) $^

.PHONY: all
all: $(PROGNAME)

check: check-cstyle

.PHONY: check-cstyle
check-cstyle: 
	$(CSTYLE) $(CSTYLE_FLAGS) $(CSTYLE_SOURCES)

CLEAN_FILES += $(BUILDDIR)

OBJFILES = $(CSOURCES:%.c=$(BUILDDIR)/%.o)

$(PROGNAME): $(OBJFILES)
	$(MAKEPROG)

$(OBJFILES): $(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(COMPILE.c)

$(BUILDDIR):
	$(MKDIRP)

# Include common Joyent Makefile for JavaScript "check" targets.
include ../Makefile.targ
