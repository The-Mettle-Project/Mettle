/**
 * Offline tests for explain.js: the report parser AND the fix synthesizer,
 * run against captured compiler fixtures + source snapshots:
 *   scripts/explain-fixture.txt            <- tests/explain_demo.mettle
 *   scripts/explain-demo-source.mettle
 *   scripts/explain-contracts-fixture.txt  <- tests/explain_contracts_demo.mettle
 *   scripts/explain-contracts-source.mettle
 * Regenerate with: mettle --release --explain <demo> -o nul 2> fixture
 * (via cmd.exe; PowerShell 2> wraps stderr in error-record noise).
 *
 * The `vscode` module only exists inside the editor, so it is stubbed with
 * just enough surface for explain.js to load.
 */

const fs = require('fs');
const path = require('path');
const Module = require('module');

const vscodeStub = {
  window: { createTextEditorDecorationType: () => ({}) },
  ThemeColor: class ThemeColor { constructor(id) { this.id = id; } },
  OverviewRulerLane: { Right: 1 },
  workspace: {},
  languages: {},
  StatusBarAlignment: { Right: 2 },
  CodeActionKind: { QuickFix: 'quickfix' },
};
const originalResolve = Module._resolveFilename;
Module._resolveFilename = function (request, ...rest) {
  if (request === 'vscode') return 'vscode-stub';
  return originalResolve.call(this, request, ...rest);
};
require.cache['vscode-stub'] = {
  id: 'vscode-stub', filename: 'vscode-stub', loaded: true, exports: vscodeStub,
};

const { parseReport, synthesizeFix } = require(path.join(__dirname, '..', 'explain.js'));

function fail(message) {
  console.error(`test-explain-parse: ${message}`);
  process.exit(1);
}

function read(name) {
  const p = path.join(__dirname, name);
  if (!fs.existsSync(p)) fail(`missing scripts/${name}`);
  return fs.readFileSync(p, 'utf8');
}

/** Apply synthesized edits (single-line, non-overlapping) to source lines. */
function applyEdits(lines, edits) {
  const sorted = [...edits].sort((a, b) => (b.line - a.line) || (b.start - a.start));
  const out = [...lines];
  for (const e of sorted) {
    out[e.line] = out[e.line].slice(0, e.start) + e.newText + out[e.line].slice(e.end);
  }
  return out;
}

// ---- part 1: parser, against the main demo --------------------------------

const model = parseReport(read('explain-fixture.txt'));

if (!model.sourceName || !model.sourceName.endsWith('.mettle')) {
  fail(`source name not parsed: ${model.sourceName}`);
}
if (model.remarks.length < 10) fail(`too few remarks parsed: ${model.remarks.length}`);

const loops = model.remarks.filter((r) => r.kind === 'loop');
if (loops.length === 0 || loops.some((r) => r.line === null)) {
  fail('loop remarks missing or without line numbers');
}
const verified = model.remarks.filter((r) => r.verified);
if (verified.length === 0) fail('no verified-fix remarks parsed (the demo has several)');
const calls = model.remarks.filter((r) => r.kind === 'call');
if (calls.length === 0 || calls.some((r) => !r.callee)) {
  fail('call remarks missing or without callee names');
}
if (model.stats.vectorized < 1 || model.stats.scalar < 1) {
  fail(`loop stats off: ${model.stats.vectorized} vectorized / ${model.stats.scalar} scalar`);
}
if (model.stats.verified !== verified.length) {
  fail(`verified stat (${model.stats.verified}) != verified remarks (${verified.length})`);
}
if (model.stats.backendOk === null || model.stats.backendTotal === null) {
  fail('backend coverage not parsed');
}
if (model.stats.weightedPct === null) fail('weighted instruction coverage not parsed');

// ---- part 2: fix synthesis, against the main demo source ------------------

const demoLines = read('explain-demo-source.mettle').split(/\r?\n/);

// int32 sum: "declare the accumulator `s` as int64" -> retype the declaration.
const sumInts = model.remarks.find((r) => r.fn === 'sum_ints' && r.kind === 'loop' && !r.positive);
if (!sumInts) fail('sum_ints scalar loop remark not found');
const sumIntsFix = synthesizeFix(sumInts, demoLines);
if (!sumIntsFix) fail('sum_ints accumulator fix not synthesized');
{
  const after = applyEdits(demoLines, sumIntsFix.edits);
  const changed = after.filter((l, i) => l !== demoLines[i]);
  if (changed.length !== 1 || !/var s: int64 = 0;/.test(changed[0])) {
    fail(`sum_ints fix produced wrong edit: ${JSON.stringify(changed)}`);
  }
}

// byte sum: declaration retype AND the (int64) cast the vpsadbw kernel needs.
const sumBytes = model.remarks.find((r) => r.fn === 'sum_bytes' && r.kind === 'loop' && !r.positive);
if (!sumBytes) fail('sum_bytes scalar loop remark not found');
const sumBytesFix = synthesizeFix(sumBytes, demoLines);
if (!sumBytesFix) fail('sum_bytes accumulator fix not synthesized');
{
  const after = applyEdits(demoLines, sumBytesFix.edits);
  const text = after.join('\n');
  if (!/var total: int64 = 0;/.test(text)) fail('sum_bytes fix missed the declaration retype');
  if (!/total = total \+ \(int64\)data\[i\];/.test(text)) {
    fail('sum_bytes fix missed the (int64) cast rewrite');
  }
}

// ---- part 3: decorator fixes, against the contracts demo ------------------

const contractsModel = parseReport(read('explain-contracts-fixture.txt'));
const contractsLines = read('explain-contracts-source.mettle').split(/\r?\n/);

// "remove `@noinline` from `damp`" (verified by the inliner re-run).
const dampLoop = contractsModel.remarks.find((r) =>
  r.fn === 'apply_damp' && r.kind === 'loop' && !r.positive);
if (!dampLoop) fail('apply_damp scalar loop remark not found in contracts fixture');
if (!dampLoop.verified) fail('apply_damp remark lost its verified line');
const dampFix = synthesizeFix(dampLoop, contractsLines);
if (!dampFix) fail('remove-@noinline fix not synthesized');
{
  const after = applyEdits(contractsLines, dampFix.edits);
  const dampDecl = after.find((l) => /fn damp\(/.test(l));
  if (!dampDecl || /@noinline/.test(dampDecl)) {
    fail(`remove-@noinline fix did not strip the decorator: ${dampDecl}`);
  }
}

// "mark the callee @inline to override the call-count cap" -> insert @inline.
const chainCall = contractsModel.remarks.find((r) =>
  r.kind === 'call' && r.callee === 'chain' && !r.positive);
if (!chainCall) fail('chain call refusal not found in contracts fixture');
const chainFix = synthesizeFix(chainCall, contractsLines);
if (!chainFix) fail('mark-@inline fix not synthesized for chain');
{
  const after = applyEdits(contractsLines, chainFix.edits);
  const chainDecl = after.find((l) => /fn chain\(/.test(l));
  if (!chainDecl || !/^@inline fn chain\(/.test(chainDecl)) {
    fail(`mark-@inline fix wrong: ${chainDecl}`);
  }
}

// Fixes must never synthesize for positive remarks or remarks without fix text.
for (const r of model.remarks.filter((x) => x.positive)) {
  if (r.fix && synthesizeFix(r, demoLines)) {
    fail(`fix synthesized for a positive remark: ${r.fn} ${r.entity}`);
  }
}

// ---- part 3: the --explain-json model (the panel's primary parse path) ------
// Fixture: explain_demo.mettle compiled twice with `scale` de-inlined between
// runs, so the changes section carries two real regressions.

const { modelFromJson, renderHtml } = require(path.join(__dirname, '..', 'explain.js'));
const jsonModel = modelFromJson(JSON.parse(read('explain-fixture.json')));

if (jsonModel.remarks.length < 10) fail(`json model too small: ${jsonModel.remarks.length}`);
const jsonLoops = jsonModel.remarks.filter((r) => r.kind === 'loop');
if (jsonLoops.length === 0 || jsonLoops.some((r) => r.line === null)) {
  fail('json loop remarks missing or without line numbers');
}
const jsonCalls = jsonModel.remarks.filter((r) => r.kind === 'call');
if (jsonCalls.some((r) => !r.callee)) fail('json call remark without callee');
if (jsonModel.stats.vectorized < 1 || jsonModel.stats.scalar < 1) {
  fail(`json stats off: ${jsonModel.stats.vectorized}/${jsonModel.stats.scalar}`);
}
if (jsonModel.stats.backendOk === null || jsonModel.backend.groups.length === 0) {
  fail('json backend section missing');
}
if (!jsonModel.remarks.some((r) => r.kind === 'loop' && r.depth >= 2)) {
  fail('no nested-loop depth in the json model (matvec inner loop is depth 2)');
}
if (!jsonModel.changes || !jsonModel.changes.hadBaseline) {
  fail('json changes section missing');
}
if (jsonModel.changes.regressed !== 2 ||
    !jsonModel.changes.entries.some((e) => e.direction === 'regressed' && e.kind === 'loop' && e.reason)) {
  fail(`json regressions wrong: ${JSON.stringify(jsonModel.changes)}`);
}

// fix synthesis works identically on JSON-sourced remarks, against the
// source snapshot the fixture was compiled from (scale carries @noinline
// there, so its removal is the verified fix on the regressed loop)
const jsonSourceLines = read('explain-json-source.mettle').split(/\r?\n/);
const jsonNoinline = jsonModel.remarks.find(
  (r) => r.kind === 'loop' && r.fix && /remove `@noinline` from `scale`/.test(r.fix));
if (!jsonNoinline) fail('regressed loop with the @noinline-removal fix not in the json model');
const jsonFix = synthesizeFix(jsonNoinline, jsonSourceLines);
if (!jsonFix) fail('fix synthesis failed on a JSON-sourced remark');
{
  const after = applyEdits(jsonSourceLines, jsonFix.edits);
  if (!after.some((l) => /^fn scale\(/.test(l))) {
    fail('JSON-sourced @noinline removal did not produce a clean declaration');
  }
}

// the rendered report leads with the regressions
const jsonHtml = renderHtml(jsonModel, 'demo.mettle');
if (!jsonHtml.includes('Since the last compile') || !jsonHtml.includes('REGRESSED')) {
  fail('changes banner missing from the rendered report');
}
if (!jsonHtml.includes('nest depth 2')) fail('depth chip missing from the rendered report');

const fixCount = [sumIntsFix, sumBytesFix, dampFix, chainFix].length;
console.log(
  `test-explain-parse passed: ${model.remarks.length} remarks ` +
  `(${model.stats.vectorized} vec, ${model.stats.scalar} scalar, ` +
  `${model.stats.verified} verified), backend ${model.stats.backendOk}/${model.stats.backendTotal}, ` +
  `${model.backend.groups.length} bail group(s), ${fixCount} fix syntheses verified; ` +
  `json model: ${jsonModel.remarks.length} remarks, ${jsonModel.changes.regressed} regressions, banner rendered`);
