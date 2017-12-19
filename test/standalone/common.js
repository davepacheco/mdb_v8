/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
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
	this.mdb_file = null;	/* file name */
	this.mdb_args = [];	/* extra CLI arguments */

	/* information about current pending command */
	this.mdb_pending_cmd = null;
	this.mdb_pending_callback = null;

	/* runtime state */
	this.mdb_exited = false;
	this.mdb_error = null;
	this.mdb_output = '';	/* buffered output */
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
		console.error('test failed; saving core file');
		this.mdb_error = new VError(error,
		    'error running test');
		this.emit('error', this.mdb_error);
		return;
	}

	fs.unlinkSync(this.mdb_file);
};

/*
 * Opens an MDB session.  Use runCmd() to invoke a command and get output.
 */
function createMdbSession(filename, callback)
{
	var mdb, cmdstr;
	var loaded = false;

	mdb = new MdbSession();
	mdb.mdb_file = filename;

	/* Use the "-S" flag to avoid interference from a user's .mdbrc file. */
	mdb.mdb_args.push('-S');

	if (process.env['MDB_LIBRARY_PATH'] &&
	    process.env['MDB_LIBRARY_PATH'] != '') {
		mdb.mdb_args.push('-L');
		mdb.mdb_args.push(process.env['MDB_LIBRARY_PATH']);
	}

	mdb.mdb_args.push(mdb.mdb_file);

	mdb.mdb_child = childprocess.spawn('mdb',
	    mdb.mdb_args, {
		'stdio': 'pipe',
		'env': { 'TZ': 'utc' }
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

	mdb.mdb_child.stderr.on('data', function (chunk) {
		console.log('mdb: stderr: ' + chunk);
		assert.ok(!loaded,
		    'dmod emitted stderr before ::load was complete');
	});

	cmdstr = '::load ' + dmodpath() + '\n';
	mdb.runCmd(cmdstr, function () {
		loaded = true;

		/*
		 * The '1000$w' sets the terminal width to a large value to keep
		 * MDB from inserting newlines at the default 80 columns.
		 */
		mdb.runCmd('1000$w', function () {
			callback(null, mdb);
		});
	});

	mdb.mdb_onprocexit = function (code) {
		if (code === 0) {
			throw (new Error('test exiting prematurely (' +
			    'mdb session not finalized)'));
		}
	};

	process.on('exit', mdb.mdb_onprocexit);
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
	    createMdbSession,
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
			    mdb.mdb_file);
		} else {
			err = new VError(err, 'test failed');
		}

		callback(err);
	});
}
