/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2018, Joyent, Inc.
 */

/*
 * test/standalone/common.js: common functions for standalone JavaScript tests
 */

var assert = require('assert');
var childprocess = require('child_process');
var events = require('events');
var fs = require('fs');
var path = require('path');
var util = require('util');
var vasync = require('vasync');
var VError = require('verror');

/* Public interface */
exports.dmodpath = dmodpath;
exports.gcoreSelf = gcoreSelf;
exports.createMdbSession = createMdbSession;
exports.standaloneTest = standaloneTest;
exports.gcAvoid = gcAvoid;

var MDB_SENTINEL = 'MDB_SENTINEL\n';

/*
 * Returns the path to the built dmod, for loading into mdb during testing.
 */
function dmodpath()
{
	var arch = process.arch == 'x64' ? 'amd64' : 'ia32';
	return (path.join(
	    __dirname, '..', '..', 'build', arch, 'mdb_v8.so'));
}

/*
 * Utility function for saving a core file of the current process using
 * gcore(1M).  This is used in a number of tests as the basis for exercising
 * mdb_v8.
 *
 * This function invokes "callback" upon completion with arguments:
 *
 * - err, if there was any error
 * - filename, with the path to the specified file
 */
function gcoreSelf(callback)
{
	var prefix, corefile, gcore;

	prefix = '/var/tmp/node';
	corefile = prefix + '.' + process.pid;
	gcore = childprocess.spawn('gcore',
	    [ '-o', prefix, process.pid + '' ]);

	gcore.stderr.on('data', function (data) {
		console.log('gcore: stderr: ' + data);
	});

	gcore.on('exit', function (code) {
		if (code !== 0) {
			callback(new Error('gcore exited with code ' + code));
			return;
		}

		console.log('gcore created %s', corefile);
		callback(null, corefile);
	});
}

function MdbSession()
{
	this.mdb_child = null;	/* child process handle */
	this.mdb_target_name = null;	/* file name or pid */
	this.mdb_args = [];	/* extra CLI arguments */
	this.mdb_target_type = null;	/* "file" or "pid" */
	this.mdb_remove_on_success = false;

	/* information about current pending command */
	this.mdb_pending_cmd = null;
	this.mdb_pending_callback = null;

	/* runtime state */
	this.mdb_exited = false;
	this.mdb_error = null;
	this.mdb_output = '';	/* buffered output */
	this.mdb_findleaks = null;
}

util.inherits(MdbSession, events.EventEmitter);

MdbSession.prototype.runCmd = function (str, callback)
{
	assert.equal(typeof (str), 'string');
	assert.equal(typeof (callback), 'function');
	assert.strictEqual(this.mdb_pending_cmd, null,
	    'command is already pending');
	assert.strictEqual(this.mdb_error, null,
	    'already experienced fatal error');
	assert.strictEqual(this.mdb_exited, false,
	    'mdb already exited');
	assert.equal(str.charAt(str.length - 1), '\n',
	    'command string must end in a newline');

	assert.strictEqual(this.mdb_pending_callback, null);
	this.mdb_pending_cmd = str;
	this.mdb_pending_callback = callback;
	process.stderr.write('> ' + str);
	this.mdb_child.stdin.write(str);
	this.mdb_child.stdin.write('!echo ' + MDB_SENTINEL);
};

MdbSession.prototype.onExit = function (code)
{
	this.mdb_exited = true;
	if (code !== 0) {
		this.mdb_error = new Error(
		    'mdb exited unexpectedly with code ' + code);
		this.emit('error', this.mdb_error);
	}
};

MdbSession.prototype.doWork = function ()
{
	var i, chunk, callback;

	i = this.mdb_output.indexOf(MDB_SENTINEL);
	assert.ok(i >= 0);
	chunk = this.mdb_output.substr(0, i);
	this.mdb_output = this.mdb_output.substr(i + MDB_SENTINEL.length);
	console.error(chunk);

	assert.notStrictEqual(this.mdb_pending_cmd, null);
	assert.notStrictEqual(this.mdb_pending_callback, null);
	callback = this.mdb_pending_callback;
	this.mdb_pending_callback = null;
	this.mdb_pending_cmd = null;
	callback(chunk);
};

MdbSession.prototype.finish = function (error)
{
	assert.strictEqual(this.mdb_error, null,
	    'already experienced fatal error');
	process.removeListener('exit', this.mdb_onprocexit);

	if (!this.mdb_exited) {
		this.mdb_child.stdin.end();
	}

	if (error) {
		this.mdb_error = new VError(error,
		    'error running test');
		this.emit('error', this.mdb_error);
		return;
	}

	if (this.mdb_remove_on_success && this.mdb_target_type == 'file') {
		fs.unlinkSync(this.mdb_target_name);
	}
};

MdbSession.prototype.checkMdbLeaks = function (callback)
{
	var self = this;
	var leakmdb;

	/*
	 * Attach another MDB session to MDB itself so that we can run
	 * "findleaks" to look for leaks in "mdb" and "mdb_v8".  At this point,
	 * we only print this information out, rather than trying to parse it
	 * and take action.  We also rely on the underlying MdbSession's
	 * transcript rather than explicitly printing out the output.
	 */
	assert.strictEqual(this.mdb_findleaks, null,
	    'mdb leak check already pending');
	leakmdb = this.mdb_findleaks = createMdbSession({
	    'targetType': 'pid',
	    'targetName': this.mdb_child.pid.toString(),
	    'loadDmod': false,
	    'removeOnSuccess': false
	}, function (err) {
		assert.equal(self.mdb_findleaks, leakmdb);

		if (err) {
			self.mdb_findleaks = null;
			callback(new VError(err, 'attaching mdb to itself'));
			return;
		}

		leakmdb.runCmd('::findleaks -d\n', function () {
			assert.equal(self.mdb_findleaks, leakmdb);
			self.mdb_findleaks = null;
			leakmdb.finish();
			callback();
		});
	});
};

/*
 * Opens an MDB session.  Use runCmd() to invoke a command and get output.
 */
function createMdbSessionFile(filename, callback)
{
	return (createMdbSession({
	    'targetType': 'file',
	    'targetName': filename,
	    'loadDmod': true,
	    'removeOnSuccess': true
	}, callback));
}

function createMdbSession(args, callback)
{
	var mdb, loaddmod;
	var loaded = false;

	assert.equal('object', typeof (args));
	assert.equal('string', typeof (args.targetType));
	assert.equal('string', typeof (args.targetName));
	assert.equal('boolean', typeof (args.removeOnSuccess));
	assert.equal('boolean', typeof (args.loadDmod));

	assert.ok(args.targetType == 'file' || args.targetType == 'pid');
	loaddmod = args.loadDmod;

	mdb = new MdbSession();
	mdb.mdb_target_name = args.targetName;
	mdb.mdb_target_type = args.targetType;
	mdb.mdb_remove_on_success = args.removeOnSuccess;

	/* Use the "-S" flag to avoid interference from a user's .mdbrc file. */
	mdb.mdb_args.push('-S');

	if (process.env['MDB_LIBRARY_PATH'] &&
	    process.env['MDB_LIBRARY_PATH'] != '') {
		mdb.mdb_args.push('-L');
		mdb.mdb_args.push(process.env['MDB_LIBRARY_PATH']);
	}

	if (args.targetType == 'file') {
		mdb.mdb_args.push(args.targetName);
	} else {
		mdb.mdb_args.push('-p');
		mdb.mdb_args.push(args.targetName);
	}

	mdb.mdb_child = childprocess.spawn('mdb',
	    mdb.mdb_args, {
		'stdio': 'pipe',
		'env': {
		    'TZ': 'utc',
		    'UMEM_DEBUG': 'default',
		    'UMEM_LOGGING': 'transaction=8M,fail'
		}
	    });

	mdb.mdb_child.on('exit', function (code) {
		mdb.onExit(code);
	});

	mdb.mdb_child.stdout.on('data', function (chunk) {
		mdb.mdb_output += chunk;
		while (mdb.mdb_output.indexOf(MDB_SENTINEL) != -1) {
			mdb.doWork();
		}
	});

	mdb.mdb_onprocexit = function (code) {
		if (code === 0) {
			throw (new Error('test exiting prematurely (' +
			    'mdb session not finalized)'));
		}
	};
	process.on('exit', mdb.mdb_onprocexit);

	mdb.mdb_child.stderr.on('data', function (chunk) {
		console.log('mdb: stderr: ' + chunk);
		assert.ok(!loaddmod || loaded,
		    'dmod emitted stderr before ::load was complete');
	});

	/*
	 * The '1000$w' sets the terminal width to a large value to keep MDB
	 * from inserting newlines at the default 80 columns.
	 */
	mdb.runCmd('1000$w\n', function () {
		var cmdstr;
		if (!loaddmod) {
			callback(null, mdb);
			return;
		}

		cmdstr = '::load ' + dmodpath() + '\n';
		mdb.runCmd(cmdstr, function () {
			loaded = true;
			callback(null, mdb);
		});
	});

	return (mdb);
}

/*
 * Standalone test-cases do the following:
 *
 * - gcore the current process
 * - start up MDB on the current process
 * - invoke each of the specified functions as a vasync pipeline, with an
 *   "MdbSession" as the sole initial argument
 * - on success, clean up the core file that was created
 */
function standaloneTest(funcs, callback)
{
	var mdb;

	vasync.waterfall([
	    gcoreSelf,
	    createMdbSessionFile,
	    function runTestPipeline(mdbhdl, wfcallback) {
		mdb = mdbhdl;
		vasync.pipeline({
		    'funcs': funcs,
		    'arg': mdbhdl
		}, wfcallback);
	    }
	], function (err) {
		if (!err) {
			mdb.finish();
			callback();
			return;
		}

		if (mdb) {
			err = new VError(err,
			    'test failed (keeping core file %s)',
			    mdb.mdb_target_name);
		} else {
			err = new VError(err, 'test failed');
		}

		callback(err);
	});
}

/*
 * This awful function exists to attempt to stave off garbage collection for a
 * modest number of future allocations.  It works by allocating a bunch of
 * objects until the JavaScript heap expands.  Once the heap expands, we remove
 * these references and return.  The hope is that we've expanded the heap enough
 * that another GC won't be necessary for a little while.
 *
 * This is obviously a really cheesy approach.  The problem is that at present,
 * mdb_v8 cannot deal well with core files created in the middle of a GC.
 * (That's in turn because V8 borrows bits in pointer addresses to store GC
 * state, and that means we can't generally tell from an address whether it's an
 * integer or a pointer that's had its low bit cleared for temporary GC
 * purposes.  That makes it really hard to process most data structures.)
 *
 * Given this limitation, and the desire to have reliable tests, we're left with
 * few options.  We can modify the test to avoid GC altogether, but the more the
 * tests differ from what normal JavaScript programs do, the less they're
 * testing the right behavior.
 * TODO another option might be that instead of using gcore, we run a DTrace
 * script that fires on a well-known probe and saves a core file.
 */
var gcAvoidRefs;
function gcAvoid()
{
	var nrefs, nrefsmax, heaptotal;

	heaptotal = process.memoryUsage().heapTotal;
	nrefs = 0;
	nrefsmax = 100000;
	gcAvoidRefs = [];
	while (nrefs < nrefsmax &&
	    process.memoryUsage().heapTotal == heaptotal) {
		gcAvoidRefs.push({ 'ref': nrefs++ });
	}

	assert.notEqual(nrefs, nrefsmax);
	gcAvoidRefs = undefined;
}
