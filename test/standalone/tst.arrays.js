/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * tst.arrays.js: This test case generates a number of arrays of various sizes,
 * finds them with "findjsobjects", then prints them out with both "jsprint" and
 * "jsarray".
 *
 * Like most of the standalone tests, this test works by creating a bunch of
 * structures in memory, using gcore(1M) to save a core file of the current
 * process, and then using an MDB session against the core file to pull out
 * those structures and verify that the debugger interprets them correctly.
 */

var assert = require('assert');
var util = require('util');

var common = require('./common');

/*
 * Test cases will be automatically generated containing arrays of the following
 * lengths.
 */
var testCaseLengths = [
    0,		/* important special case */
    1,		/* important special case */
    3,		/* a few elements */

    1023,	/* boundary cases in the code */
    1024,
    1025,

    4095,	/* possible boundary case in the code */
    4096,
    4097,

    100000	/* large array */
];

/*
 * Array of asynchronous test functions, extended in main() for
 * automatically-generated test cases.
 */
var testFuncs = [
    findTestObjectAddr,
    findArrayAddrs
];

/*
 * "testObject" is the root object from which we will hang the objects used for
 * our test cases.  That way once we find it with "findjsobjects", we can easily
 * find all of the test case objects.
 */
var testObject = {};

/*
 * Addresses found in the core file for "testObject" itself as well as the
 * arrays hanging off of it.
 */
var testObjectAddr;
var testArrayAddrs = {};

function main()
{
	var arr, i;

	/*
	 * Generate actual test cases for the array lengths configured above.
	 * Each test case consists of a single array stored into
	 * testObject['array_N'], where N is the length of the array.  The
	 * contents of the array are strings named for the array itself and
	 * which element they are so that it's pretty clear which output belongs
	 * to what.  (This test would use less memory and run faster if we just
	 * used integers, but it's harder to know that the test is doing the
	 * right thing.)
	 */
	testCaseLengths.forEach(function (targetLength) {
		var key;

		key = util.format('array_%d', targetLength);
		assert.ok(!testObject.hasOwnProperty(key),
		    'duplicate test for array of length ' + targetLength);
		arr = testObject[key] = [];
		for (i = 0; i < targetLength; i++) {
			arr.push(eltvalue(key, i));
		}

		testFuncs.push(testArrayJsprint.bind(null, targetLength));
		testFuncs.push(testArrayJsarray.bind(null, targetLength));
	});

	/*
	 * Generate tests for more exotic cases.  First, an array with a
	 * pre-defined length, where not all of the elements were specified.
	 */
	arr = testObject['array_predefined_length'] = new Array(8);
	for (i = 1; i < 6; i++) {
		arr[i] = eltvalue('array_predefined_length', i);
	}
	testFuncs.push(testPredefinedLengthJsprint);
	testFuncs.push(testPredefinedLengthJsarray);

	/*
	 * Now, an array with a hole in it.
	 */
	arr = testObject['array_with_hole'] = new Array(7);
	for (i = 0; i < 7; i++) {
		arr[i] = eltvalue('array_with_hole', i);
	}
	delete (arr[3]);
	testFuncs.push(testHoleJsprint);
	testFuncs.push(testHoleJsarray);

	testFuncs.push(function (mdb, callback) {
		mdb.checkMdbLeaks(callback);
	});

	/*
	 * This is a little cheesy, but we set this property to a boolean value
	 * immediately before saving the core file to minimize the chance that
	 * when we go look for this object that we'll find several other garbage
	 * objects having the same property with the same value (because they've
	 * been copied around by intervening GC operations).
	 */
	testObject['testObjectFinished'] = true;

	common.standaloneTest(testFuncs, function (err) {
		if (err) {
			throw (err);
		}

		console.log('%s passed', process.argv[1]);
	});
}

/*
 * Given a test array's name and which element we're looking at, return the
 * expected value of that element in that array.
 */
function eltvalue(name, i)
{
	assert.equal(typeof (name), 'string');
	assert.equal(typeof (i), 'number');
	return (util.format('%s_element_%d', name, i));
}

/*
 * From the core file, finds the address of "testObject" for use in subsequent
 * phases.
 */
function findTestObjectAddr(mdb, callback) {
	var cmdstr;

	cmdstr = '::findjsobjects -p testObjectFinished | ::findjsobjects | ' +
	    '::jsprint -b testObjectFinished\n';
	mdb.runCmd(cmdstr, function (output) {
		var lines, li, parts;

		lines = output.split('\n');
		assert.strictEqual(lines[lines.length - 1].length, 0,
		    'last line was not empty');

		for (li = 0; li < lines.length - 1; li++) {
			parts = lines[li].split(':');
			if (parts.length == 2 && parts[1] == ' true') {
				if (testObjectAddr !== undefined) {
					/*
					 * We've probably found a garbage object
					 * that's convincing enough that we
					 * can't tell that it's wrong.
					 */
					callback(new Error(
					    'found more than one possible ' +
					    'test object'));
					return;
				}

				testObjectAddr = parts[0];
			}
		}

		if (testObjectAddr === undefined) {
			callback(new Error('did not find test object'));
		} else {
			console.error('test object: ', testObjectAddr);
			callback();
		}
	});
}

/*
 * From the core file, finds the addresses for each of the test arrays in
 * "testObject" for use in subsequent phases.
 */
function findArrayAddrs(mdb, callback) {
	var cmdstr;

	assert.equal(typeof (testObjectAddr), 'string');
	cmdstr = util.format('%s::jsprint -ad1\n', testObjectAddr);
	mdb.runCmd(cmdstr, function (output) {
		var lines, i, parts, key, ptr;

		lines = output.split('\n');
		assert.equal(lines[0].trim(), testObjectAddr + ': {');
		assert.equal(lines[lines.length - 2].trim(), '}');
		assert.strictEqual(lines[lines.length - 1].length, 0);

		for (i = 1; i < lines.length - 2; i++) {
			if (!/^    "array_/.test(lines[i])) {
				continue;
			}

			parts = lines[i].trim().split(':');
			assert.equal(parts.length, 3,
			    'unexpected form of output');
			key = parts[0].trim();
			assert.equal(key.charAt(0), '"');
			assert.equal(key.charAt(key.length - 1), '"');
			key = key.substr(1, key.length - 2);
			ptr = parts[1].trim();

			console.error('address of %s: %s', key, ptr);
			testArrayAddrs[key] = ptr;
		}

		callback();
	});
}

/*
 * Invoked for each of the test cases generated from "testCaseLengths" to verify
 * the "::jsprint" output for the array.  "::jsprint" output is not really
 * intended for programmatic stability, so this test case may need to evolve in
 * the future, but it's still worthwhile to verify that these various arrays are
 * actually printed properly.
 */
function testArrayJsprint(targetLength, mdb, callback)
{
	var keyname, cmdstr;

	keyname = util.format('array_%d', targetLength);
	assert.equal(typeof (testObjectAddr), 'string');
	assert.equal(typeof (testArrayAddrs[keyname]), 'string',
	    'did not find address of array ' + keyname);
	cmdstr = util.format('%s::jsprint\n', testArrayAddrs[keyname]);
	mdb.runCmd(cmdstr, function (output) {
		var lines, i, expected;

		/*
		 * Arrays of length 0 and 1 are printed more concisely, so we
		 * check for those explicitly.
		 */
		if (targetLength === 0) {
			assert.equal(output, '[]\n',
			    'zero element array output');
			callback();
			return;
		}

		if (targetLength == 1) {
			assert.equal(output,
			    util.format('[ "%s" ]\n', eltvalue(keyname, 0)));
			callback();
			return;
		}

		lines = output.split('\n');
		assert.equal(lines[0].trim(), '[',
		    'unexpected first row of ::jsarray output on array');
		assert.equal(lines[lines.length - 2].trim(), ']',
		    'unexpected last row of ::jsarray output on array');
		assert.equal(lines[lines.length - 1].length, 0,
		    'last line was not empty');
		assert.equal(lines.length, targetLength + 3,
		    'unexpected number of array elements');

		for (i = 0; i < targetLength; i++) {
			expected = util.format('    "%s",',
			    eltvalue(keyname, i));
			assert.equal(lines[i + 1], expected);
		}

		callback();
	});
}

/*
 * Invoked for each of the test cases generated from "testCaseLengths" to verify
 * the "::jsarray" output for the array.  "::jsarray" primarily exists to be a
 * bare-bones array iterator, so this test case should not require significant
 * changes over time.
 */
function testArrayJsarray(targetLength, mdb, callback)
{
	var keyname, cmdstr;

	keyname = util.format('array_%d', targetLength);
	assert.equal(typeof (testObjectAddr), 'string');
	assert.equal(typeof (testArrayAddrs[keyname]), 'string',
	    'did not find address of array ' + keyname);
	cmdstr = util.format('%s::jsarray | ::jsprint\n',
	    testArrayAddrs[keyname]);
	mdb.runCmd(cmdstr, function (output) {
		var lines, i;

		lines = output.split('\n');
		assert.ok(lines.length > 0);
		assert.strictEqual(lines[lines.length - 1].length, 0,
		    'last line was not empty');
		assert.strictEqual(lines.length, targetLength + 1,
		    'unexpected number of lines');
		for (i = 0; i < targetLength; i++) {
			assert.equal(lines[i],
			    '"' + eltvalue(keyname, i) + '"');
		}

		callback();
	});
}

/*
 * Verifies the "jsprint" output for the array with predefined length.
 */
function testPredefinedLengthJsprint(mdb, callback)
{
	var keyname, cmdstr;

	keyname = 'array_predefined_length';
	assert.equal(typeof (testObjectAddr), 'string');
	assert.equal(typeof (testArrayAddrs[keyname]), 'string',
	    'did not find address of array ' + keyname);
	cmdstr = util.format('%s::jsprint\n', testArrayAddrs[keyname]);
	mdb.runCmd(cmdstr, function (output) {
		/*
		 * We expect special "hole" values for the elements that were
		 * not set.
		 */
		assert.equal(output, [
		    '[',
		    '    hole,',
		    '    "array_predefined_length_element_1",',
		    '    "array_predefined_length_element_2",',
		    '    "array_predefined_length_element_3",',
		    '    "array_predefined_length_element_4",',
		    '    "array_predefined_length_element_5",',
		    '    hole,',
		    '    hole,',
		    ']',
		    ''
		].join('\n'));
		callback();
	});
}

/*
 * Verifies the "jsarray" output for the array with predefined length.
 */
function testPredefinedLengthJsarray(mdb, callback)
{
	var keyname, cmdstr;

	keyname = 'array_predefined_length';
	assert.equal(typeof (testObjectAddr), 'string');
	assert.equal(typeof (testArrayAddrs[keyname]), 'string',
	    'did not find address of array ' + keyname);
	cmdstr = util.format('%s::jsarray | ::jsprint\n',
	    testArrayAddrs[keyname]);
	mdb.runCmd(cmdstr, function (output) {
		/* See "::jsprint" analog above. */
		assert.equal(output, [
		    'hole',
		    '"array_predefined_length_element_1"',
		    '"array_predefined_length_element_2"',
		    '"array_predefined_length_element_3"',
		    '"array_predefined_length_element_4"',
		    '"array_predefined_length_element_5"',
		    'hole',
		    'hole',
		    ''
		].join('\n'));
		callback();
	});
}

/*
 * Verifies the "jsprint" output for the array with a deleted element.
 */
function testHoleJsprint(mdb, callback)
{
	var keyname, cmdstr;

	keyname = 'array_with_hole';
	assert.equal(typeof (testObjectAddr), 'string');
	assert.equal(typeof (testArrayAddrs[keyname]), 'string',
	    'did not find address of array ' + keyname);
	cmdstr = util.format('%s::jsprint\n', testArrayAddrs[keyname]);
	mdb.runCmd(cmdstr, function (output) {
		/*
		 * We expect special "hole" values for elements that were
		 * deleted.
		 */
		assert.equal(output, [
		    '[',
		    '    "array_with_hole_element_0",',
		    '    "array_with_hole_element_1",',
		    '    "array_with_hole_element_2",',
		    '    hole,',
		    '    "array_with_hole_element_4",',
		    '    "array_with_hole_element_5",',
		    '    "array_with_hole_element_6",',
		    ']',
		    ''
		].join('\n'));
		callback();
	});
}

/*
 * Verifies the "jsarray" output for the array with a deleted element.
 */
function testHoleJsarray(mdb, callback)
{
	var keyname, cmdstr;

	keyname = 'array_with_hole';
	assert.equal(typeof (testObjectAddr), 'string');
	assert.equal(typeof (testArrayAddrs[keyname]), 'string',
	    'did not find address of array ' + keyname);
	cmdstr = util.format('%s::jsarray | ::jsprint\n',
	    testArrayAddrs[keyname]);
	mdb.runCmd(cmdstr, function (output) {
		/* See "::jsprint" analog above. */
		assert.equal(output, [
		    '"array_with_hole_element_0"',
		    '"array_with_hole_element_1"',
		    '"array_with_hole_element_2"',
		    'hole',
		    '"array_with_hole_element_4"',
		    '"array_with_hole_element_5"',
		    '"array_with_hole_element_6"',
		    ''
		].join('\n'));
		callback();
	});

}

main();
