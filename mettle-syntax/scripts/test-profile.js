// Headless test for the VTune-style profiler fusion (profile.js). Stubs the
// `vscode` module (which only exists inside the extension host) so the pure
// fuse/render/tree functions can be exercised under plain node.

const assert = require('assert');
const Module = require('module');

const vscodeStub = {
  workspace: { getConfiguration: () => ({ get: (_k, d) => d }) },
  window: {},
  Range: class { constructor(a, b, c, d) { this.a = a; this.b = b; this.c = c; this.d = d; } },
  Position: class { constructor(l, c) { this.line = l; this.character = c; } },
  Selection: class {},
  Uri: { file: (p) => ({ fsPath: p }) },
  MarkdownString: class { constructor(s) { this.value = s; } },
  ThemeColor: class { constructor(id) { this.id = id; } },
  OverviewRulerLane: { Full: 7, Right: 4 },
  ViewColumn: { Beside: -2, One: 1 },
  EventEmitter: class { constructor() { this.event = () => ({ dispose() {} }); } fire() {} },
};

const originalResolve = Module._resolveFilename;
Module._resolveFilename = function (request, ...rest) {
  if (request === 'vscode') return 'vscode-stub';
  return originalResolve.call(this, request, ...rest);
};
require.cache['vscode-stub'] = { id: 'vscode-stub', filename: 'vscode-stub', loaded: true, exports: vscodeStub };

const { fuseProfile, renderProfileHtml, buildCallTree, buildInsights, waterfillInto } = require('../profile');

let passed = 0;
function check(name, fn) { fn(); passed++; console.log(`  ok - ${name}`); }

// A synthetic two-block function: a cheap setup block (executed once) and a hot
// memory-bound loop body (executed 1000x).
const model = {
  version: 3,
  functions: [{
    name: 'main', file: 'x.mettle', line: 1, byte_size: 64,
    insns: [
      { block: 0, line: 2, rthru: 50, press: [10, 0, 0, 0, 0, 0], falu: 0, intel: 'mov eax, 0', att: 'mov $0, eax', tag: '' },
      { block: 1, line: 3, rthru: 100, press: [0, 0, 0, 0, 20, 0], falu: 0, intel: 'mov rax, [rcx]', att: 'mov (rcx), rax', tag: '' },
    ],
    loops: [{ startRec: 1, endRec: 1, headLine: 3, depth: 1, bottleneck: 'load' }],
  }],
};
const mprof = {
  version: 1,
  blocks: [1, 1000],
  functions: [{ id: 0, name: 'main', file: 'x.mettle', line: 1, calls: 1, total_ns: 5000, self_ns: 5000, max_ns: 5000 }],
  edges: [],
  ops: [],
};

const fused = fuseProfile(model, mprof);

check('measured cycles = static cost x exec count', () => {
  // (50/100)*1 + (100/100)*1000 = 0.5 + 1000
  assert.ok(Math.abs(fused.progCyc - 1000.5) < 1e-6, `progCyc=${fused.progCyc}`);
});
check('retired = sum of block exec counts', () => {
  assert.strictEqual(fused.progRetired, 1001);
});
check('per-instruction count is joined from blocks[]', () => {
  assert.strictEqual(model.functions[0].insns[1].count, 1000);
  assert.strictEqual(model.functions[0].insns[0].count, 1);
});
check('program is bound on the load port (index 4)', () => {
  // load pressure = 20 * 1000 = 20000 dominates everything.
  assert.strictEqual(fused.bound, 4, `bound=${fused.bound} port=${JSON.stringify(fused.port)}`);
});
check('hot loop measured iters + cycles', () => {
  const l = model.functions[0]._loops[0];
  assert.strictEqual(l.iters, 1000);
  assert.ok(Math.abs(l.mcyc - 1000) < 1e-6, `loop mcyc=${l.mcyc}`);
});
check('hottest source line is the loop body (line 3)', () => {
  const lines = model.functions[0]._lines;
  assert.strictEqual(lines[0].line, 3);
  assert.strictEqual(lines[0].count, 1000);
});
check('wall time + root come from measured runtime stats', () => {
  assert.strictEqual(fused.wallNs, 5000);
  assert.strictEqual(fused.rootName, 'main');
});

check('renderProfileHtml produces HTML for each scope', () => {
  for (const scope of ['function', 'loop', 'line']) {
    const html = renderProfileHtml(model, mprof, fused, 'x.mettle', 'intel', scope);
    assert.ok(typeof html === 'string' && html.includes('measured cycles'), `scope ${scope} missing summary`);
    assert.ok(html.includes('main'), `scope ${scope} missing function`);
  }
});

check('buildInsights gives actionable memory-bound advice', () => {
  const ins = buildInsights(model, fused, 'x.mettle');
  assert.ok(ins.length >= 1, 'expected at least one insight');
  // The hottest loop is bound on the load port -> memory-bound advice.
  const mem = ins.find((i) => /memory-bound/i.test(i.detail));
  assert.ok(mem, `expected a memory-bound insight, got: ${JSON.stringify(ins.map((i) => i.detail.slice(0, 30)))}`);
  assert.ok(ins[0].weight >= ins[ins.length - 1].weight, 'insights must be ranked by weight');
});

check('buildInsights surfaces the optimizer fix on a hot line', () => {
  const m2 = JSON.parse(JSON.stringify(model));
  m2.functions[0].remarks = [{ line: 3, fix: 'mark the loop @simd to vectorize it', reason: 'counted unit-stride loop', positive: false }];
  const f2 = fuseProfile(m2, mprof);
  const ins = buildInsights(m2, f2, 'x.mettle');
  const fix = ins.find((i) => i.sev === 'fix');
  assert.ok(fix && /@simd/.test(fix.detail), 'expected a fix insight carrying the optimizer suggestion');
});

check('buildCallTree roots at main', () => {
  const tree = buildCallTree(mprof);
  assert.ok(tree && tree.name === 'main');
});

check('waterfillInto levels flexible ALU work', () => {
  const press = [0, 0, 0, 0, 0, 0];
  waterfillInto(press, 400, [0, 1, 2, 3]);
  assert.deepStrictEqual(press.slice(0, 4), [100, 100, 100, 100]);
});

check('blocks-less mprof degrades gracefully', () => {
  const f2 = fuseProfile({ functions: [{ name: 'a', insns: [{ block: 0, rthru: 10, press: [1, 0, 0, 0, 0, 0], falu: 0 }] }] }, { blocks: [] });
  assert.strictEqual(f2.progCyc, 0); // no counts -> no measured cost
});

console.log(`\nprofile fusion: ${passed} checks passed`);
