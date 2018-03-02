/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * tst.jsfindrefs.js: exercises the ::jsfindrefs dcmd.  See
 * test/standalone/tst.common.js for information about how this works.
 */

var assert = require('assert');
var jsprim = require('jsprim');
var util = require('util');
var VError = require('verror');

var common = require('./common');

/*
 * Construct an object graph that represents a variety of cases.  We want to
 * make sure there are some loops, object property references, array element
 * references, and closure variable references.
 *
 * TODO test cases to add:
 * - following an object back through:
 *   - a property reference
 *     - dictionary-based
 *     - in-object-based
 *     - "properties" array based
 *   - an array reference
 *   - a closure reference
 *   - a regular expression reference (e.g., pointing to a string)
 *   - a date reference (e.g., pointing to a heap number)
 *   - an oddball reference (e.g., for the strings "null", "undefined", etc.)
 *   - a bound function reference
 *   - I think the following are not possible: references via:
 *     - heap number, mutable heap number
 * - loops in the graph -- does this matter?
 * - edge cases:
 *   - something where there are no references
 *   - something where we find no references after hitting the max depth
 *   - something where we find references to non-FixedArrays
 */
var aString = '^regular expression!$';
var testObject = {
    'aDateWithHeapNumber': new Date(0),
    'aString': aString,
    'aRegExp': new RegExp(aString),
    'aBigObject': {},
    'aSubObject': {},
    'anArray': [ 16, 32, 64, 96, aString ],
    'aSlicedString': '0123456789012345678901234567890123456789'.slice(1, 37),
    'aConsString': aString.concat('boom'),
    'aClosure': function leakClosureVariables() {
	/* This closure should have a reference to aString. */
	console.log(aString);
    },
    'aNull': null,
    'anUndefined': undefined,
    'aTrue': true,
    'aFalse': false
};

function main()
{
	var testFuncs, i;

	/*
	 * Finish initializing our test object.
	 */
	/* Create a circular reference via a closure variable. */
	testObject['aClosure2'] = function leakAnotherVar() {
		console.log(testObject['aSubObject']);
	};
	/* Create a circular reference via the array. */
	testObject['anArray'].push(testObject);
	/* Flesh out a large object. */
	for (i = 0; i < 128; i++) {
		testObject['aBigObject']['prop_' + i] = 'str_' + i;
	}

	testFuncs = [];

	testFuncs.push(function (mdb, callback) {
		mdb.checkMdbLeaks(callback);
	});

	common.finalizeTestObject(testObject);
	common.standaloneTest(testFuncs, function (err) {
		if (err) {
			throw (err);
		}

		console.log('%s passed', process.argv[1]);
	});
}

main();
