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
var strsplit = require('strsplit');
var util = require('util');
var vasync = require('vasync');
var VError = require('verror');

var common = require('./common');

/*
 * Construct an object graph that represents a variety of cases.  We want to
 * make sure we cover object property references, array element references,
 * closure variable references, and references within built-in types (like bound
 * functions, regular expressions, and so on).
 *
 * Initialization of the main test object happens inside init() in order to
 * avoid closures picking up an additional reference to all the other top-level
 * properties.
 */
var testObject;			/* used to find all values of interest */
var testObjectAddr;		/* address (in core file) of "testObject" */
var testAddrs = {};		/* addresses of "testObject" values */
var bigObjectAddrs = {};	/* addresses of "bigObject" values */
var simpleProps = [
    'aRegExp',
    'aBigObject',
    'aSubObject',
    'anArray',
    'aSlicedString',
    'aConsString',
    'aClosure'
];

function init()
{
	var aString = '^regular expression!$';
	var aLongerString = '0123456789012345678901234567890123456789';
	var aDummyString = 'dummy';
	var i;

	testObject = {
	    'aString': aString,
	    'aDummyString': aDummyString,
	    'aLongerString': aLongerString,
	    'aRegExp': new RegExp(aString),
	    'aBigObject': {},
	    'aSubObject': {},
	    'anArray': [ 16, 32, 64, 96, aString ],
	    'aSlicedString': aLongerString.slice(1, 37),
	    'aConsString': aString.concat('boom'),
	    'aClosure': function leakClosureVariables() {
		/* This closure should have a reference to aDummyString. */
		console.log(aDummyString);
	    },
	    'aBoundFunction': main.bind(null, aString),
	    'aClosure2': null, /* set later */
	    'aNull': null,
	    'anUndefined': undefined,
	    'aTrue': true,
	    'aFalse': false
	};

	/* Create a circular reference via the array. */
	testObject['anArray'].push(testObject);

	/* Flesh out a large object. */
	for (i = 0; i < 128; i++) {
		testObject['aBigObject']['prop_' + i] = 'str_' + i;
	}
}

function main()
{
	var testFuncs;

	init();

	testFuncs = [];
	testFuncs.push(function findTestObject(mdb, callback) {
		common.findTestObject(mdb, function gotTestObject(err, addr) {
			testObjectAddr = addr;
			callback(err);
		});
	});
	testFuncs.push(findTopLevelObjects);
	testFuncs.push(testPropsSimple);
	testFuncs.push(testPropsSimpleVerbose);
	testFuncs.push(testPropViaSlicedString);
	testFuncs.push(testPropAString);
	testFuncs.push(testPropADummyString);
	testFuncs.push(findBigObjectProperties);
	testFuncs.push(testBigObjectProp);
	testFuncs.push(testCycle);
	testFuncs.push(testBogusAddress);

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

/*
 * Locates the test addresses for each of the properties of "testObject".
 */
function findTopLevelObjects(mdb, callback)
{
	console.error('test: locating top-level property addresses');
	assert.equal('string', typeof (testObjectAddr));
	mdb.runCmd(testObjectAddr + '::jsprint -ad1\n', function (output) {
		var lines, count;
		var i, parts, propname, propaddr;

		count = 0;
		jsprim.forEachKey(testObject, function () { count++; });

		/*
		 * XXX The ::jsprint output, at least for 32-bit Node 0.10.48,
		 * appears not to contain the "aClosure2" property when it's not
		 * set inline at the top of the file.  This needs to be
		 * debugged.  It would help to have a dcmd that prints out
		 * property descriptors for an object.  We've needed that for
		 * ages.
		 */
		// count--;
		lines = common.splitMdbLines(output, { 'count': count + 2 });

		/*
		 * There are two extra lines in the output for the header and
		 * footer of the object.  These are deliberately skipped in this
		 * loop.
		 */
		for (i = 1; i < lines.length - 1; i++) {
			parts = strsplit.strsplit(lines[i], ':', 3);
			assert.equal(parts.length, 3);
			propname = JSON.parse(parts[0].trim());
			propaddr = parts[1];
			assert.ok(jsprim.hasKey(testObject, propname));
			assert.ok(!jsprim.hasKey(testAddrs, propname));
			testAddrs[propname] = propaddr.trim();
		}

		console.error(testAddrs);
		callback();
	});
}

/*
 * For each of the properties of "testObject" that are not referenced anywhere
 * else, use "::jsfindrefs" to find the one reference.  This only really
 * exercises the cases of values referenced via an object property.
 */
function testPropsSimple(mdb, callback)
{
	console.error('test: normal output for single-reference properties');
	vasync.forEachPipeline({
	    'inputs': simpleProps,
	    'func': function testOneSimpleProperty(propname, subcb) {
		var propaddr;

		assert.equal('string', typeof (testAddrs[propname]));
		propaddr = testAddrs[propname];
		mdb.runCmd(propaddr + '::jsfindrefs\n', function (output) {
			var lines;
			lines = common.splitMdbLines(output, { 'count': 1 });
			assert.equal(lines[0], testObjectAddr);
			subcb();
		});
	    }
	}, callback);
}

/*
 * Similar to "testPropsSimple", but this test exercises the verbose mode of
 * "::jsfindrefs".
 */
function testPropsSimpleVerbose(mdb, callback)
{
	console.error('test: verbose output for single-reference properties');
	vasync.forEachPipeline({
	    'inputs': simpleProps,
	    'func': function testOneSimpleProperty(propname, subcb) {
		var propaddr;

		assert.equal('string', typeof (testAddrs[propname]));
		propaddr = testAddrs[propname];
		mdb.runCmd(propaddr + '::jsfindrefs -v\n', function (output) {
			var lines, expected;
			lines = common.splitMdbLines(output, { 'count': 1 });
			expected = testObjectAddr + ' (type: JSObject)';
			assert.equal(lines[0], expected);
			subcb();
		});
	    }
	}, callback);
}

/*
 * Test that we can find references to objects via a SlicedString.  We use the
 * "aLongerString" property, which should have two references: one from our
 * main test object, and one from the "aSlicedString" object.
 */
function testPropViaSlicedString(mdb, callback)
{
	var addr;

	console.error('test: sliced string reference');
	assert.equal('string', typeof (testAddrs['aLongerString']));
	addr = testAddrs['aLongerString'];

	vasync.forEachPipeline({
	    'inputs': [
		addr + '::jsfindrefs ! sort\n',
		addr + '::jsfindrefs -v ! sort\n'
	    ],
	    'func': function runCmd(cmd, subcallback) {
		mdb.runCmd(cmd, function (output) {
			subcallback(null, output);
		});
	    }
	}, function (err, results) {
		var lines, expectedAddrs, expectedVerbose;

		assert.ok(!err);
		expectedAddrs = [
		    testObjectAddr,
		    testAddrs['aSlicedString']
		].sort();
		lines = common.splitMdbLines(results.operations[0].result,
		    { 'count': 2 });
		assert.equal(lines[0], expectedAddrs[0]);
		assert.equal(lines[1], expectedAddrs[1]);

		expectedVerbose = [
		    testObjectAddr + ' (type: JSObject)',
		    testAddrs['aSlicedString'] + ' (type: SlicedString)'
		].sort();
		lines = common.splitMdbLines(results.operations[1].result,
		    { 'count': 2 });
		assert.equal(lines[0], expectedVerbose[0]);
		assert.equal(lines[1], expectedVerbose[1]);

		callback();
	});
}

/*
 * Tests that we can find all the references to our special string, "aString".
 * This will exercise the ability to find string referneces via:
 *
 *   - common object property
 *   - regular expressions
 *   - ConsStrings
 *   - bound functions
 *   - array elements
 */
function testPropAString(mdb, callback)
{
	var addr;

	console.error('test: closure reference');
	assert.equal('string', typeof (testAddrs['aString']));
	addr = testAddrs['aString'];

	vasync.forEachPipeline({
	    'inputs': [
		/*
		 * This produces duplicates in some cases.  This would be nice
		 * to avoid, but it's not a high priority at the moment.
		 */
		addr + '::jsfindrefs ! sort -u\n',
		addr + '::jsfindrefs -v ! sort -u\n'
	    ],
	    'func': function runCmd(cmd, subcallback) {
		mdb.runCmd(cmd, function (output) {
			subcallback(null, output);
		});
	    }
	}, function (err, results) {
		var lines, expectedAddrs, expectedVerbose;

		assert.ok(!err);
		expectedAddrs = [
		    testObjectAddr,
		    testAddrs['aConsString'],
		    testAddrs['anArray'],
		    testAddrs['aRegExp'],
		    testAddrs['aBoundFunction']
		].sort();

		lines = common.splitMdbLines(results.operations[0].result,
		    { 'count': expectedAddrs.length });
		assert.deepEqual(lines, expectedAddrs);

		expectedVerbose = [
		    testObjectAddr + ' (type: JSObject)',
		    testAddrs['aConsString'] + ' (type: ConsString)',
		    testAddrs['anArray'] + ' (type: JSArray)',
		    testAddrs['aRegExp'] + ' (type: JSRegExp)',
		    testAddrs['aBoundFunction'] + ' (type: JSFunction)'
		].sort();

		lines = common.splitMdbLines(results.operations[1].result,
		    { 'count': expectedVerbose.length });
		assert.deepEqual(lines, expectedVerbose);
		callback();
	});
}

/*
 * Tests that we can find the references we expect to "aDummyString", which is
 * used only via a normal property reference and a closure variable.
 */
function testPropADummyString(mdb, callback)
{
	var addr;

	console.error('test: closure references');
	assert.equal('string', typeof (testAddrs['aDummyString']));
	addr = testAddrs['aDummyString'];

	vasync.forEachPipeline({
	    'inputs': [
		addr + '::jsfindrefs ! sort\n',
		addr + '::jsfindrefs -v ! sort\n'
	    ],
	    'func': function runCmd(cmd, subcallback) {
		mdb.runCmd(cmd, function (output) {
			subcallback(null, output);
		});
	    }
	}, function (err, results) {
		var lines, expectedAddrs, expectedVerbose;

		assert.ok(!err);
		expectedAddrs = [
		    testObjectAddr,
		    testAddrs['aClosure']
		].sort();

		lines = common.splitMdbLines(results.operations[0].result,
		    { 'count': expectedAddrs.length });
		assert.deepEqual(lines, expectedAddrs);

		expectedVerbose = [
		    testObjectAddr + ' (type: JSObject)',
		    testAddrs['aClosure'] + ' (type: JSFunction)'
		].sort();

		lines = common.splitMdbLines(results.operations[1].result,
		    { 'count': expectedVerbose.length });
		assert.deepEqual(lines, expectedVerbose);
		callback();
	});
}

/*
 * Locates the addresses of the property values inside our big object.
 */
function findBigObjectProperties(mdb, callback)
{
	console.error('test: locating big object properties');
	assert.equal('string', typeof (testAddrs['aBigObject']));
	mdb.runCmd(testAddrs['aBigObject'] + '::jsprint -ad1\n',
	    function (output) {
		var lines, count;
		var i, parts, propname, propaddr;

		count = 0;
		jsprim.forEachKey(testObject['aBigObject'],
		    function () { count++; });
		lines = common.splitMdbLines(output, { 'count': count + 2 });

		/*
		 * There are two extra lines in the output for the header and
		 * footer of the object.  These are deliberately skipped in this
		 * loop.
		 * XXX commonize with above.
		 */
		for (i = 1; i < lines.length - 1; i++) {
			parts = strsplit.strsplit(lines[i], ':', 3);
			assert.equal(parts.length, 3);
			propname = JSON.parse(parts[0].trim());
			propaddr = parts[1];
			assert.ok(jsprim.hasKey(testObject['aBigObject'],
			    propname));
			assert.ok(!jsprim.hasKey(bigObjectAddrs, propname));
			bigObjectAddrs[propname] = propaddr.trim();
		}

		console.error(bigObjectAddrs);
		callback();
	    });
}

/*
 * Tests finding a property value from our big object.  This is intended to
 * exercise a different case of object layout -- namely, dictionary layout --
 * than what is likely used for our main test object.
 *
 * This test is also used to verify that we cleanly stop iterating (having found
 * no references) if the depth is limited too far.  To test this, we run the
 * same test with depth limited to 1 and verify that we don't find any
 * reference.
 */
function testBigObjectProp(mdb, callback)
{
	var addr;

	console.error('test: big object property and depth limit');
	addr = bigObjectAddrs['prop_25'];
	assert.equal('string', typeof (addr));

	vasync.forEachPipeline({
	    'inputs': [
		addr + '::jsfindrefs\n',
		addr + '::jsfindrefs -l 1\n',
		addr + '::jsfindrefs -v\n'
	    ],
	    'func': function runCmd(cmd, subcallback) {
		mdb.runCmd(cmd, function (output) {
			subcallback(null, output);
		});
	    }
	}, function (err, results) {
		var lines;

		assert.ok(!err);
		lines = common.splitMdbLines(results.operations[0].result,
		    { 'count': 1 });
		assert.deepEqual(lines, [ testAddrs['aBigObject'] ]);

		assert.strictEqual('', results.operations[1].result);

		lines = common.splitMdbLines(results.operations[2].result,
		    { 'count': 1 });
		assert.deepEqual(lines,
		    [ testAddrs['aBigObject'] + ' (type: JSObject)' ]);
		callback();
	});
}

/*
 * Verifies that we don't spin into an infinite loop in the case of cycles in
 * the JavaScript object graph.  We test this by showing that we can find the
 * references to "testObject", which should include our array, and we can find
 * references to our test array, which should include "testObject".  This
 * demonstrates that we found the cycle and didn't loop forever trying to
 * process it.
 *
 * It would be nice to also test the case of cycles in the graph of V8 objects
 * that aren't also JavaScript objects (e.g., two FixedArrays pointing at each
 * other).  This is arguably a bigger risk, but it's harder to construct
 * instances of such structures.
 */
function testCycle(mdb, callback)
{
	console.error('test: cycle');
	assert.equal('string', typeof (testAddrs['anArray']));
	assert.equal('string', typeof (testObjectAddr));

	vasync.forEachPipeline({
	    'inputs': [
	        testObjectAddr + '::jsfindrefs\n',
		testAddrs['anArray'] + '::jsfindrefs\n'
	    ],
	    'func': function runCmd(cmd, subcallback) {
		mdb.runCmd(cmd, function (output) {
			subcallback(null, output);
		});
	    }
	}, function (err, results) {
		var lines, found, i;

		assert.ok(!err);

		lines = common.splitMdbLines(results.operations[0].result, {});
		found = false;
		for (i = 0; i < lines.length; i++) {
			if (lines[i] == testAddrs['anArray']) {
				found = true;
				break;
			}
		}
		assert.ok(found,
		    'did not find reference from array to testObject');

		lines = common.splitMdbLines(results.operations[1].result, {});
		found = false;
		for (i = 0; i < lines.length; i++) {
			if (lines[i] == testObjectAddr) {
				found = true;
				break;
			}
		}
		assert.ok(found,
		    'did not find reference from testObject to array');
		callback();
	});
}

/*
 * Verifies the case that an address has no valid references from JavaScript
 * objects.  It would be ideal to test this with a real JavaScript value that's
 * not referenced, but that's naturally hard to find.  Instead, we pick the
 * address of "main".  This should be mapped, but isn't itself a JavaScript
 * object, and so should not have a reference from a JavaScript object.
 */
function testBogusAddress(mdb, callback)
{
	console.error('test: bogus address (from stack)');
	mdb.runCmd('main=K\n', function (mainoutput) {
		var lines, cmd;

		lines = common.splitMdbLines(mainoutput, { 'count': 1 });
		cmd = lines[0].trim() + '::jsfindrefs\n';
		mdb.runCmd(cmd, function (output) {
			assert.strictEqual(output, '',
			    'found unexpected reference to main');
			callback();
		});
	});
}

main();
