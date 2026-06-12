const fs = require('fs');
const path = require('path');
const Module = require('module');
const stub = { window: { createTextEditorDecorationType: () => ({}) }, ThemeColor: class {}, OverviewRulerLane: { Right: 1 }, workspace: {}, languages: {}, StatusBarAlignment: { Right: 2 }, CodeActionKind: { QuickFix: 'q' } };
const orig = Module._resolveFilename;
Module._resolveFilename = function (r, ...a) { return r === 'vscode' ? 'vscode-stub' : orig.call(this, r, ...a); };
require.cache['vscode-stub'] = { id: 'vscode-stub', filename: 'vscode-stub', loaded: true, exports: stub };
const x = require(path.join(__dirname, '..', 'explain.js'));
const model = x.parseReport(fs.readFileSync('scripts/explain-fixture.txt', 'utf8'));
// annotate like refreshReportNow does
const lines = fs.readFileSync('scripts/explain-demo-source.mettle', 'utf8').split(/\r?\n/);
for (const r of model.remarks) { r.applicable = null; if (!r.positive && r.fix) { r.applicable = x.synthesizeFix(r, lines); } }
try {
  const html = x.renderHtml(model, 'explain_demo.mettle');
  console.log('renderHtml ok, length', html.length);
  const lh = x.loadingHtml('explain_demo.mettle');
  console.log('loadingHtml ok, length', lh.length);
  const bm = html.match(/<strong>([^<]+)<\/strong>/);
  console.log('banner:', bm ? bm[1] : '(none)');
} catch (e) {
  console.error('RENDER THREW:', e.stack);
  process.exit(1);
}


