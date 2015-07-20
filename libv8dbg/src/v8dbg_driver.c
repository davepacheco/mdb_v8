/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * v8dbg_driver.c: placeholder program to drive libv8dbg
 */

#include <stdio.h>
#include <libv8dbg.h>

#define	UNUSED __attribute__((unused))

int
main(int argc UNUSED, char *argv[] UNUSED)
{
	(void) printf("it works\n");
	return (0);
}
