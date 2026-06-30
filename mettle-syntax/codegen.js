const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { execFile } = require('child_process');

/** @type {vscode.WebviewPanel | null} */
let panel = null;
let sourcePath = null;
let model = null;
let asmSyntax = 'intel';
let asmGroup = 'flat';

const decorationTypes = {};
function decoration(rulerColor) {
  return vscode.window.createTextEditorDecorationType({
    isWholeLine: true,
    after: { margin: '0 0 0 2em', color: new vscode.ThemeColor('editorGhostText.foreground') },
    overviewRulerColor: rulerColor,
    overviewRulerLane: vscode.OverviewRulerLane.Right,
  });
}
function ensureDecorations() {
  if (decorationTypes.vectorized) return;
  decorationTypes.vectorized = decoration('rgba(78, 201, 148, 0.7)');
  decorationTypes.idiom = decoration('rgba(97, 175, 239, 0.7)');
  decorationTypes.strength = decoration('rgba(198, 120, 221, 0.7)');
  decorationTypes.spill = decoration('rgba(224, 108, 117, 0.7)');
  decorationTypes.other = decoration('rgba(150, 150, 150, 0.4)');
}

function runAnnotate(deps, filePath, workspaceRoot) {
  const cfg = vscode.workspace.getConfiguration('mettle');
  const compiler = deps.findCompiler(workspaceRoot, filePath);
  const timeoutMs = Math.max(2000, Number(cfg.get('codegen.timeoutMs', 30000)) || 30000);

  const stem = path.basename(filePath, '.mettle').replace(/[^A-Za-z0-9_-]/g, '_');
  const hash = require('crypto').createHash('sha1').update(filePath.toLowerCase()).digest('hex').slice(0, 10);
  let tempDir;
  try {
    tempDir = path.join(os.tmpdir(), 'mettle-codegen', `${stem}-${hash}`);
    fs.mkdirSync(tempDir, { recursive: true });
  } catch (err) {
    return Promise.resolve({ ok: false, error: `temp dir: ${err.message}` });
  }
  const tempOut = path.join(tempDir, 'codegen.obj');
  const jsonPath = path.join(tempDir, 'codegen.annot.json');

  const args = [
    '-i', filePath,
    '-o', tempOut,
    '--annotate-asm',
    '--asm-syntax=both',
    '-I', path.dirname(filePath),
    '-I', workspaceRoot,
  ];
  const stdlibPath = cfg.get('linter.stdlibPath', '');
  if (stdlibPath) {
    args.push('--stdlib', path.isAbsolute(stdlibPath) ? stdlibPath : path.join(workspaceRoot, stdlibPath));
  }
  for (const includePath of cfg.get('linter.extraIncludePaths', []) || []) {
    if (!includePath || typeof includePath !== 'string') continue;
    args.push('-I', path.isAbsolute(includePath) ? includePath : path.join(workspaceRoot, includePath));
  }

  return new Promise((resolve) => {
    execFile(compiler, args, {
      timeout: timeoutMs,
      maxBuffer: 64 * 1024 * 1024,
      cwd: workspaceRoot,
      env: { ...process.env, METTLE_EXPLAIN_REPORT_LINES: '0', NO_COLOR: '1' },
    }, (err) => {
      if (err && /** @type {NodeJS.ErrnoException} */ (err).code === 'ENOENT') {
        resolve({ ok: false, error: `Compiler not found: ${compiler}. Set mettle.linter.compilerPath.` });
        return;
      }
      let json = null;
      try {
        json = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
      } catch (e) {
        resolve({ ok: false, error: err ? 'Compilation failed before annotations were written.' : `Could not read annotations: ${e.message}` });
        return;
      }
      resolve({ ok: true, json });
    });
  });
}

function focusFunctions(m, filePath) {
  if (!m || !Array.isArray(m.functions)) return [];
  const base = path.basename(filePath).toLowerCase();
  const own = m.functions.filter((f) => f.file && path.basename(f.file).toLowerCase() === base);
  return own.length ? own : m.functions;
}

function lineDecisions(m, filePath) {
  const byLine = new Map();
  const rank = { vectorized: 5, idiom: 4, 'strength-reduce': 3, spill: 2 };
  for (const f of focusFunctions(m, filePath)) {
    for (const r of f.remarks || []) {
      if (!r.line) continue;
      const prev = byLine.get(r.line);
      const tag = r.positive ? 'vectorized' : 'other';
      const text = r.headline || '';
      if (!prev || (rank[tag] || 0) >= (rank[prev.tag] || 0)) {
        byLine.set(r.line, { tag, text });
      }
    }
    for (const ins of f.insns || []) {
      if (!ins.line || !ins.tag) continue;
      const tag = ins.tag;
      if (!(tag in rank)) continue;
      const prev = byLine.get(ins.line);
      const text = ins.note || tag;
      if (!prev || (rank[tag] || 0) > (rank[prev.tag] || 0)) {
        byLine.set(ins.line, { tag, text });
      }
    }
  }
  return byLine;
}

function applyDecorations() {
  ensureDecorations();
  const buckets = { vectorized: [], idiom: [], strength: [], spill: [], other: [] };
  if (model && sourcePath) {
    const byLine = lineDecisions(model, sourcePath);
    for (const [line, d] of byLine) {
      const key = d.tag === 'strength-reduce' ? 'strength' : (buckets[d.tag] ? d.tag : 'other');
      const ln = Math.max(0, line - 1);
      buckets[key].push({
        range: new vscode.Range(ln, 0, ln, 0),
        renderOptions: { after: { contentText: `   » ${d.text}` } },
        hoverMessage: new vscode.MarkdownString(`**codegen:** ${d.text}`),
      });
    }
  }
  for (const editor of vscode.window.visibleTextEditors) {
    if (!sourcePath || path.normalize(editor.document.uri.fsPath).toLowerCase() !== path.normalize(sourcePath).toLowerCase()) {
      for (const k of Object.keys(buckets)) editor.setDecorations(decorationTypes[k], []);
      continue;
    }
    for (const k of Object.keys(buckets)) editor.setDecorations(decorationTypes[k], buckets[k]);
  }
}

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

function tagClass(tag) {
  if (!tag) return '';
  if (tag === 'strength-reduce') return 'strength';
  if (tag === 'global-cache' || tag === 'address-fold') return 'fold';
  return tag;
}

const RES = ['p0', 'p1', 'p5', 'p6', 'load', 'store'];
function fmtCyc(centi) { return (Number(centi) / 100).toFixed(2); }

const KIND_COLOR = {
  mov: '#7f848e', alu: '#61afef', mul: '#528bff', div: '#e06c75', shift: '#61afef',
  lea: '#56b6c2', cmp: '#7f848e', setcc: '#7f848e', cmov: '#7f848e',
  branch: '#c678dd', call: '#be5046', float: '#e5c07b', vec: '#4ec994',
  load: '#d19a66', store: '#d19a66', kernel: '#98c379', frame: '#4b5263', other: '#5c6370',
};
function insnHeat(r) { return (r.rthru || 0) * Math.pow(10, Math.min(r.depth || 0, 3)); }

function summaryCard(f) {
  const s = f.summary;
  if (!s) return '';
  const mix = s.mix || {};
  const keys = Object.keys(mix).sort((a, b) => mix[b] - mix[a]);
  const segs = keys.map((k) =>
    `<span class="seg" style="flex:${mix[k]};background:${KIND_COLOR[k] || '#666'}" title="${esc(k)}: ${mix[k]}"></span>`).join('');
  const legend = keys.map((k) =>
    `<span class="lg"><i style="background:${KIND_COLOR[k] || '#666'}"></i>${esc(k)} <em>${mix[k]}</em></span>`).join('');
  const chip = (cls, txt) => `<span class="chip ${cls}">${txt}</span>`;
  const chips = [
    chip('accent', `~${fmtCyc(s.totalRthru)} <em>cyc</em>`),
    s.loops ? chip('', `${s.loops} loop${s.loops > 1 ? 's' : ''}`) : null,
    chip(s.spills ? 'warn' : 'good', `${s.spills} spill${s.spills === 1 ? '' : 's'}`),
    s.vecOps ? chip('good', `${s.vecOps} vector op${s.vecOps > 1 ? 's' : ''}`) : null,
    s.regsUsed != null ? chip('', `${s.regsUsed} <em>regs</em>`) : null,
  ].filter(Boolean).join('');
  const est = s.estimatedSpans || 0;
  const note = `<div class="estnote">cost = static Skylake-class port model decoded from the emitted instructions; macro-fusion (cmp+jcc) and reg-reg mov elimination modeled, branch prediction is not`
    + (est ? ` · ${est} span${est > 1 ? 's' : ''} could not be decoded and fall back to an opcode estimate` : '')
    + `</div>`;
  return `<div class="card summary"><div class="chips">${chips}</div>
    <div class="mixbar">${segs}</div><div class="legend">${legend}</div>${note}</div>`;
}

function loopCards(f) {
  const loops = f.loops || [];
  if (!loops.length) return '';
  const cards = loops.map((l) => {
    const maxp = Math.max(1, ...(l.press || [1]));
    const bars = (l.press || []).map((p, i) => {
      const hot = RES[i] === l.bottleneck;
      return `<div class="pp ${hot ? 'hot' : ''}"><span class="ppn">${RES[i]}</span>
        <span class="ppbar"><b style="width:${(p / maxp * 100).toFixed(0)}%"></b></span>
        <span class="ppv">${fmtCyc(p)}</span></div>`;
    }).join('');
    const partial = l.hasKernel || l.estimated;
    const depth = l.depth > 0 ? `<span class="depthb">nest ${l.depth}</span>` : '';
    const kernel = l.hasKernel ? `<span class="kbadge">+ SIMD kernel</span>` : '';
    const cyc = (partial ? '≈' : '') + fmtCyc(l.cyclesPerIter);
    const foot = l.hasKernel
      ? 'figure excludes the inline SIMD kernel (counted separately)'
      : l.estimated ? 'contains a span the decoder could not read (opcode estimate)' : '';
    return `<div class="card loop${partial ? ' coarse' : ''}">
      <div class="lh"><span class="ln" data-line="${l.headLine}">⟳ loop · line ${l.headLine}</span>${depth}</div>
      <div class="metric"><span class="cyc">${cyc}</span><span class="unit">cyc/iter</span><span class="sep">·</span><span class="bnlabel">bound on</span><span class="bn" title="busiest issue port">${esc(l.bottleneck)}</span>${kernel}</div>
      <div class="pports">${bars}</div>
      ${foot ? `<div class="estnote">${foot}</div>` : ''}</div>`;
  }).join('');
  return `<div class="loops">${cards}</div>`;
}

function timelineData(f) {
  const rm = f.regmap;
  if (!rm || !Array.isArray(rm.intervals) || !rm.intervals.length) return null;
  const insns = f.insns || [];
  const line = {};
  const calls = [];
  for (const r of insns) {
    if (typeof r.idx !== 'number' || r.idx < 0) continue;
    if (r.line) line[r.idx] = r.line;
    if (r.kind === 'call') calls.push(r.idx);
  }
  const loops = (f.loops || []).map((l) => {
    const a = (insns[l.startRec] || {}).idx;
    const b = (insns[l.endRec] || {}).idx;
    if (a == null || b == null) return null;
    return { a, b: Math.max(a + 1, b), depth: l.depth || 0, line: l.headLine || 0 };
  }).filter(Boolean);
  const intervals = rm.intervals.map((iv) => ({
    name: iv.name, cls: iv.cls, start: iv.start, end: iv.end,
    cc: iv.crossesCall ? 1 : 0, lc: iv.loopCarried ? 1 : 0,
  }));
  return {
    name: f.name, axis: Math.max(1, rm.axis || 1), spills: rm.spills || 0,
    intervals, loops, calls, line,
  };
}
function regTimeline(f, i) {
  const rm = f.regmap;
  if (!rm || !Array.isArray(rm.intervals) || !rm.intervals.length) return '';
  const n = rm.intervals.length;
  const spillNote = rm.spills ? ` &middot; <span class="warn">${rm.spills} spilled to stack</span>` : '';
  return `<details class="rmwrap" open><summary>register allocation timeline &middot; ${n} live interval${n === 1 ? '' : 's'}${spillNote}</summary>
    <div class="rmhost" data-i="${i}" data-mode="used">
      <div class="rmtoolbar">
        <span class="rmseg"><button class="rmtoggle" data-mode="used">used regs</button><button class="rmtoggle" data-mode="all">full file</button></span>
        <span class="rmhintx">hover an interval, glide across for the live set, click to jump</span>
        <span class="rmreadout"></span>
      </div>
      <div class="rmsvgbox"></div>
      <div class="rmtip"></div>
    </div></details>`;
}

function renderHtml(m, filePath, syntax, group) {
  const funcs = focusFunctions(m, filePath);
  const insnRow = (r, maxHeat) => {
    const asm = syntax === 'att' ? r.att : r.intel;
    const ratio = maxHeat > 0 ? insnHeat(r) / maxHeat : 0;
    const bg = ratio > 0.02 ? `style="background:rgba(224,108,117,${(0.45 * ratio).toFixed(3)})"` : '';
    const dec = r.tag ? `<span class="tag ${tagClass(r.tag)}">${esc(r.tag)}</span>${r.note ? ' ' + esc(r.note) : ''}` : '';
    const ln = r.line ? `<span class="ln" data-line="${r.line}">${r.line}</span>` : '';
    const cost = (r.rthru > 0 || r.lat > 1)
      ? `<span class="lat" title="latency ${r.lat}c · recip throughput ${fmtCyc(r.rthru)}c · ports ${esc(r.ports || '?')}">${r.lat}c·${fmtCyc(r.rthru)}</span>` : '';
    const gut = r.depth > 0 ? `<span class="gut" title="loop depth ${r.depth}">${'▍'.repeat(Math.min(r.depth, 4))}</span>` : '';
    return `<tr data-line="${r.line || 0}" data-mir="${typeof r.idx === 'number' ? r.idx : -1}" ${bg}>
      <td class="off">${(r.off || 0).toString(16).padStart(4, '0')}</td>
      <td class="gutcol">${gut}</td>
      <td class="bytes" title="${esc(r.bytes)}">${esc(r.bytes)}</td>
      <td class="asm">${esc(asm)}</td>
      <td class="cost">${cost}</td>
      <td class="src">${ln}</td>
      <td class="dec">${dec}</td></tr>`;
  };

  const flatRows = (f) => {
    const insns = f.insns || [];
    const maxHeat = insns.reduce((mx, r) => Math.max(mx, insnHeat(r)), 1);
    return insns.map((r) => insnRow(r, maxHeat)).join('');
  };

  const lineRows = (f) => {
    const insns = f.insns || [];
    const maxHeat = insns.reduce((mx, r) => Math.max(mx, insnHeat(r)), 1);
    let out = '', cur = null;
    const flush = () => {
      if (!cur) return;
      out += `<tr class="grp" data-line="${cur.line}"><td></td><td></td><td colspan="5">`
        + `<span class="ln" data-line="${cur.line}">${cur.line ? 'line ' + cur.line : 'prologue/epilogue'}</span>`
        + `<span class="gmeta">${cur.bytes} bytes · ~${fmtCyc(cur.cyc)} cyc · ${cur.items.length} ops</span></td></tr>`;
      out += cur.items.map((r) => insnRow(r, maxHeat)).join('');
      cur = null;
    };
    for (const r of insns) {
      const key = r.line || 0;
      if (!cur || cur.line !== key) { flush(); cur = { line: key, items: [], bytes: 0, cyc: 0 }; }
      cur.items.push(r); cur.bytes += r.len || 0; cur.cyc += r.rthru || 0;
    }
    flush();
    return out;
  };

  const remarkBlock = (f) => (f.remarks || []).map((r) => {
    const cls = r.positive ? 'pos' : 'neg';
    let h = `<div class="remark ${cls}"><span class="rk" data-line="${r.line}">${esc(r.entity)} @ ${r.line}</span>: ${esc(r.headline || '')}`;
    if (r.reason) h += `<div class="why">reason: ${esc(r.reason)}</div>`;
    if (r.fix) h += `<div class="why">fix: ${esc(r.fix)}</div>`;
    if (r.verified) h += `<div class="why ok">verified: ${esc(r.verified)}</div>`;
    return h + '</div>';
  }).join('');

  const sections = funcs.map((f, i) => `
    <section class="fn" id="fn-${i}" data-name="${esc((f.name || '').toLowerCase())}">
      <h2>${esc(f.name)} <span class="meta">${esc(f.file ? path.basename(f.file) : '')}:${f.line} · ${f.byte_size} bytes · ${esc(f.backend || '')}${f.backendReason ? ' ' + esc(f.backendReason) : ''}</span></h2>
      ${summaryCard(f)}
      ${loopCards(f)}
      ${remarkBlock(f) ? `<div class="remarks">${remarkBlock(f)}</div>` : ''}
      ${regTimeline(f, i)}
      <table><thead><tr><th>addr</th><th></th><th>bytes</th><th>asm (${syntax})</th><th>cost</th><th>src</th><th>decision</th></tr></thead>
      <tbody>${group === 'line' ? lineRows(f) : flatRows(f)}</tbody></table>
    </section>`).join('');

  const fbCount = funcs.filter((f) => f.backend && f.backend.indexOf('fallback') >= 0).length;
  const toc = funcs.map((f, i) => {
    const fb = f.backend && f.backend.indexOf('fallback') >= 0;
    return `<a href="#fn-${i}" class="toc ${fb ? 'fb' : ''}" data-name="${esc((f.name || '').toLowerCase())}"><i></i>${esc(f.name)}</a>`;
  }).join('');
  const tocPanel = `<div class="tocpanel">
    <div class="toctitle">
      <span class="cnt">${funcs.length} function${funcs.length === 1 ? '' : 's'}</span>
      <span class="toclegend">
        <span><i class="ra"></i>register-allocated</span>
        ${fbCount ? `<span><i class="fbi"></i>baseline · ${fbCount}</span>` : ''}
      </span>
      <input class="tocfilter" id="tocfilter" type="text" placeholder="filter functions..." spellcheck="false">
    </div>
    <div class="tocwrap">${toc}</div>
  </div>`;

  return `<!DOCTYPE html><html><head><meta charset="utf-8"><style>
    :root { --mono: var(--vscode-editor-font-family, monospace); }
    body { font-family: var(--vscode-font-family); font-size: 13px; line-height: 1.5; color: var(--vscode-foreground); padding: 0 14px 48px; }
    /* toolbar */
    .bar { position: sticky; top: 0; background: var(--vscode-editor-background); padding: 10px 0; margin-bottom: 2px; border-bottom: 1px solid var(--vscode-panel-border); z-index: 5; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    .fname { font-family: var(--mono); }
    .segmented { display: inline-flex; border: 1px solid var(--vscode-panel-border); border-radius: 6px; overflow: hidden; }
    .segmented button { border: none; border-radius: 0; background: transparent; color: var(--vscode-foreground); padding: 3px 11px; cursor: pointer; opacity: 0.65; }
    .segmented button + button { border-left: 1px solid var(--vscode-panel-border); }
    .segmented button.active { background: var(--vscode-button-background); color: var(--vscode-button-foreground); opacity: 1; }
    button.ghost { background: transparent; border: 1px solid var(--vscode-panel-border); border-radius: 6px; color: var(--vscode-foreground); opacity: 0.8; padding: 3px 11px; cursor: pointer; }
    button.ghost:hover { opacity: 1; }
    .hint { margin-left: auto; opacity: 0.45; font-size: 0.85em; }
    /* toc: a tidy, filterable function index instead of a wall of links */
    .tocpanel { margin: 6px 0 4px; background: var(--vscode-editorWidget-background, rgba(127,127,127,0.04)); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 8px; padding: 9px 12px; }
    .toctitle { display: flex; align-items: center; gap: 12px; margin-bottom: 8px; }
    .toctitle .cnt { font-size: 0.82em; text-transform: uppercase; letter-spacing: 0.05em; opacity: 0.6; font-weight: 600; }
    .toclegend { display: flex; gap: 14px; font-size: 0.78em; opacity: 0.7; }
    .toclegend span { display: inline-flex; align-items: center; gap: 5px; white-space: nowrap; }
    .toclegend i { width: 7px; height: 7px; border-radius: 50%; }
    .toclegend i.ra { background: var(--vscode-charts-green, #4ec994); }
    .toclegend i.fbi { background: var(--vscode-charts-purple, #c678dd); }
    .tocfilter { margin-left: auto; background: var(--vscode-input-background); color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border, var(--vscode-panel-border)); border-radius: 5px; padding: 3px 9px; font-size: 0.86em; min-width: 150px; }
    .tocfilter:focus { outline: none; border-color: var(--vscode-focusBorder); }
    .tocwrap { display: flex; flex-wrap: wrap; gap: 5px 6px; max-height: 7.5em; overflow-y: auto; align-content: flex-start; }
    .toc { display: inline-flex; align-items: center; gap: 6px; color: var(--vscode-foreground); text-decoration: none; white-space: nowrap; background: rgba(127,127,127,0.09); border: 1px solid var(--vscode-panel-border); border-radius: 5px; padding: 2px 9px; font-size: 0.85em; font-family: var(--mono); opacity: 0.88; }
    .toc:hover { opacity: 1; background: var(--vscode-list-hoverBackground); border-color: var(--vscode-focusBorder); }
    .toc i { width: 6px; height: 6px; border-radius: 50%; background: var(--vscode-charts-green, #4ec994); flex: 0 0 auto; }
    .toc.fb { opacity: 0.72; }
    .toc.fb i { background: var(--vscode-charts-purple, #c678dd); }
    /* function sections: skip layout/paint of off-screen functions so a file
       with 100+ functions stays responsive (the browser renders only what's
       near the viewport; contain-intrinsic-size keeps the scrollbar stable). */
    section.fn { content-visibility: auto; contain-intrinsic-size: auto 700px; }
    /* function header */
    h2 { font-size: 1.1em; margin: 22px 0 8px; padding-top: 14px; border-top: 1px solid var(--vscode-panel-border); font-family: var(--mono); }
    h2 .meta { font-weight: normal; opacity: 0.5; font-size: 0.72em; font-family: var(--vscode-font-family); margin-left: 6px; }
    /* cards */
    .card { background: var(--vscode-editorWidget-background, rgba(127,127,127,0.045)); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 8px; padding: 10px 12px; margin: 8px 0; }
    /* summary */
    .summary .chips { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 10px; }
    .chip { font-size: 0.82em; padding: 2px 9px; border-radius: 999px; background: rgba(127,127,127,0.14); white-space: nowrap; }
    .chip em { font-style: normal; opacity: 0.55; }
    .chip.accent { background: rgba(97,175,239,0.18); color: var(--vscode-charts-blue, #61afef); font-weight: 600; }
    .chip.warn { background: rgba(224,108,117,0.18); color: var(--vscode-charts-red, #e06c75); }
    .chip.good { background: rgba(78,201,148,0.16); color: var(--vscode-charts-green, #4ec994); }
    .mixbar { display: flex; height: 7px; border-radius: 4px; overflow: hidden; gap: 1px; }
    .mixbar .seg { min-width: 2px; }
    .legend { display: flex; flex-wrap: wrap; gap: 4px 14px; margin-top: 9px; opacity: 0.7; font-size: 0.8em; }
    .legend .lg { white-space: nowrap; }
    .legend .lg i { display: inline-block; width: 8px; height: 8px; border-radius: 2px; margin-right: 5px; }
    .legend .lg em { font-style: normal; opacity: 0.5; }
    /* loop cards */
    .loops { display: flex; flex-wrap: wrap; gap: 10px; margin: 8px 0; }
    .loop { flex: 1 1 250px; min-width: 240px; max-width: 380px; padding: 10px 14px 12px; }
    .loop .lh { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; margin-bottom: 7px; }
    .loop .ln { color: var(--vscode-textLink-foreground); cursor: pointer; font-family: var(--mono); font-size: 0.92em; }
    .loop .depthb { font-size: 0.72em; padding: 1px 6px; border-radius: 999px; background: rgba(198,120,221,0.2); color: var(--vscode-charts-purple, #c678dd); }
    .loop .metric { display: flex; align-items: baseline; flex-wrap: wrap; gap: 7px; margin-bottom: 11px; padding-bottom: 10px; border-bottom: 1px solid var(--vscode-panel-border); }
    .loop .cyc { font-family: var(--mono); font-weight: 700; font-size: 1.55em; line-height: 1; color: var(--vscode-foreground); font-variant-numeric: tabular-nums; }
    .loop .unit { opacity: 0.55; font-size: 0.8em; }
    .loop .sep { opacity: 0.3; }
    .loop .bnlabel { opacity: 0.55; font-size: 0.82em; }
    .loop .bn { font-family: var(--mono); font-size: 0.84em; font-weight: 600; padding: 1px 7px; border-radius: 4px; background: rgba(224,108,117,0.18); color: var(--vscode-charts-red, #e06c75); }
    .loop .kbadge { font-size: 0.74em; color: var(--vscode-charts-green, #4ec994); margin-left: auto; align-self: center; }
    .loop.coarse { border-style: dashed; }
    .estnote { margin-top: 8px; opacity: 0.5; font-size: 0.78em; font-style: italic; line-height: 1.4; }
    .loop .estnote { margin-top: 9px; }
    .pports { display: grid; gap: 5px; }
    .pp { display: grid; grid-template-columns: 2.8em 1fr 2.8em; align-items: center; gap: 10px; font-size: 0.82em; }
    .pp .ppn { text-align: right; opacity: 0.7; font-family: var(--mono); }
    .pp .ppbar { height: 8px; background: rgba(127,127,127,0.18); border-radius: 4px; overflow: hidden; }
    .pp .ppbar b { display: block; height: 100%; background: var(--vscode-charts-blue, #61afef); border-radius: 4px; }
    .pp .ppv { text-align: right; opacity: 0.7; font-family: var(--mono); font-variant-numeric: tabular-nums; }
    .pp.hot .ppn, .pp.hot .ppv { opacity: 1; font-weight: 700; color: var(--vscode-charts-red, #e06c75); }
    .pp.hot .ppbar b { background: var(--vscode-charts-red, #e06c75); }
    /* register-allocation timeline */
    .rmwrap { margin: 8px 0; }
    .rmwrap > summary { cursor: pointer; opacity: 0.6; font-size: 0.85em; padding: 2px 0; outline: none; }
    .rmwrap > summary:hover { opacity: 0.9; }
    .rmhost { position: relative; margin-top: 6px; }
    .rmtoolbar { display: flex; align-items: center; gap: 10px; margin-bottom: 5px; font-size: 0.82em; }
    .rmseg { display: inline-flex; border: 1px solid var(--vscode-panel-border); border-radius: 6px; overflow: hidden; }
    .rmtoggle { border: none; border-radius: 0; background: transparent; color: var(--vscode-foreground); padding: 2px 9px; cursor: pointer; opacity: 0.6; font-size: 0.95em; }
    .rmtoggle + .rmtoggle { border-left: 1px solid var(--vscode-panel-border); }
    .rmtoggle.active { background: var(--vscode-button-background); color: var(--vscode-button-foreground); opacity: 1; }
    .rmhintx { opacity: 0.4; }
    .rmreadout { margin-left: auto; font-family: var(--mono); opacity: 0.85; font-variant-numeric: tabular-nums; white-space: nowrap; }
    .rmreadout b { color: var(--vscode-charts-blue, #61afef); }
    .rmsvgbox { background: var(--vscode-editorWidget-background, rgba(127,127,127,0.04)); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 8px; padding: 4px 0; }
    .rmsvg2 { display: block; }
    .rmsvg2 .rl { fill: var(--vscode-foreground); opacity: 0.65; font-size: 9.5px; font-family: var(--mono); }
    .rmsvg2 .rl.unused { opacity: 0.22; }
    .rmsvg2 .lanebg { fill: transparent; }
    .rmsvg2 .lanebg.alt { fill: var(--vscode-foreground); opacity: 0.03; }
    .rmsvg2 .band { fill: var(--vscode-charts-orange, #d19a66); opacity: 0.09; }
    .rmsvg2 .callline { stroke: var(--vscode-charts-red, #e06c75); stroke-width: 1; stroke-dasharray: 2 3; opacity: 0.5; }
    .rmsvg2 .presAll { fill: var(--vscode-charts-yellow, #e5c07b); opacity: 0.22; }
    .rmsvg2 .presGp { fill: var(--vscode-charts-blue, #61afef); opacity: 0.30; }
    .rmsvg2 .peakline { stroke: var(--vscode-charts-red, #e06c75); stroke-width: 1; opacity: 0.8; }
    .rmsvg2 .peaklbl { fill: var(--vscode-charts-red, #e06c75); font-size: 9px; font-family: var(--mono); }
    .rmsvg2 .axlbl { fill: var(--vscode-foreground); opacity: 0.4; font-size: 8.5px; font-family: var(--vscode-font-family); }
    .rmsvg2 .tick { stroke: var(--vscode-foreground); opacity: 0.25; }
    .rmsvg2 .ticklbl { fill: var(--vscode-foreground); opacity: 0.45; font-size: 8px; font-family: var(--mono); text-anchor: middle; }
    .rmsvg2 .iv { cursor: pointer; }
    .rmsvg2 .iv.gp { fill: var(--vscode-charts-blue, #61afef); opacity: 0.85; }
    .rmsvg2 .iv.xmm { fill: var(--vscode-charts-yellow, #e5c07b); opacity: 0.85; }
    .rmsvg2 .iv.lc { stroke: var(--vscode-charts-purple, #c678dd); stroke-width: 1.5; }
    .rmsvg2 .iv.cc { stroke: var(--vscode-charts-red, #e06c75); stroke-width: 1; stroke-dasharray: 2 2; }
    .rmsvg2 .iv.hot { opacity: 1; filter: brightness(1.25); }
    .rmsvg2 .playhead { stroke: var(--vscode-foreground); stroke-width: 1; opacity: 0; pointer-events: none; }
    .rmtip { position: absolute; pointer-events: none; display: none; z-index: 20; background: var(--vscode-editorHoverWidget-background, #252526); border: 1px solid var(--vscode-editorHoverWidget-border, var(--vscode-panel-border)); border-radius: 5px; padding: 5px 8px; font-size: 0.8em; line-height: 1.5; box-shadow: 0 2px 8px rgba(0,0,0,0.35); max-width: 280px; }
    .rmtip .tk { font-family: var(--mono); color: var(--vscode-charts-blue, #61afef); }
    .rmtip .flag { display: inline-block; margin-top: 2px; font-size: 0.92em; }
    .rmtip .flag.cc { color: var(--vscode-charts-red, #e06c75); }
    .rmtip .flag.lc { color: var(--vscode-charts-purple, #c678dd); }
    tr.mirhl { background: var(--vscode-editor-selectionBackground); opacity: 1; }
    /* remarks */
    .remarks { margin: 8px 0; }
    .remark { margin: 3px 0; padding-left: 9px; border-left: 2px solid var(--vscode-panel-border); }
    .remark.pos { border-left-color: var(--vscode-charts-green, #4ec994); }
    .remark.neg { border-left-color: var(--vscode-charts-red, #e06c75); }
    .remark .rk { color: var(--vscode-textLink-foreground); cursor: pointer; }
    .why { opacity: 0.65; padding-left: 12px; white-space: normal; }
    .why.ok { color: var(--vscode-charts-green, #4ec994); opacity: 0.95; }
    /* listing table */
    table { border-collapse: collapse; width: 100%; margin-top: 8px; font-family: var(--mono); }
    td, th { text-align: left; padding: 1px 12px 1px 0; vertical-align: top; white-space: pre; }
    th { opacity: 0.45; font-weight: normal; border-bottom: 1px solid var(--vscode-panel-border); font-family: var(--vscode-font-family); font-size: 0.85em; padding-bottom: 5px; }
    td.off { opacity: 0.4; }
    td.gutcol { padding: 0; width: 1.4em; }
    .gut { color: var(--vscode-charts-red, #e06c75); letter-spacing: -2px; }
    td.bytes { opacity: 0.4; max-width: 16ch; overflow: hidden; text-overflow: ellipsis; }
    td.asm { color: var(--vscode-symbolIcon-functionForeground, #dcb67a); }
    td.cost { opacity: 0.55; font-size: 0.9em; }
    td.cost .lat { white-space: nowrap; cursor: help; font-variant-numeric: tabular-nums; }
    td.src .ln { color: var(--vscode-textLink-foreground); cursor: pointer; }
    td.dec { white-space: normal; opacity: 0.85; }
    tr:hover { background: var(--vscode-list-hoverBackground); }
    tr.hl { background: var(--vscode-editor-selectionBackground); }
    tr.grp td { padding-top: 9px; }
    tr.grp .ln { color: var(--vscode-textLink-foreground); cursor: pointer; font-weight: 600; font-family: var(--vscode-font-family); }
    tr.grp .gmeta { opacity: 0.45; margin-left: 12px; font-size: 0.85em; font-family: var(--vscode-font-family); }
    .tag { padding: 0 6px; border-radius: 4px; font-size: 0.82em; }
    .tag.vectorized { background: rgba(78,201,148,0.22); }
    .tag.idiom { background: rgba(97,175,239,0.22); }
    .tag.strength { background: rgba(198,120,221,0.22); }
    .tag.spill { background: rgba(224,108,117,0.22); }
    .tag.call, .tag.fold, .tag.frame, .tag.alloc, .tag.inlined { background: rgba(150,150,150,0.18); }
  </style></head><body>
    <div class="bar">
      <strong class="fname">${esc(path.basename(filePath))}</strong>
      <span class="segmented">
        <button id="intel" class="${syntax === 'intel' ? 'active' : ''}">Intel</button>
        <button id="att" class="${syntax === 'att' ? 'active' : ''}">AT&amp;T</button>
      </span>
      <span class="segmented">
        <button id="g-flat" class="${group !== 'line' ? 'active' : ''}">Flat</button>
        <button id="g-line" class="${group === 'line' ? 'active' : ''}">By line</button>
      </span>
      <button id="refresh" class="ghost">⟳ Refresh</button>
      <span class="hint">${funcs.length} function${funcs.length === 1 ? '' : 's'} · cost ≈ Skylake latency·throughput</span>
    </div>
    ${tocPanel}
    ${sections || '<p>No annotated functions. Compile a file with functions to see codegen decisions.</p>'}
    <script>
      const vscodeApi = acquireVsCodeApi();
      const RM = ${JSON.stringify(funcs.map(timelineData))};
      function jump(line) { vscodeApi.postMessage({ type: 'jump', line: Number(line) }); }
      // Live filter: narrow the index chips and the function sections together.
      (function(){
        const filt = document.getElementById('tocfilter');
        if (!filt) return;
        filt.addEventListener('input', () => {
          const q = filt.value.trim().toLowerCase();
          document.querySelectorAll('.toc').forEach((a) => { a.style.display = (!q || (a.dataset.name || '').indexOf(q) >= 0) ? '' : 'none'; });
          document.querySelectorAll('section.fn').forEach((s) => { s.style.display = (!q || (s.dataset.name || '').indexOf(q) >= 0) ? '' : 'none'; });
        });
        filt.addEventListener('keydown', (e) => { if (e.key === 'Escape') { filt.value = ''; filt.dispatchEvent(new Event('input')); } });
      })();
      document.body.addEventListener('click', (e) => {
        const t = e.target;
        if (t.classList && t.classList.contains('rmtoggle')) return;
        if (t.dataset && t.dataset.line && Number(t.dataset.line) > 0) jump(t.dataset.line);
        if (t.id === 'intel') vscodeApi.postMessage({ type: 'syntax', syntax: 'intel' });
        if (t.id === 'att') vscodeApi.postMessage({ type: 'syntax', syntax: 'att' });
        if (t.id === 'g-flat') vscodeApi.postMessage({ type: 'group', group: 'flat' });
        if (t.id === 'g-line') vscodeApi.postMessage({ type: 'group', group: 'line' });
        if (t.id === 'refresh') vscodeApi.postMessage({ type: 'refresh' });
      });
      window.addEventListener('message', (ev) => {
        const m = ev.data;
        if (m.type === 'highlight') {
          document.querySelectorAll('tr.hl').forEach((r) => r.classList.remove('hl'));
          let first = null;
          document.querySelectorAll('tr[data-line="' + m.line + '"]').forEach((r) => { r.classList.add('hl'); if (!first) first = r; });
          if (first) first.scrollIntoView({ block: 'center', behavior: 'smooth' });
        }
      });

      // ---- interactive register-allocation timeline ----
      var GPORD = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15'];
      function uniq(a) { var o = []; for (var i = 0; i < a.length; i++) if (o.indexOf(a[i]) < 0) o.push(a[i]); return o; }
      function lineAt(d, mir) {
        if (d.line[mir]) return d.line[mir];
        for (var k = mir; k >= 0; k--) if (d.line[k]) return d.line[k];
        return 0;
      }
      function buildOne(host) {
        var d = RM[Number(host.dataset.i)];
        if (!d) { host.style.display = 'none'; return; }
        var box = host.querySelector('.rmsvgbox');
        var readout = host.querySelector('.rmreadout');
        var tip = host.querySelector('.rmtip');
        var section = host.closest('section');

        function lanesFor(mode) {
          var present = {}; var gpU = [], xU = [];
          for (var i = 0; i < d.intervals.length; i++) {
            var iv = d.intervals[i]; present[iv.name] = 1;
            if (iv.cls === 0) gpU.push(iv.name); else xU.push(iv.name);
          }
          var lanes = [];
          var xPre = xU.join(' ').indexOf('ymm') >= 0 ? 'ymm' : 'xmm';
          if (mode === 'all') {
            for (var g = 0; g < GPORD.length; g++) lanes.push({ name: GPORD[g], cls: 0, used: !!present[GPORD[g]] });
            for (var x = 0; x < 16; x++) { var nm = xPre + x; lanes.push({ name: nm, cls: 1, used: !!present[nm] }); }
          } else {
            var gpO = GPORD.filter(function (c) { return gpU.indexOf(c) >= 0; });
            var xs = uniq(xU); var xcanon = []; for (var q = 0; q < 16; q++) { xcanon.push('xmm' + q); xcanon.push('ymm' + q); }
            var xO = xcanon.filter(function (c) { return xs.indexOf(c) >= 0; });
            for (var a = 0; a < gpO.length; a++) lanes.push({ name: gpO[a], cls: 0, used: 1 });
            for (var b = 0; b < xO.length; b++) lanes.push({ name: xO[b], cls: 1, used: 1 });
          }
          return lanes;
        }

        function render() {
          var mode = host.dataset.mode || 'used';
          var lanes = lanesFor(mode);
          var laneIx = {}; for (var i = 0; i < lanes.length; i++) laneIx[lanes[i].name] = i;
          var W = Math.max(440, host.clientWidth || 720);
          var labelW = 46, padR = 12, rowH = 14, presH = 42, top = presH + 16, axisH = 18;
          var plotW = W - labelW - padR;
          var laneH = lanes.length * rowH;
          var H = top + laneH + axisH;
          var axis = Math.max(1, d.axis);
          function X(mir) { return labelW + Math.max(0, Math.min(mir, axis)) / axis * plotW; }

          var buckets = Math.min(axis + 1, Math.max(64, Math.floor(plotW)));
          var gpP = [], allP = [], peak = 0, peakAt = 0;
          for (var bi = 0; bi < buckets; bi++) {
            var xm = buckets <= 1 ? 0 : bi / (buckets - 1) * axis;
            var g = 0, tot = 0;
            for (var j = 0; j < d.intervals.length; j++) {
              var iv = d.intervals[j];
              if (iv.start <= xm && xm <= iv.end) { tot++; if (iv.cls === 0) g++; }
            }
            gpP.push(g); allP.push(tot);
            if (tot > peak) { peak = tot; peakAt = xm; }
          }
          var maxP = Math.max(1, peak);
          function presY(v) { return top - 3 - v / maxP * (presH - 8); }
          function area(arr) {
            var s = 'M ' + labelW.toFixed(1) + ' ' + (top - 3).toFixed(1);
            for (var i = 0; i < arr.length; i++) {
              var x = labelW + (arr.length <= 1 ? 0 : i / (arr.length - 1) * plotW);
              s += ' L ' + x.toFixed(1) + ' ' + presY(arr[i]).toFixed(1);
            }
            return s + ' L ' + (labelW + plotW).toFixed(1) + ' ' + (top - 3).toFixed(1) + ' Z';
          }

          var s = '<svg viewBox="0 0 ' + W + ' ' + H + '" width="100%" height="' + H + '" class="rmsvg2" preserveAspectRatio="xMidYMid meet">';
          for (var li = 0; li < lanes.length; li++) {
            var y = top + li * rowH;
            s += '<rect class="lanebg' + (li % 2 ? ' alt' : '') + '" x="' + labelW + '" y="' + y + '" width="' + plotW + '" height="' + rowH + '"></rect>';
          }
          for (var lp = 0; lp < d.loops.length; lp++) {
            var L = d.loops[lp], x0 = X(L.a), x1 = X(L.b);
            s += '<rect class="band" x="' + x0.toFixed(1) + '" y="' + top + '" width="' + Math.max(2, x1 - x0).toFixed(1) + '" height="' + laneH + '"></rect>';
          }
          s += '<path class="presAll" d="' + area(allP) + '"></path>';
          s += '<path class="presGp" d="' + area(gpP) + '"></path>';
          s += '<text class="axlbl" x="2" y="' + (top - presH + 6) + '">live regs</text>';
          s += '<line class="peakline" x1="' + X(peakAt).toFixed(1) + '" y1="' + (top - presH) + '" x2="' + X(peakAt).toFixed(1) + '" y2="' + (top - 2) + '"></line>';
          s += '<text class="peaklbl" x="' + (X(peakAt) + 3).toFixed(1) + '" y="' + (top - presH + 8) + '">peak ' + peak + (d.spills ? ', ' + d.spills + ' spilled' : '') + '</text>';
          for (var cc = 0; cc < d.calls.length; cc++) {
            var cx = X(d.calls[cc]);
            s += '<line class="callline" x1="' + cx.toFixed(1) + '" y1="' + top + '" x2="' + cx.toFixed(1) + '" y2="' + (top + laneH) + '"></line>';
          }
          for (var ln = 0; ln < lanes.length; ln++) {
            var ly = top + ln * rowH;
            s += '<text class="rl' + (lanes[ln].used ? '' : ' unused') + '" x="0" y="' + (ly + rowH - 4) + '">' + lanes[ln].name + '</text>';
          }
          for (var iv2 = 0; iv2 < d.intervals.length; iv2++) {
            var v = d.intervals[iv2], lix = laneIx[v.name];
            if (lix == null) continue;
            var vy = top + lix * rowH, vx0 = X(v.start), vx1 = X(v.end), vw = Math.max(3, vx1 - vx0);
            var cls = (v.cls === 0 ? 'gp' : 'xmm') + (v.lc ? ' lc' : '') + (v.cc ? ' cc' : '');
            s += '<rect class="iv ' + cls + '" data-iv="' + iv2 + '" x="' + vx0.toFixed(1) + '" y="' + (vy + 2) + '" width="' + vw.toFixed(1) + '" height="' + (rowH - 4) + '" rx="2"></rect>';
          }
          var ticks = 6;
          for (var t = 0; t <= ticks; t++) {
            var mv = Math.round(t / ticks * axis), tx = X(mv);
            s += '<line class="tick" x1="' + tx.toFixed(1) + '" y1="' + (top + laneH) + '" x2="' + tx.toFixed(1) + '" y2="' + (top + laneH + 3) + '"></line>';
            s += '<text class="ticklbl" x="' + tx.toFixed(1) + '" y="' + (top + laneH + 12) + '">' + mv + '</text>';
          }
          s += '<text class="axlbl" x="' + (W - padR) + '" y="' + (H - 2) + '" text-anchor="end">MIR index</text>';
          s += '<line class="playhead" x1="-9" y1="' + top + '" x2="-9" y2="' + (top + laneH) + '"></line>';
          s += '</svg>';
          box.innerHTML = s;

          var svg = box.querySelector('svg');
          var ph = svg.querySelector('.playhead');
          function dataX(e) { var r = svg.getBoundingClientRect(); return (e.clientX - r.left) / r.width * W; }
          svg.addEventListener('mousemove', function (e) {
            var dx = dataX(e);
            var mir = Math.round(Math.max(0, Math.min(axis, (dx - labelW) / plotW * axis)));
            ph.setAttribute('x1', X(mir).toFixed(1)); ph.setAttribute('x2', X(mir).toFixed(1));
            ph.style.opacity = dx >= labelW ? '0.7' : '0';
            var lg = [], lx = 0;
            for (var i = 0; i < d.intervals.length; i++) {
              var iv = d.intervals[i];
              if (iv.start <= mir && mir <= iv.end) { if (iv.cls === 0) lg.push(iv.name); else lx++; }
            }
            readout.innerHTML = 'idx ' + mir + ' &middot; <b>' + lg.length + '</b> GP' + (lg.length ? ' (' + uniq(lg).join(' ') + ')' : '') + ' &middot; <b>' + lx + '</b> vec';
          });
          svg.addEventListener('mouseleave', function () { ph.style.opacity = '0'; readout.textContent = ''; });

          var bars = svg.querySelectorAll('rect.iv');
          for (var bi2 = 0; bi2 < bars.length; bi2++) {
            (function (bar) {
              var iv = d.intervals[Number(bar.dataset.iv)];
              bar.addEventListener('mouseenter', function () {
                bar.classList.add('hot');
                var html = '<span class="tk">' + iv.name + '</span> &middot; MIR [' + iv.start + ' .. ' + iv.end + ']';
                var sl = lineAt(d, iv.start), el = lineAt(d, iv.end);
                if (sl) html += '<br>source line' + (el && el !== sl ? 's ' + sl + ' to ' + el : ' ' + sl);
                if (iv.cc) html += '<br><span class="flag cc">crosses a call (kept callee-saved or spilled)</span>';
                if (iv.lc) html += '<br><span class="flag lc">loop-carried (reused every iteration)</span>';
                tip.innerHTML = html; tip.style.display = 'block';
                highlightRows(iv.start, iv.end, true);
              });
              bar.addEventListener('mousemove', function (e) {
                var hr = host.getBoundingClientRect();
                var lft = e.clientX - hr.left + 12, tp = e.clientY - hr.top + 12;
                if (lft + tip.offsetWidth > host.clientWidth) lft = host.clientWidth - tip.offsetWidth - 4;
                tip.style.left = lft + 'px'; tip.style.top = tp + 'px';
              });
              bar.addEventListener('mouseleave', function () { bar.classList.remove('hot'); tip.style.display = 'none'; highlightRows(0, 0, false); });
              bar.addEventListener('click', function () { var l = lineAt(d, iv.start); if (l) jump(l); });
            })(bars[bi2]);
          }
        }

        function highlightRows(lo, hi, on) {
          if (!section) return;
          var rows = section.querySelectorAll('tr[data-mir]');
          for (var i = 0; i < rows.length; i++) {
            var mir = Number(rows[i].getAttribute('data-mir'));
            if (on && mir >= 0 && mir >= lo && mir <= hi) rows[i].classList.add('mirhl');
            else rows[i].classList.remove('mirhl');
          }
        }

        var toggles = host.querySelectorAll('.rmtoggle');
        for (var ti = 0; ti < toggles.length; ti++) {
          (function (tg) {
            if (tg.dataset.mode === (host.dataset.mode || 'used')) tg.classList.add('active');
            tg.addEventListener('click', function () {
              host.dataset.mode = tg.dataset.mode;
              for (var k = 0; k < toggles.length; k++) toggles[k].classList.toggle('active', toggles[k].dataset.mode === tg.dataset.mode);
              render();
            });
          })(toggles[ti]);
        }
        render();
        host.__render = render;
      }
      // Build each register-timeline SVG only when its function scrolls into
      // view -- on a 100+ function file, building them all upfront is the main
      // source of slowness.
      function buildTimelines() {
        var hosts = document.querySelectorAll('.rmhost');
        if (typeof IntersectionObserver === 'function') {
          var io = new IntersectionObserver(function (entries) {
            for (var i = 0; i < entries.length; i++) {
              if (!entries[i].isIntersecting) continue;
              var h = entries[i].target;
              io.unobserve(h);
              // rAF: let content-visibility render the section so the SVG reads
              // a real width instead of the 0/placeholder size.
              if (!h.__built) { h.__built = 1; (function (host) { requestAnimationFrame(function () { buildOne(host); }); })(h); }
            }
          }, { rootMargin: '400px 0px' });
          for (var i = 0; i < hosts.length; i++) io.observe(hosts[i]);
        } else {
          for (var j = 0; j < hosts.length; j++) buildOne(hosts[j]);
        }
      }
      var rmResizeT = null;
      window.addEventListener('resize', function () {
        clearTimeout(rmResizeT);
        rmResizeT = setTimeout(function () {
          var hosts = document.querySelectorAll('.rmhost');
          for (var i = 0; i < hosts.length; i++) if (hosts[i].__render) hosts[i].__render();
        }, 150);
      });
      buildTimelines();
    </script>
  </body></html>`;
}

function loadingHtml(name) {
  return `<!DOCTYPE html><html><body style="font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 20px;">
    <p>Compiling <strong>${esc(name)}</strong> with <code>--annotate-asm</code>...</p></body></html>`;
}


async function refresh(deps, filePath) {
  const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(filePath));
  const workspaceRoot = folder ? folder.uri.fsPath : path.dirname(filePath);
  const result = await runAnnotate(deps, filePath, workspaceRoot);
  if (!result.ok) {
    if (panel) panel.webview.html = `<body style="padding:20px;font-family:var(--vscode-font-family);color:var(--vscode-foreground)"><p>Codegen annotation failed:</p><pre>${esc(result.error)}</pre></body>`;
    return;
  }
  model = result.json;
  sourcePath = filePath;
  if (panel) panel.webview.html = renderHtml(model, filePath, asmSyntax, asmGroup);
  applyDecorations();
  notifyChanged();
}

function syncHighlightFromEditor(editor) {
  if (!panel || !editor || !sourcePath) return;
  if (path.normalize(editor.document.uri.fsPath).toLowerCase() !== path.normalize(sourcePath).toLowerCase()) return;
  const line = editor.selection.active.line + 1;
  panel.webview.postMessage({ type: 'highlight', line });
}

function registerCodegen(context, deps) {
  asmSyntax = vscode.workspace.getConfiguration('mettle').get('codegen.asmSyntax', 'intel');

  const showAnnotations = async () => {
    const editor = vscode.window.activeTextEditor;
    const doc = editor?.document;
    if (!doc || doc.languageId !== 'mettle') {
      vscode.window.showInformationMessage('Open a Mettle file to show its codegen annotations.');
      return;
    }
    if (doc.isDirty) await doc.save();
    const filePath = doc.uri.fsPath;

    if (!panel) {
      panel = vscode.window.createWebviewPanel(
        'mettleCodegenAnnotations',
        'Mettle: Codegen Annotations',
        { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true },
        { enableScripts: true, retainContextWhenHidden: true }
      );
      panel.onDidDispose(() => {
        panel = null;
        model = null;
        sourcePath = null;
        applyDecorations();
        notifyChanged();
      }, null, context.subscriptions);
      panel.webview.onDidReceiveMessage(async (msg) => {
        try {
          if (msg.type === 'jump' && sourcePath) {
            const opened = await vscode.window.showTextDocument(vscode.Uri.file(sourcePath), { viewColumn: vscode.ViewColumn.One });
            const ln = Math.max(0, (msg.line || 1) - 1);
            const pos = new vscode.Position(Math.min(ln, opened.document.lineCount - 1), 0);
            opened.selection = new vscode.Selection(pos, pos);
            opened.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
          } else if (msg.type === 'syntax') {
            asmSyntax = msg.syntax === 'att' ? 'att' : 'intel';
            if (model && sourcePath) panel.webview.html = renderHtml(model, sourcePath, asmSyntax, asmGroup);
          } else if (msg.type === 'group') {
            asmGroup = msg.group === 'line' ? 'line' : 'flat';
            if (model && sourcePath) panel.webview.html = renderHtml(model, sourcePath, asmSyntax, asmGroup);
          } else if (msg.type === 'refresh' && sourcePath) {
            await refresh(deps, sourcePath);
          }
        } catch (err) {
          vscode.window.showErrorMessage(`Mettle codegen action failed: ${err && err.message ? err.message : err}`);
        }
      }, null, context.subscriptions);
    } else {
      panel.reveal(undefined, true);
    }
    if (sourcePath !== filePath || !model) panel.webview.html = loadingHtml(path.basename(filePath));
    await refresh(deps, filePath);
  };

  context.subscriptions.push(
    vscode.window.onDidChangeTextEditorSelection((e) => syncHighlightFromEditor(e.textEditor)),
    vscode.window.onDidChangeVisibleTextEditors(() => applyDecorations()),
    vscode.workspace.onDidSaveTextDocument(async (doc) => {
      if (!sourcePath) return;
      if (!vscode.workspace.getConfiguration('mettle').get('codegen.refreshOnSave', true)) return;
      if (path.normalize(doc.uri.fsPath).toLowerCase() === path.normalize(sourcePath).toLowerCase()) {
        await refresh(deps, sourcePath);
      }
    }),
    vscode.languages.registerCodeLensProvider({ language: 'mettle' }, new CodegenCodeLensProvider())
  );

  return { showAnnotations, revealFunction };
}

function revealFunction(name) {
  if (!panel || !model) return;
  const funcs = focusFunctions(model, sourcePath);
  const i = funcs.findIndex((f) => f.name === name);
  if (i >= 0) {
    panel.reveal(undefined, true);
    const f = funcs[i];
    if (f.line) panel.webview.postMessage({ type: 'highlight', line: f.line });
  }
}
const changeListeners = [];
function notifyChanged() { for (const cb of changeListeners) { try { cb(); } catch (_) {} } }

class CodegenCodeLensProvider {
  constructor() {
    this._emitter = new vscode.EventEmitter();
    this.onDidChangeCodeLenses = this._emitter.event;
    changeListeners.push(() => this._emitter.fire());
  }
  provideCodeLenses(document) {
    if (!model || !sourcePath) return [];
    if (path.normalize(document.uri.fsPath).toLowerCase() !== path.normalize(sourcePath).toLowerCase()) return [];
    const lenses = [];
    for (const f of focusFunctions(model, sourcePath)) {
      if (!f.line) continue;
      const counts = {};
      for (const ins of f.insns || []) if (ins.tag) counts[ins.tag] = (counts[ins.tag] || 0) + 1;
      const interesting = ['vectorized', 'idiom', 'strength-reduce', 'spill']
        .filter((t) => counts[t]).map((t) => `${counts[t]} ${t}`);
      const backend = f.backend && f.backend.indexOf('fallback') >= 0 ? 'baseline' : 'reg-alloc';
      const extra = [];
      const hot = (f.loops || []).slice().sort((a, b) => (b.cyclesPerIter || 0) - (a.cyclesPerIter || 0))[0];
      if (hot) extra.push(`$(sync) ~${(hot.cyclesPerIter / 100).toFixed(1)}c/iter on ${hot.bottleneck}`);
      if (f.summary && f.summary.spills) extra.push(`$(warning) ${f.summary.spills} spill${f.summary.spills > 1 ? 's' : ''}`);
      const tail = [...interesting, ...extra];
      const title = `$(symbol-misc) codegen: ${backend}, ${f.byte_size}B${tail.length ? ' - ' + tail.join(', ') : ''}`;
      const ln = Math.max(0, Math.min(f.line - 1, document.lineCount - 1));
      lenses.push(new vscode.CodeLens(new vscode.Range(ln, 0, ln, 0), {
        title,
        command: 'mettle.showCodegenAnnotations',
        arguments: [],
      }));
    }
    return lenses;
  }
}

module.exports = { registerCodegen, renderHtml, focusFunctions, lineDecisions };
