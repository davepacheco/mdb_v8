/*
 * XXX see notes.txt
 */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

var assert = require('assert');
var util = require('util');

var common = require('./common');

/*
 * Generate some variously-sized arrays.
 */

var itemi, nitems = 100000;
var testObject = {
    'bigArray': []
};
for (itemi = 0; itemi < nitems; itemi++) {
	testObject.bigArray.push(nitems - itemi);
}

var bigArrayPtrs;
common.standaloneTest([
    function findBigArray(mdb, callback) {
	mdb.runCmd('::findjsobjects -p bigArray | ::findjsobjects | ' +
	    '::jsprint -ad0 bigArray\n', function (output) {
		bigArrayPtrs = output.split('\n').filter(function (l) {
			return (l.length > 0);
		}).map(function (l) {
			return (l.split(':')[0].trim());
		});
		assert.ok(bigArrayPtrs.length > 0);
		callback();
	});
    },

    function printBigArray(mdb, callback) {
	var cmdstr;

	cmdstr = bigArrayPtrs.map(function (c) {
		return (util.format('%s::jsarray', c));
	}).join(';') + '\n';
	mdb.runCmd(cmdstr, function (output) {
		var lines = output.split('\n');
		assert.equal(nitems + 1, lines.length);

		/*
		 * This relies on the representation of SMI values, but it's an
		 * easy way to programmatically verify the correctness of this
		 * large array.
		 */
		lines.forEach(function (l, i) {
			if (i == lines.length - 1) {
				assert.equal(l.length, 0);
			} else {
				assert.equal(l,
				    (2 * (nitems - i)).toString(16));
			}
		});

		callback();
	});
    }
], function (err) {
	if (err) {
		throw (err);
	}

	console.log('%s passed', process.argv[1]);
});
