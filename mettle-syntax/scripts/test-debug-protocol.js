/**
 * End-to-end test of the Mettle debug runtime protocol -- the exact contract
 * the VS Code debug adapter (debugAdapter.js) relies on.
 *
 * Compiles tests/debug_demo.mettle with --debug-hooks, hosts the named pipe,
 * launches the binary, and drives a real session: entry stop, breakpoint,
 * stack, variables, step in/out/over, eval, and a live variable WRITE whose
 * effect is asserted on the program's final stdout.
 *
 * Usage: node test-debug-protocol.js [path-to-mettle.exe]
 * (defaults to ../../bin/mettle.exe relative to this script)
 */

const assert = require('assert');
const net = require('net');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { execFileSync, spawn } = require('child_process');

const root = path.resolve(__dirname, '..', '..');
const compiler = process.argv[2] || path.join(root, 'bin', 'mettle.exe');
const fixture = path.join(root, 'tests', 'debug_demo.mettle');
const crashFixture = path.join(root, 'tests', 'debug_crash.mettle');
const exe = path.join(os.tmpdir(), `mettle-dbg-test-${process.pid}.exe`);
const crashExe = path.join(os.tmpdir(), `mettle-dbg-crash-${process.pid}.exe`);

function fail(message) {
  console.error(`test-debug-protocol: ${message}`);
  process.exit(1);
}

if (!fs.existsSync(compiler)) fail(`compiler not found: ${compiler}`);
if (!fs.existsSync(fixture)) fail(`fixture not found: ${fixture}`);

// Fixture line numbers (1-based) located by content so edits don't break us.
const fixtureLines = fs.readFileSync(fixture, 'utf8').split(/\r?\n/);
const lineOf = (needle) => {
  const i = fixtureLines.findIndex((l) => l.includes(needle));
  if (i < 0) fail(`fixture line not found: ${needle}`);
  return i + 1;
};
const BP_LOOP = lineOf('total = total + scale(i, 3);');
const LINE_INCR = lineOf('i = i + 1;');
const crashLines = fs.readFileSync(crashFixture, 'utf8').split(/\r?\n/);

// --- compile ----------------------------------------------------------------

execFileSync(compiler, [fixture, '-o', exe, '--build', '--debug-hooks'], {
  stdio: ['ignore', 'pipe', 'pipe'],
});

// --- pipe server + session driver ---------------------------------------------

const pipeName = `\\\\.\\pipe\\mettle-dbg-test-${process.pid}`;
let conn = null;
let lineBuffer = '';
const pending = [];   // unconsumed protocol lines
const waiters = [];   // promise resolvers waiting for a line

function onLine(line) {
  trace.push(`<${line}`);
  if (waiters.length > 0) waiters.shift()(line);
  else pending.push(line);
}

const trace = [];
function nextLine(timeoutMs = 8000) {
  if (pending.length > 0) return Promise.resolve(pending.shift());
  return new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`protocol timeout; trace: ${trace.slice(-12).join(' | ')}`)),
      timeoutMs);
    waiters.push((line) => { clearTimeout(timer); resolve(line); });
  });
}

async function expect(prefix) {
  const line = await nextLine();
  assert.ok(line.startsWith(prefix), `expected '${prefix}...', got '${line}'`);
  return line.split('\t');
}

/** Read lines until `terminator`; returns the collected lines before it. */
async function collectUntil(terminator) {
  const lines = [];
  for (;;) {
    const line = await nextLine();
    if (line === terminator) return lines;
    lines.push(line);
  }
}

function send(line) {
  trace.push(`>${line}`);
  conn.write(line + '\n');
}

const server = net.createServer((socket) => {
  conn = socket;
  socket.setEncoding('utf8');
  socket.on('data', (chunk) => {
    lineBuffer += chunk;
    let idx;
    while ((idx = lineBuffer.indexOf('\n')) >= 0) {
      onLine(lineBuffer.slice(0, idx).replace(/\r$/, ''));
      lineBuffer = lineBuffer.slice(idx + 1);
    }
  });
});

let child = null;
let stdout = '';

async function run() {
  await new Promise((resolve) => server.listen(pipeName, resolve));

  child = spawn(exe, [], {
    env: { ...process.env, METTLE_DBG_PIPE: pipeName },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stdout.on('data', (d) => { stdout += d.toString(); });

  // --- handshake: hello, tables, entry stop -------------------------------
  await expect('hello');
  const tableLines = await collectUntil('tablesdone');
  const files = new Map();
  let fnCount = 0;
  for (const line of tableLines) {
    const f = line.split('\t');
    if (f[0] === 'file') files.set(parseInt(f[1], 10), f[2]);
    if (f[0] === 'fn') fnCount++;
  }
  assert.ok(fnCount >= 3, `function table too small: ${fnCount}`);
  let demoFileId = -1;
  for (const [id, p] of files) {
    if (p.toLowerCase().endsWith('debug_demo.mettle')) demoFileId = id;
  }
  assert.ok(demoFileId >= 0, 'fixture file not in the file table');

  const entry = await expect('stopped\tentry');
  assert.ok(entry.length >= 5, 'entry stop carries location fields');

  // --- breakpoint in the loop -----------------------------------------------
  send(`setbp\t${demoFileId}\t${BP_LOOP}`);
  send('go');
  const bp = await expect('stopped\tbreakpoint');
  assert.strictEqual(parseInt(bp[3], 10), BP_LOOP, 'stopped at the loop line');

  // --- stack ------------------------------------------------------------------
  send('stack');
  const frames = await collectUntil('framesdone');
  assert.ok(frames.length >= 1, 'at least the main frame');
  const top = frames[0].split('\t');
  assert.strictEqual(top[0], 'frame');
  assert.strictEqual(parseInt(top[3], 10), BP_LOOP, 'top frame at the loop line');

  // --- variables: declared locals with correct initial values ----------------
  // var line shape: var \t name \t type \t is_param \t kids \t value
  send('vars\t0');
  let vars = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  const byName = new Map(vars.map((v) => [v[1], v]));
  assert.strictEqual(byName.get('total')[5], '0', 'total starts 0');
  assert.strictEqual(byName.get('i')[5], '0', 'i starts 0');
  assert.strictEqual(byName.get('ratio')[5], '1.5', 'float64 renders');
  assert.ok(byName.get('label')[5].includes('"checkpoint"'), 'string renders');
  assert.strictEqual(byName.get('corner')[4], '1', 'struct local is expandable');
  assert.strictEqual(byName.get('grid')[4], '1', 'array local is expandable');
  assert.strictEqual(byName.get('pp')[4], '1', 'struct pointer is expandable');
  assert.strictEqual(byName.get('total')[4], '0', 'scalar is not expandable');

  // --- struct/array/pointer expansion -----------------------------------------
  send('expand\t0\tcorner');
  let kids = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  let kidMap = new Map(kids.map((v) => [v[1], v]));
  assert.strictEqual(kidMap.get('x')[5], '7', 'corner.x expands to 7');
  assert.strictEqual(kidMap.get('y')[5], '9', 'corner.y expands to 9');

  send('expand\t0\tbox.min');
  kids = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  kidMap = new Map(kids.map((v) => [v[1], v]));
  assert.strictEqual(kidMap.get('y')[5], '2', 'nested struct path expands');

  send('expand\t0\tgrid');
  kids = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  assert.strictEqual(kids.length, 4, 'array expands to its elements');
  assert.strictEqual(kids[2][1], '[2]');
  assert.strictEqual(kids[2][5], '12', 'grid[2] = 12');

  send('expand\t0\tpp');
  kids = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  kidMap = new Map(kids.map((v) => [v[1], v]));
  assert.strictEqual(kidMap.get('x')[5], '7', 'pointer auto-derefs to fields');

  // --- path eval + path WRITE -----------------------------------------------------
  // evalr shape: evalr \t ok \t type \t kids \t value
  send('eval\t0\tbox.max.y');
  let evalReply = await expect('evalr\t1');
  assert.strictEqual(evalReply[4], '40', 'eval walks nested fields');
  send('eval\t0\tgrid[3]');
  evalReply = await expect('evalr\t1');
  assert.strictEqual(evalReply[4], '13', 'eval indexes arrays');
  send('eval\t0\tpp.y');
  evalReply = await expect('evalr\t1');
  assert.strictEqual(evalReply[4], '9', 'eval derefs pointers for field access');
  send('set\t0\tbox.max.x\t99');
  let setReply = await expect('setr\t1');
  assert.strictEqual(setReply[2], '99', 'struct field written through the path');
  send('eval\t0\tbox.max.x');
  evalReply = await expect('evalr\t1');
  assert.strictEqual(evalReply[4], '99', 'written field reads back');

  // --- step into scale(): params visible, depth grows -------------------------
  send('stepin');
  const inScale = await expect('stopped\tstep');
  const depthInScale = parseInt(inScale[4], 10);
  send('stack');
  const scaleFrames = await collectUntil('framesdone');
  assert.strictEqual(scaleFrames.length, depthInScale, 'stack matches depth');
  assert.ok(scaleFrames.length >= 2, 'inside a callee');
  send('vars\t0');
  vars = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  const params = new Map(vars.map((v) => [v[1], v]));
  assert.strictEqual(params.get('value')[5], '0', 'param value=0 (first iter)');
  assert.strictEqual(params.get('amount')[5], '3', 'param amount=3');
  assert.strictEqual(params.get('value')[3], '1', 'value flagged as a parameter');

  // --- step out: lands on the next line hook in main (i = i + 1) ----------------
  send('stepout');
  const atIncr = await expect('stopped\tstep');
  assert.strictEqual(parseInt(atIncr[3], 10), LINE_INCR, 'step-out lands on i = i + 1');

  // --- eval + WRITE: jump i from 0 to 3, changing which iterations run ----------
  send('eval\t0\tratio');
  evalReply = await expect('evalr\t1');
  assert.strictEqual(evalReply[4], '1.5', 'eval reads ratio');
  send('set\t0\ti\t3');
  setReply = await expect('setr\t1');
  assert.strictEqual(setReply[2], '3', 'write confirmed with the new value');

  // --- step over the increment: i becomes 4, control returns to the while cond ---
  send('next');
  await expect('stopped\tstep');

  // --- breakpoint hits again with the warped state ------------------------------
  send('go');
  const bp2 = await expect('stopped\tbreakpoint');
  assert.strictEqual(parseInt(bp2[3], 10), BP_LOOP);
  send('vars\t0');
  vars = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  const after = new Map(vars.map((v) => [v[1], v]));
  assert.strictEqual(after.get('i')[5], '4', 'i incremented from the written 3');
  assert.strictEqual(after.get('total')[5], '0', 'iterations 1-3 were skipped');

  // --- conditional breakpoint: fires only when its expression holds ---------------
  // From here (i=4, total=0): line 17 adds scale(4,3)=24, so at the increment
  // line `total == 24` is true exactly once.
  send('clearall');
  send(`bpadd\t${demoFileId}\t${LINE_INCR}\ttotal == 24`);
  send('go');
  const condStop = await expect('stopped\tbreakpoint');
  assert.strictEqual(parseInt(condStop[3], 10), LINE_INCR, 'conditional bp line');
  send('vars\t0');
  vars = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  const atCond = new Map(vars.map((v) => [v[1], v]));
  assert.strictEqual(atCond.get('total')[5], '24', 'condition held when it fired');
  assert.strictEqual(atCond.get('i')[5], '4', 'still in the warped iteration');

  // --- clear breakpoints, run to completion ---------------------------------------
  send('clearall');
  send('go');
  const exitCode = await new Promise((resolve) => child.on('exit', resolve));
  assert.strictEqual(exitCode, 0, 'program exits cleanly');
  // total = scale(0,3) + scale(4,3) = 0 + 24: the live write changed the output
  assert.ok(stdout.includes('total=24'),
    `live variable write altered execution (stdout: ${JSON.stringify(stdout)})`);

  // ==== session 2: crash interception =============================================
  // The crash fixture null-derefs on iteration 3. The runtime must stop AT
  // the faulting line with the stack and variables intact.
  execFileSync(compiler, [crashFixture, '-o', crashExe, '--build', '--debug-hooks'], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const CRASH_LINE = crashLines.findIndex((l) => l.includes('p[0] = 42;')) + 1;
  assert.ok(CRASH_LINE > 0, 'crash fixture line found');

  lineBuffer = '';
  pending.length = 0;
  child = spawn(crashExe, [], {
    env: { ...process.env, METTLE_DBG_PIPE: pipeName },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  await expect('hello');
  await collectUntil('tablesdone');
  await expect('stopped\tentry');
  send('go');

  const exc = await expect('stopped\texception');
  assert.strictEqual(parseInt(exc[3], 10), CRASH_LINE, 'stopped at the faulting line');
  assert.ok(/0xc0000005/i.test(exc[6] || ''), `access violation code (got ${exc[6]})`);

  send('stack');
  const crashFrames = await collectUntil('framesdone');
  assert.ok(crashFrames.length >= 1, 'stack available at the crash');
  assert.strictEqual(parseInt(crashFrames[0].split('\t')[3], 10), CRASH_LINE);

  send('vars\t0');
  vars = (await collectUntil('varsdone')).map((l) => l.split('\t'));
  const atCrash = new Map(vars.map((v) => [v[1], v]));
  assert.strictEqual(atCrash.get('i')[5], '3', 'crash-iteration state inspectable');
  assert.strictEqual(atCrash.get('p')[5], '0x10', 'the bad pointer is visible');

  send('go'); // a fault is not resumable: the process dies through default handling
  const crashExit = await new Promise((resolve) => child.on('exit', resolve));
  assert.notStrictEqual(crashExit, 0, 'crashed process exits nonzero');

  console.log(`test-debug-protocol passed: ${fnCount} fns; bp/step/eval/set, struct+array+pointer expansion, path eval/write, conditional bp, crash stop at line ${CRASH_LINE} verified; warped total=24`);
}

run()
  .catch((err) => {
    if (child) child.kill();
    fail(err.message);
  })
  .finally(() => {
    server.close();
    try { fs.unlinkSync(exe); } catch (_) { /* still running on failure paths */ }
    try { fs.unlinkSync(crashExe); } catch (_) { /* may not exist */ }
  });
