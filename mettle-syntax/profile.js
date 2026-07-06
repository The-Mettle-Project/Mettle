// VTune-style measured codegen profiler.
//
// Compiles the active file with `--annotate-asm --profile-blocks --build`, runs
// the instrumented binary (which dumps a .mprof sidecar of per-basic-block
// execution counts), then fuses the measured frequency with the static
// per-instruction Skylake port model the annotator already produces. The result
// is a real "where the cycles go" view: hotspot grid, issue-port utilization,
// measured loop trip counts, source+asm heat, and a call-graph flame graph.

const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { execFile } = require('child_process');

const RES = ['p0', 'p1', 'p5', 'p6', 'load', 'store'];
const ALU = [0, 1, 2, 3]; // p0/p1/p5/p6 carry flexible ALU work

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}
function fmtBig(n) {
  n = Number(n) || 0;
  const a = Math.abs(n);
  if (a >= 1e9) return (n / 1e9).toFixed(2) + 'G';
  if (a >= 1e6) return (n / 1e6).toFixed(2) + 'M';
  if (a >= 1e3) return (n / 1e3).toFixed(1) + 'K';
  return String(Math.round(n));
}
function fmtPct(x) { x = Number(x) || 0; return (x * 100).toFixed(x >= 0.1 ? 1 : 2) + '%'; }
function fmtNs(ns) {
  ns = Number(ns) || 0;
  if (ns >= 1e9) return (ns / 1e9).toFixed(2) + ' s';
  if (ns >= 1e6) return (ns / 1e6).toFixed(2) + ' ms';
  if (ns >= 1e3) return (ns / 1e3).toFixed(1) + ' µs';
  return Math.round(ns) + ' ns';
}

function focusFunctions(m, filePath) {
  if (!m || !Array.isArray(m.functions)) return [];
  const base = path.basename(filePath).toLowerCase();
  const own = m.functions.filter((f) => f.file && path.basename(f.file).toLowerCase() === base);
  return own.length ? own : m.functions;
}

// Distribute `add` centicycles of flexible ALU work across the given port
// indices of press[] so as to minimize the resulting maximum -- the water-fill
// an out-of-order scheduler approximates (ported from the compiler's model).
function waterfillInto(press, add, idx) {
  if (add <= 0) return;
  const base = idx.map((i) => press[i]);
  let lo = Math.min.apply(null, base);
  let hi = lo + add;
  while (lo < hi) {
    const mid = lo + Math.floor((hi - lo + 1) / 2);
    let need = 0;
    for (const b of base) if (b < mid) need += mid - b;
    if (need <= add) lo = mid; else hi = mid - 1;
  }
  const out = base.slice();
  let used = 0;
  for (let i = 0; i < base.length; i++) if (base[i] < lo) { out[i] = lo; used += lo - base[i]; }
  let left = add - used;
  while (left-- > 0) {
    let mi = 0;
    for (let i = 1; i < out.length; i++) if (out[i] < out[mi]) mi = i;
    out[mi]++;
  }
  for (let i = 0; i < idx.length; i++) press[idx[i]] = out[i];
}

function boundOf(press) {
  let mi = 0;
  for (let i = 1; i < press.length; i++) if (press[i] > press[mi]) mi = i;
  return mi;
}

// Attach measured execution counts and measured cost to every instruction,
// aggregate per line / loop / function, and roll up program-wide port pressure.
function fuseProfile(model, mprof) {
  const blocks = Array.isArray(mprof.blocks) ? mprof.blocks.map((x) => Number(x) || 0) : [];
  const bc = (id) => (typeof id === 'number' && id >= 0 && id < blocks.length ? blocks[id] : 0);
  const funcs = model.functions || [];

  let progCyc = 0, progFlex = 0, progRetired = 0;
  const progPress = [0, 0, 0, 0, 0, 0];

  for (const f of funcs) {
    let fcyc = 0, fflex = 0, fretired = 0;
    const fpress = [0, 0, 0, 0, 0, 0];
    const lineAgg = new Map();
    for (const r of f.insns || []) {
      const cnt = bc(r.block);
      r.count = cnt;
      r.mcyc = (r.rthru || 0) / 100 * cnt;
      fcyc += r.mcyc;
      fretired += cnt;
      const pr = r.press || [0, 0, 0, 0, 0, 0];
      for (let i = 0; i < 6; i++) fpress[i] += (pr[i] || 0) * cnt;
      fflex += (r.falu || 0) * cnt;
      if (r.line) {
        const a = lineAgg.get(r.line) || { line: r.line, cyc: 0, count: 0, ops: 0 };
        a.cyc += r.mcyc; if (cnt > a.count) a.count = cnt; a.ops++;
        lineAgg.set(r.line, a);
      }
    }
    const portTotal = fpress.slice();
    waterfillInto(portTotal, fflex, ALU);
    f._mcyc = fcyc;
    f._retired = fretired;
    f._press = portTotal;
    f._bound = boundOf(portTotal);
    f._lines = Array.from(lineAgg.values()).sort((a, b) => b.cyc - a.cyc);
    f._loops = (f.loops || []).map((l) => {
      let cyc = 0, iters = 0;
      const a = l.startRec, b = l.endRec, insns = f.insns || [];
      if (typeof a === 'number' && typeof b === 'number') {
        for (let i = a; i <= b && i < insns.length; i++) {
          cyc += insns[i].mcyc || 0;
          if ((insns[i].count || 0) > iters) iters = insns[i].count;
        }
      }
      return { headLine: l.headLine, depth: l.depth || 0, bottleneck: l.bottleneck, mcyc: cyc, iters };
    });
    progCyc += fcyc; progFlex += fflex; progRetired += fretired;
    for (let i = 0; i < 6; i++) progPress[i] += fpress[i];
  }

  const portTotal = progPress.slice();
  waterfillInto(portTotal, progFlex, ALU);

  // wall-clock + call info from the measured runtime stats, joined by name.
  const stats = new Map();
  let wallNs = 0, rootName = '';
  for (const s of mprof.functions || []) {
    stats.set(s.name, s);
    if ((s.total_ns || 0) > wallNs) { wallNs = s.total_ns; rootName = s.name; }
  }
  const mainStat = (mprof.functions || []).find((s) => s.name === 'main');
  if (mainStat) { wallNs = mainStat.total_ns || wallNs; rootName = 'main'; }

  return {
    blocks, progCyc, progRetired, port: portTotal,
    bound: boundOf(portTotal), wallNs, rootName, stats,
  };
}

// ---- call-graph flame (icicle) tree from the measured edges ----------------
function buildCallTree(mprof) {
  const fns = mprof.functions || [];
  if (!fns.length) return null;
  const byId = new Map(fns.map((f) => [f.id, f]));
  const childMap = new Map();
  for (const e of mprof.edges || []) {
    if (!childMap.has(e.caller)) childMap.set(e.caller, []);
    childMap.get(e.caller).push(e);
  }
  let root = fns.find((f) => f.name === 'main');
  if (!root) root = fns.slice().sort((a, b) => (b.total_ns || 0) - (a.total_ns || 0))[0];
  if (!root) return null;
  const stack = new Set();
  const node = (id, value, calls) => {
    const f = byId.get(id) || {};
    if (stack.has(id)) return { name: f.name || '?', value, calls, line: f.line || 0, file: f.file || '', kids: [], rec: 1 };
    stack.add(id);
    const kids = (childMap.get(id) || [])
      .filter((e) => (e.total_ns || 0) > 0)
      .sort((a, b) => b.total_ns - a.total_ns)
      .map((e) => node(e.callee, e.total_ns, e.calls));
    stack.delete(id);
    return { name: f.name || '?', value: Math.max(value, 1), calls, line: f.line || 0, file: f.file || '', kids };
  };
  return node(root.id, root.total_ns || 1, root.calls || 1);
}

// ---- actionable insights ---------------------------------------------------
// Concrete advice for a loop bound on a given issue port.
function portAdvice(port) {
  switch (port) {
    case 'load': case 'store':
      return `Memory-bound (bound on the ${port} port). Cut memory traffic: block/tile the loop for cache locality, access arrays sequentially, switch array-of-structs to struct-of-arrays, or hoist invariant loads out of the loop.`;
    case 'p0':
      return `Bound on port 0 (integer/vector divide and some shifts). If the loop divides or takes a remainder by a value, make the divisor a compile-time constant so the strength-reducer can replace it with a multiply.`;
    case 'p1':
      return `Bound on port 1 (multiply). Reduce multiplies in the loop - strength-reduce multiplies by constants into shifts/adds, and hoist products that don't change each iteration.`;
    case 'p5':
      return `Bound on port 5 (vector shuffles / address generation). Simplify lane-crossing shuffles or complex addressing in the loop.`;
    case 'p6':
      return `Branch-bound (port 6): too many branches per iteration. Unroll the loop, or rewrite the inner test in a branchless form.`;
    default:
      return `Bound on the ${port} issue port.`;
  }
}

// Short category tag + color tone for a loop's bottleneck port.
function portTag(port) {
  switch (port) {
    case 'load': case 'store': return { tag: 'MEMORY', tone: 'mem' };
    case 'p6': return { tag: 'BRANCH', tone: 'branch' };
    case 'p0': return { tag: 'DIVIDE', tone: 'core' };
    case 'p1': return { tag: 'MULTIPLY', tone: 'core' };
    case 'p5': return { tag: 'SHUFFLE', tone: 'core' };
    default: return { tag: 'CORE', tone: 'core' };
  }
}

// Turn the fused profile + the optimizer's own remarks into a ranked, concrete
// to-do list. Each insight carries the source line it points at and a weight
// (its share of measured cycles) so the worst offenders sort to the top.
function buildInsights(model, fused, filePath) {
  const prog = fused.progCyc || 1;
  const funcs = focusFunctions(model, filePath);
  const out = [];

  const lineCyc = new Map();
  for (const f of funcs) for (const ln of f._lines || []) lineCyc.set(ln.line, (lineCyc.get(ln.line) || 0) + ln.cyc);

  // Hot loops first: diagnose the bottleneck port and say what to do about it.
  const loopLines = new Set();
  for (const f of funcs) {
    for (const l of (f._loops || []).slice().sort((a, b) => b.mcyc - a.mcyc)) {
      const share = l.mcyc / prog;
      if (share < 0.08) continue;
      loopLines.add(l.headLine);
      const bn = typeof l.bottleneck === 'number' ? RES[l.bottleneck] : l.bottleneck;
      out.push({ kind: 'loop', ...portTag(bn), line: l.headLine, weight: share,
        title: `Loop at line ${l.headLine}: ${fmtPct(share)} of cycles over ${fmtBig(l.iters)} iterations`,
        detail: portAdvice(bn) });
    }
  }

  // The single hottest source line -- unless a loop already explains that line.
  let topLine = 0, topCyc = 0;
  for (const [l, c] of lineCyc) if (c > topCyc) { topCyc = c; topLine = l; }
  if (topLine && topCyc / prog >= 0.15 && !loopLines.has(topLine)) {
    out.push({ kind: 'topline', tag: 'HOTSPOT', tone: 'hot', line: topLine, weight: topCyc / prog,
      title: `Line ${topLine} is ${fmtPct(topCyc / prog)} of all measured cycles`,
      detail: 'The single biggest cost in the program - optimize here first for the most impact.' });
  }

  // Register spills inside hot functions.
  for (const f of funcs) {
    const sp = f.summary && f.summary.spills;
    if (!sp) continue;
    const share = (f._mcyc || 0) / prog;
    if (share < 0.08) continue;
    out.push({ kind: 'spill', tag: 'SPILL', tone: 'spill', line: f.line, weight: share,
      title: `${f.name}: ${sp} register spill${sp > 1 ? 's' : ''} in hot code (${fmtPct(share)} of cycles)`,
      detail: 'The register allocator ran out of registers and is spilling to the stack inside hot code. Reduce how many values are live at once: split the loop, sink computations to their use, or shrink variable live ranges.' });
  }

  // Measured cost x the optimizer's existing fix suggestion: the killer combo --
  // "this line is expensive AND here is exactly what to change."
  for (const f of funcs) {
    for (const r of f.remarks || []) {
      if (!r.line || !r.fix) continue;
      const share = (lineCyc.get(r.line) || 0) / prog;
      if (share < 0.05) continue;
      out.push({ kind: 'fix', sev: 'fix', tag: 'FIX', tone: 'fix', line: r.line, weight: share + 0.001,
        title: `Line ${r.line} is ${fmtPct(share)} of cycles - the optimizer suggests a fix`,
        detail: r.fix, sub: r.reason || '' });
    }
  }

  // Collapse duplicates of the same kind on the same line, keep the heaviest.
  const seen = new Map();
  for (const ins of out) {
    const key = ins.line + ':' + ins.kind;
    if (!seen.has(key) || seen.get(key).weight < ins.weight) seen.set(key, ins);
  }
  return Array.from(seen.values()).sort((a, b) => b.weight - a.weight).slice(0, 7);
}

function insightsHtml(insights) {
  if (!insights.length) {
    return `<div class="insights"><div class="irow tone-core"><span class="itag">EVEN</span>
      <span class="iwrap"><span class="ititle">No single hotspot dominates</span>
      <span class="idetail">Measured cost is spread out, with no loop above ~8% of cycles.</span></span></div></div>`;
  }
  const rows = insights.map((ins) => `
    <div class="irow tone-${ins.tone}" data-line="${ins.line || 0}">
      <span class="itag">${esc(ins.tag)}</span>
      <span class="iwrap">
        <span class="ititle ln" data-line="${ins.line || 0}">${esc(ins.title)}</span>
        <span class="idetail">${esc(ins.detail)}</span>
        ${ins.sub ? `<span class="isub">${esc(ins.sub)}</span>` : ''}
      </span>
    </div>`).join('');
  return `<div class="insights">${rows}</div>`;
}

// ---------------------------------------------------------------------------
function runProfile(deps, filePath, workspaceRoot, programArgs) {
  const cfg = vscode.workspace.getConfiguration('mettle');
  const compiler = deps.findCompiler(workspaceRoot, filePath);
  const buildTimeout = Math.max(4000, Number(cfg.get('codegen.timeoutMs', 30000)) || 30000);
  const runTimeout = Math.max(5000, Number(cfg.get('profile.runTimeoutMs', 60000)) || 60000);

  const stem = path.basename(filePath, '.mettle').replace(/[^A-Za-z0-9_-]/g, '_');
  const hash = require('crypto').createHash('sha1').update(filePath.toLowerCase()).digest('hex').slice(0, 10);
  let tempDir;
  try {
    tempDir = path.join(os.tmpdir(), 'mettle-profile', `${stem}-${hash}`);
    fs.mkdirSync(tempDir, { recursive: true });
  } catch (err) {
    return Promise.resolve({ ok: false, error: `temp dir: ${err.message}` });
  }
  const exeName = process.platform === 'win32' ? 'profile.exe' : 'profile.bin';
  const exePath = path.join(tempDir, exeName);
  const jsonPath = path.join(tempDir, 'profile.annot.json');
  const mprofPath = path.join(tempDir, 'profile.mprof');
  try { fs.existsSync(mprofPath) && fs.unlinkSync(mprofPath); } catch (_) {}

  const args = [
    '-i', filePath, '-o', exePath,
    '--annotate-asm', '--asm-syntax=both', '--profile-blocks', '--build',
    '-I', path.dirname(filePath), '-I', workspaceRoot,
  ];
  const stdlibPath = cfg.get('linter.stdlibPath', '');
  if (stdlibPath) args.push('--stdlib', path.isAbsolute(stdlibPath) ? stdlibPath : path.join(workspaceRoot, stdlibPath));
  for (const inc of cfg.get('linter.extraIncludePaths', []) || []) {
    if (inc && typeof inc === 'string') args.push('-I', path.isAbsolute(inc) ? inc : path.join(workspaceRoot, inc));
  }

  return new Promise((resolve) => {
    execFile(compiler, args, {
      timeout: buildTimeout, maxBuffer: 64 * 1024 * 1024, cwd: workspaceRoot,
      env: { ...process.env, METTLE_EXPLAIN_REPORT_LINES: '0', NO_COLOR: '1' },
    }, (err) => {
      if (err && err.code === 'ENOENT') {
        resolve({ ok: false, error: `Compiler not found: ${compiler}. Set mettle.linter.compilerPath.` });
        return;
      }
      let json = null;
      try { json = JSON.parse(fs.readFileSync(jsonPath, 'utf8')); }
      catch (e) { resolve({ ok: false, error: err ? 'Compilation failed before the profiled binary was built. Check the build with --profile-blocks --build.' : `Could not read annotations: ${e.message}` }); return; }

      execFile(exePath, Array.isArray(programArgs) ? programArgs : [], {
        timeout: runTimeout, maxBuffer: 64 * 1024 * 1024, cwd: tempDir,
        env: { ...process.env, METTLE_PROFILE_OUT: mprofPath, NO_COLOR: '1' },
      }, (rerr) => {
        let mprof = null;
        try { mprof = JSON.parse(fs.readFileSync(mprofPath, 'utf8')); }
        catch (e) {
          const why = rerr && rerr.killed ? `the program ran longer than the ${Math.round(runTimeout / 1000)}s profile timeout (it may need input or loop forever).`
            : rerr ? `the program exited abnormally before writing a profile.` : `no .mprof was written (${e.message}).`;
          resolve({ ok: false, error: `Built and launched the instrumented binary, but ${why}` });
          return;
        }
        resolve({ ok: true, json, mprof });
      });
    });
  });
}

// ---- rendering -------------------------------------------------------------
const PORT_ROLE = ['integer ALU · divide', 'integer ALU · multiply', 'ALU · shuffle · LEA', 'ALU · branch', 'load address+data', 'store address+data'];

// Per-port utilization: one bar per issue port (relative to the busiest), with a
// plain-language verdict. Reads like VTune's port-utilization histogram.
function portPanel(press) {
  const total = press.reduce((a, b) => a + b, 0) || 1;
  const max = Math.max.apply(null, press.concat([1]));
  const bnd = boundOf(press);
  const core = press[0] + press[1] + press[2] + press[3];
  const mem = press[4] + press[5];
  const memBound = mem > core;
  const rows = RES.map((n, i) => {
    const isMem = i >= 4, hot = i === bnd;
    return `<div class="prow ${isMem ? 'mem' : 'core'}${hot ? ' hot' : ''}" title="${n} - ${esc(PORT_ROLE[i])}">
      <span class="pn">${n}</span>
      <span class="pbar"><b style="width:${(press[i] / max * 100).toFixed(1)}%"></b></span>
      <span class="pv">${fmtPct(press[i] / total)}</span></div>`;
  }).join('');
  return `<div class="portpanel">
    <div class="phead">
      <span class="pverdict ${memBound ? 'mem' : 'core'}">${memBound ? 'Memory-bound' : 'Compute-bound'}</span>
      <span class="pboundq">bottleneck: <b>${esc(RES[bnd])}</b> at ${fmtPct(press[bnd] / total)} · Core ${fmtPct(core / total)} / Memory ${fmtPct(mem / total)}</span>
    </div>
    <div class="pgrid">${rows}</div>
    <div class="estnote">share of measured issue cycles per port (exec count × static Skylake port model, flex ALU water-filled across p0/p1/p5/p6); bars are relative to the busiest port; not a hardware sample</div>
  </div>`;
}

// Flatten the measured call tree into indented rows (DFS, render order).
function flattenCallTree(node, depth, rootVal, stats, out) {
  if (!node) return;
  const st = stats.get(node.name);
  out.push({ name: node.name, depth, line: node.line || 0, total: node.value / (rootVal || 1), self: st ? st.self_ns : 0, calls: node.calls || 0, rec: !!node.rec });
  for (const k of node.kids || []) flattenCallTree(k, depth + 1, rootVal, stats, out);
}

// Top-down call tree with inclusive-time bars, self time and call counts -- the
// "where does the time go through the call hierarchy" view, click to jump.
function callTreeHtml(tree, fused) {
  if (!tree) return `<div class="ctree"><div class="ctempty">No call-graph timing was recorded (the program made no instrumented calls).</div></div>`;
  const rows = [];
  flattenCallTree(tree, 0, tree.value || 1, fused.stats, rows);
  const wall = fused.wallNs || 1;
  const body = rows.slice(0, 80).map((r) => `
    <div class="crow" data-line="${r.line || 0}" style="padding-left:${(8 + r.depth * 15)}px">
      <span class="cbarwrap"><span class="cbar"><b style="width:${(r.total * 100).toFixed(1)}%"></b></span></span>
      <span class="cpct">${fmtPct(r.total)}</span>
      <span class="cname ln" data-line="${r.line || 0}">${esc(r.name)}${r.rec ? ' <span class="crec">↺</span>' : ''}</span>
      <span class="cself" title="self time">${fmtNs(r.self)}</span>
      <span class="ccalls" title="call count">${fmtBig(r.calls)}×</span>
    </div>`).join('');
  return `<div class="ctree">
    <div class="chead"><span></span><span>total</span><span>function</span><span>self</span><span>calls</span></div>
    ${body}</div>`;
}

function hotspotGrid(funcs, fused, filePath, scope) {
  const prog = fused.progCyc || 1;
  let rows = [];
  if (scope === 'loop') {
    for (const f of funcs) for (const l of f._loops || []) {
      if (!l.mcyc) continue;
      rows.push({ name: `⟳ ${esc(f.name)} · line ${l.headLine}`, line: l.headLine, cyc: l.mcyc, extra: `${fmtBig(l.iters)}×`, bound: RES[/** @type {number} */(typeof l.bottleneck === 'string' ? RES.indexOf(l.bottleneck) : l.bottleneck)] || l.bottleneck || '' });
    }
  } else if (scope === 'line') {
    for (const f of funcs) for (const ln of f._lines || []) {
      if (!ln.cyc) continue;
      rows.push({ name: `${esc(path.basename(f.file || filePath))}:${ln.line}`, line: ln.line, cyc: ln.cyc, extra: `${fmtBig(ln.count)}×`, bound: '' });
    }
  } else {
    for (const f of funcs) {
      if (!f._mcyc) continue;
      const st = fused.stats.get(f.name);
      rows.push({ name: esc(f.name), line: f.line, cyc: f._mcyc, extra: st ? `${fmtBig(st.calls)} calls` : `${fmtBig(f._retired)} ret`, bound: RES[f._bound] || '' });
    }
  }
  rows.sort((a, b) => b.cyc - a.cyc);
  rows = rows.slice(0, 200);
  const max = rows.length ? rows[0].cyc : 1;
  const body = rows.map((r) => {
    const share = r.cyc / prog;
    const heat = Math.min(1, r.cyc / max);
    return `<tr data-line="${r.line || 0}">
      <td class="hg-pct"><span class="hgbar"><b style="width:${(share * 100).toFixed(1)}%;opacity:${(0.35 + 0.55 * heat).toFixed(2)}"></b></span><span class="hgv">${fmtPct(share)}</span></td>
      <td class="hg-cyc">${fmtBig(r.cyc)}</td>
      <td class="hg-x">${r.extra}</td>
      <td class="hg-bn">${r.bound ? `<span class="bn">${esc(r.bound)}</span>` : ''}</td>
      <td class="hg-nm"><span class="ln" data-line="${r.line || 0}">${r.name}</span></td>
    </tr>`;
  }).join('');
  const tabs = [['function', 'Functions'], ['loop', 'Loops'], ['line', 'Lines']]
    .map(([k, lbl]) => `<button class="hgtab ${scope === k ? 'active' : ''}" data-scope="${k}">${lbl}</button>`).join('');
  return `<div class="hotspot">
    <div class="hghead"><span class="hgtitle">Hotspots</span><span class="segmented">${tabs}</span></div>
    <table class="hgtable"><thead><tr><th>cycles %</th><th>Mcyc</th><th></th><th>port</th><th>${scope === 'function' ? 'function' : scope === 'loop' ? 'loop' : 'source line'}</th></tr></thead>
    <tbody>${body || '<tr><td colspan="5" class="empty">No executed code was sampled.</td></tr>'}</tbody></table>
  </div>`;
}

function profileInsnRow(r, prog, syntax) {
  const asm = syntax === 'att' ? r.att : r.intel;
  const share = prog > 0 ? (r.mcyc || 0) / prog : 0;
  const bg = share > 0.001 ? `style="background:rgba(224,108,117,${Math.min(0.6, share * 1.6).toFixed(3)})"` : '';
  const cnt = r.count ? `<span class="cnt" title="executions">${fmtBig(r.count)}×</span>` : '';
  const mc = r.mcyc ? `<span class="mc">${fmtBig(r.mcyc)}</span>` : '';
  const pc = share > 0.0005 ? `<span class="pc">${fmtPct(share)}</span>` : '';
  const dec = r.tag ? `<span class="tag">${esc(r.tag)}</span>${r.note ? ' ' + esc(r.note) : ''}` : '';
  const ln = r.line ? `<span class="ln" data-line="${r.line}">${r.line}</span>` : '';
  return `<tr data-line="${r.line || 0}" ${bg}>
    <td class="off">${(r.off || 0).toString(16).padStart(4, '0')}</td>
    <td class="cnt-c">${cnt}</td>
    <td class="asm">${esc(asm)}</td>
    <td class="mc-c">${mc}</td>
    <td class="pc-c">${pc}</td>
    <td class="src">${ln}</td>
    <td class="dec">${dec}</td></tr>`;
}

function functionSection(f, fused, i, syntax) {
  const prog = fused.progCyc || 1;
  const share = (f._mcyc || 0) / prog;
  const st = fused.stats.get(f.name);
  const insns = (f.insns || []).filter((r) => (r.count || 0) > 0 || (r.mcyc || 0) > 0);
  const shown = insns.length ? insns : (f.insns || []);
  const rows = shown.map((r) => profileInsnRow(r, prog, syntax)).join('');
  const loopChips = (f._loops || []).filter((l) => l.mcyc).sort((a, b) => b.mcyc - a.mcyc).map((l) =>
    `<span class="lchip" data-line="${l.headLine}">⟳ line ${l.headLine}: <b>${fmtBig(l.iters)}×</b> · ${fmtBig(l.mcyc)} cyc · ${esc(typeof l.bottleneck === 'number' ? RES[l.bottleneck] : l.bottleneck || '')}</span>`).join('');
  const wall = st ? ` · ${fmtNs(st.total_ns)} (${fmtNs(st.self_ns)} self)` : '';
  return `<section class="pfn" id="pfn-${i}">
    <h2><span class="ln" data-line="${f.line || 0}">${esc(f.name)}</span>
      <span class="meta">${fmtPct(share)} of cycles · ${fmtBig(f._mcyc)} Mcyc · bound on ${esc(RES[f._bound] || '?')}${wall}</span></h2>
    <div class="fnbar"><b style="width:${(share * 100).toFixed(1)}%"></b></div>
    ${loopChips ? `<div class="lchips">${loopChips}</div>` : ''}
    ${rows ? `<table class="plist"><thead><tr><th>addr</th><th>exec</th><th>asm (${syntax})</th><th>cyc</th><th>%</th><th>src</th><th>decision</th></tr></thead><tbody>${rows}</tbody></table>` : '<p class="cold">never executed</p>'}
  </section>`;
}

function renderProfileHtml(model, mprof, fused, filePath, syntax, scope) {
  const all = focusFunctions(model, filePath);
  const funcs = all.slice().sort((a, b) => (b._mcyc || 0) - (a._mcyc || 0));
  const prog = fused.progCyc || 1;
  const insights = buildInsights(model, fused, filePath);
  const tree = callTreeHtml(buildCallTree(mprof), fused);

  const summary = `<div class="sumband">
    <div class="sumcard"><span class="sv">${fmtBig(fused.progCyc)}</span><span class="sl">measured cycles</span><span class="ss">static cost × exec count</span></div>
    <div class="sumcard"><span class="sv">${fmtBig(fused.progRetired)}</span><span class="sl">instructions retired</span><span class="ss">dynamic instruction count</span></div>
    <div class="sumcard"><span class="sv">${fmtNs(fused.wallNs)}</span><span class="sl">wall time · ${esc(fused.rootName || 'root')}</span><span class="ss">measured runtime</span></div>
    <div class="sumcard accent"><span class="sv">${esc(RES[fused.bound] || '?')}</span><span class="sl">bound on</span><span class="ss">busiest issue port</span></div>
  </div>`;

  const sections = funcs.map((f, i) => functionSection(f, fused, i, syntax)).join('');

  return `<!DOCTYPE html><html><head><meta charset="utf-8"><style>
    :root { --mono: var(--vscode-editor-font-family, monospace); --hot: var(--vscode-charts-red, #e06c75); --core: var(--vscode-charts-blue, #61afef); --mem: var(--vscode-charts-purple, #b48ead); --fix: var(--vscode-charts-green, #4ec994); }
    body { font-family: var(--vscode-font-family); font-size: 13px; line-height: 1.5; color: var(--vscode-foreground); padding: 0 14px 60px; }
    .bar { position: sticky; top: 0; background: var(--vscode-editor-background); padding: 10px 0; margin-bottom: 4px; border-bottom: 1px solid var(--vscode-panel-border); z-index: 6; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    .bar strong { font-family: var(--mono); }
    .segmented { display: inline-flex; border: 1px solid var(--vscode-panel-border); border-radius: 6px; overflow: hidden; }
    .segmented button { border: none; border-radius: 0; background: transparent; color: var(--vscode-foreground); padding: 3px 11px; cursor: pointer; opacity: 0.6; font-size: 0.92em; }
    .segmented button + button { border-left: 1px solid var(--vscode-panel-border); }
    .segmented button.active { background: var(--vscode-button-background); color: var(--vscode-button-foreground); opacity: 1; }
    button.ghost { background: transparent; border: 1px solid var(--vscode-panel-border); border-radius: 6px; color: var(--vscode-foreground); opacity: 0.8; padding: 3px 11px; cursor: pointer; }
    .hint { margin-left: auto; opacity: 0.45; font-size: 0.84em; }
    /* summary band */
    .sumband { display: flex; gap: 10px; flex-wrap: wrap; margin: 12px 0; }
    .sumcard { flex: 1 1 140px; background: var(--vscode-editorWidget-background, rgba(127,127,127,0.05)); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 8px; padding: 10px 14px; display: flex; flex-direction: column; }
    .sumcard.accent { border-color: var(--hot); }
    .sumcard .sv { font-family: var(--mono); font-size: 1.7em; font-weight: 700; font-variant-numeric: tabular-nums; line-height: 1.1; }
    .sumcard.accent .sv { color: var(--hot); }
    .sumcard .sl { font-size: 0.86em; margin-top: 2px; }
    .sumcard .ss { font-size: 0.74em; opacity: 0.5; }
    /* actionable insights: flat color-coded list, no boxes */
    .insights { border: 1px solid var(--vscode-panel-border); border-radius: 8px; overflow: hidden; margin: 2px 0 10px; }
    .irow { display: flex; gap: 11px; align-items: baseline; padding: 8px 12px; border-bottom: 1px solid var(--vscode-panel-border); }
    .irow:last-child { border-bottom: none; }
    .irow:hover { background: var(--vscode-list-hoverBackground); }
    .itag { flex: 0 0 auto; align-self: flex-start; margin-top: 1px; font-size: 0.66em; font-weight: 700; letter-spacing: 0.08em; padding: 3px 0; width: 70px; text-align: center; border-radius: 4px; font-family: var(--mono); }
    .iwrap { display: flex; flex-direction: column; min-width: 0; }
    .ititle { font-weight: 600; cursor: pointer; }
    .idetail { opacity: 0.72; margin-top: 2px; line-height: 1.45; }
    .isub { opacity: 0.45; font-size: 0.85em; font-style: italic; margin-top: 3px; }
    .tone-mem .itag { background: rgba(180,142,173,0.2); color: var(--mem); }
    .tone-hot .itag, .tone-branch .itag { background: rgba(224,108,117,0.2); color: var(--hot); }
    .tone-core .itag { background: rgba(97,175,239,0.2); color: var(--core); }
    .tone-fix .itag { background: rgba(78,201,148,0.2); color: var(--fix); }
    .tone-fix .ititle { color: var(--fix); }
    .tone-spill .itag { background: rgba(229,181,103,0.2); color: var(--vscode-editorWarning-foreground, #cca700); }
    .grid2 { display: grid; grid-template-columns: minmax(260px, 1fr) minmax(300px, 1.2fr); gap: 14px; align-items: start; margin-bottom: 6px; }
    @media (max-width: 880px) { .grid2 { grid-template-columns: 1fr; } }
    h3 { font-size: 0.92em; text-transform: uppercase; letter-spacing: 0.06em; opacity: 0.55; margin: 14px 0 6px; }
    .estnote { margin-top: 9px; opacity: 0.5; font-size: 0.74em; font-style: italic; line-height: 1.4; }
    /* per-port utilization */
    .portpanel { background: var(--vscode-editorWidget-background, rgba(127,127,127,0.05)); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 8px; padding: 12px 14px; }
    .phead { display: flex; align-items: baseline; gap: 10px; flex-wrap: wrap; margin-bottom: 11px; }
    .pverdict { font-weight: 700; font-size: 1.05em; }
    .pverdict.core { color: var(--core); } .pverdict.mem { color: var(--mem); }
    .pboundq { opacity: 0.6; font-size: 0.84em; }
    .pboundq b { font-family: var(--mono); opacity: 0.95; }
    .pgrid { display: grid; gap: 6px; }
    .prow { display: grid; grid-template-columns: 3em 1fr 3.2em; align-items: center; gap: 11px; font-size: 0.86em; }
    .prow .pn { text-align: right; font-family: var(--mono); opacity: 0.7; }
    .prow .pbar { height: 11px; background: rgba(127,127,127,0.14); border-radius: 3px; overflow: hidden; }
    .prow .pbar b { display: block; height: 100%; border-radius: 3px; }
    .prow.core .pbar b { background: var(--core); } .prow.mem .pbar b { background: var(--mem); }
    .prow .pv { text-align: right; font-family: var(--mono); opacity: 0.7; font-variant-numeric: tabular-nums; }
    .prow.hot .pn, .prow.hot .pv { opacity: 1; font-weight: 700; }
    .prow.hot .pbar b { filter: brightness(1.2); box-shadow: 0 0 0 1px var(--vscode-foreground); }
    /* call tree */
    .ctree { background: var(--vscode-editorWidget-background, rgba(127,127,127,0.04)); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 8px; padding: 4px 0; max-height: 360px; overflow-y: auto; }
    .chead, .crow { display: grid; grid-template-columns: 84px 3.4em 1fr 4.4em 3.6em; align-items: center; gap: 8px; padding: 3px 12px 3px 8px; }
    .chead { opacity: 0.4; font-size: 0.78em; border-bottom: 1px solid var(--vscode-panel-border); padding-bottom: 5px; margin-bottom: 2px; }
    .crow { font-size: 0.86em; }
    .crow:hover { background: var(--vscode-list-hoverBackground); }
    .cbarwrap { grid-column: 1; }
    .cbar { display: block; height: 10px; background: rgba(127,127,127,0.14); border-radius: 3px; overflow: hidden; }
    .cbar b { display: block; height: 100%; background: var(--hot); border-radius: 3px; }
    .cpct { font-family: var(--mono); font-variant-numeric: tabular-nums; opacity: 0.85; text-align: right; }
    .cname { color: var(--vscode-textLink-foreground); cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .crec { opacity: 0.5; }
    .cself, .ccalls { font-family: var(--mono); opacity: 0.6; text-align: right; font-variant-numeric: tabular-nums; font-size: 0.92em; }
    .ctempty { opacity: 0.5; font-style: italic; padding: 12px; }
    /* hotspot grid */
    .hotspot { background: var(--vscode-editorWidget-background, rgba(127,127,127,0.05)); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 8px; padding: 8px 4px 4px; }
    .hghead { display: flex; align-items: center; gap: 10px; padding: 2px 10px 8px; }
    .hgtitle { font-weight: 600; }
    .hgtab { border: none; background: transparent; color: var(--vscode-foreground); padding: 3px 10px; cursor: pointer; opacity: 0.6; font-size: 0.9em; }
    .hgtab.active { background: var(--vscode-button-background); color: var(--vscode-button-foreground); opacity: 1; }
    table.hgtable { border-collapse: collapse; width: 100%; font-size: 0.9em; }
    .hgtable th { text-align: left; opacity: 0.45; font-weight: normal; font-size: 0.85em; padding: 0 10px 5px; border-bottom: 1px solid var(--vscode-panel-border); }
    .hgtable td { padding: 2px 10px; vertical-align: middle; white-space: nowrap; }
    .hgtable tr:hover { background: var(--vscode-list-hoverBackground); }
    .hg-pct { width: 34%; }
    .hgbar { display: inline-block; width: 64%; height: 9px; background: rgba(127,127,127,0.16); border-radius: 3px; overflow: hidden; vertical-align: middle; margin-right: 7px; }
    .hgbar b { display: block; height: 100%; background: var(--hot); border-radius: 3px; }
    .hgv { font-family: var(--mono); font-variant-numeric: tabular-nums; opacity: 0.8; }
    .hg-cyc, .hg-x { font-family: var(--mono); opacity: 0.7; font-variant-numeric: tabular-nums; }
    .hg-bn .bn { font-family: var(--mono); font-size: 0.85em; padding: 1px 6px; border-radius: 4px; background: rgba(224,108,117,0.18); color: var(--hot); }
    .hg-nm .ln { color: var(--vscode-textLink-foreground); cursor: pointer; }
    .empty, .cold { opacity: 0.5; font-style: italic; padding: 10px; }
    /* per-function listing -- off-screen functions skip layout/paint so large
       files stay responsive while scrolling. */
    section.pfn { content-visibility: auto; contain-intrinsic-size: auto 520px; }
    .pfn h2 { font-size: 1.06em; margin: 22px 0 4px; padding-top: 14px; border-top: 1px solid var(--vscode-panel-border); font-family: var(--mono); }
    .pfn h2 .ln { cursor: pointer; }
    .pfn h2 .meta { font-weight: normal; opacity: 0.5; font-size: 0.66em; font-family: var(--vscode-font-family); margin-left: 8px; }
    .fnbar { height: 4px; background: rgba(127,127,127,0.14); border-radius: 3px; overflow: hidden; margin: 0 0 8px; }
    .fnbar b { display: block; height: 100%; background: var(--hot); }
    .lchips { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 8px; }
    .lchip { font-size: 0.8em; padding: 2px 9px; border-radius: 999px; background: rgba(224,108,117,0.13); cursor: pointer; }
    .lchip b { font-variant-numeric: tabular-nums; }
    table.plist { border-collapse: collapse; width: 100%; font-family: var(--mono); margin-top: 6px; table-layout: auto; }
    .plist td, .plist th { text-align: left; padding: 1px 18px 1px 0; vertical-align: top; white-space: pre; }
    .plist th { opacity: 0.45; font-weight: normal; border-bottom: 1px solid var(--vscode-panel-border); font-family: var(--vscode-font-family); font-size: 0.85em; padding-bottom: 5px; }
    /* numeric columns (exec/cyc/%) are right-aligned data, so right-align their headers too */
    .plist th:nth-child(2), .plist th:nth-child(4), .plist th:nth-child(5) { text-align: right; }
    /* let the last column soak up all slack so the data columns pack tight and stay aligned */
    .plist th:last-child, .plist td.dec { width: 100%; padding-right: 0; }
    .plist td.off { opacity: 0.38; }
    .plist td.cnt-c { text-align: right; } .cnt { color: var(--vscode-charts-purple, #c678dd); }
    .plist td.asm { color: var(--vscode-foreground); opacity: 0.92; }
    .plist td.mc-c { text-align: right; opacity: 0.7; } .mc { font-variant-numeric: tabular-nums; }
    .plist td.pc-c { text-align: right; } .pc { color: var(--hot); font-variant-numeric: tabular-nums; }
    .plist td.src .ln { color: var(--vscode-textLink-foreground); cursor: pointer; }
    .plist td.dec { white-space: normal; opacity: 0.85; }
    .plist tr:hover { background: var(--vscode-list-hoverBackground); }
    .plist tr.hl { background: var(--vscode-editor-selectionBackground); }
    .tag { padding: 0 6px; border-radius: 4px; font-size: 0.82em; background: rgba(78,201,148,0.18); }
  </style></head><body>
    <div class="bar">
      <strong>${esc(path.basename(filePath))}</strong>
      <span class="segmented">
        <button id="intel" class="${syntax === 'att' ? '' : 'active'}">Intel</button>
        <button id="att" class="${syntax === 'att' ? 'active' : ''}">AT&amp;T</button>
      </span>
      <button id="refresh" class="ghost">⟳ Re-run</button>
      <span class="hint">measured profile · ${funcs.length} function${funcs.length === 1 ? '' : 's'} executed</span>
    </div>
    ${summary}
    <h3>What to do</h3>
    ${insightsHtml(insights)}
    <div class="grid2">
      <div><h3>Issue-port utilization</h3>${portPanel(fused.port)}</div>
      <div><h3>Call graph · top-down</h3>${tree}</div>
    </div>
    ${hotspotGrid(funcs, fused, filePath, scope)}
    <h3>Annotated hot code</h3>
    ${sections || '<p class="cold">No functions were executed.</p>'}
    <script>
      const vscodeApi = acquireVsCodeApi();
      function jump(line){ if(Number(line)>0) vscodeApi.postMessage({type:'jump', line:Number(line)}); }
      document.body.addEventListener('click', (e) => {
        const t = e.target.closest('[data-line]') || e.target;
        if (t.dataset && t.dataset.line && Number(t.dataset.line) > 0) { jump(t.dataset.line); return; }
        if (e.target.dataset && e.target.dataset.scope) { vscodeApi.postMessage({type:'scope', scope:e.target.dataset.scope}); return; }
        if (e.target.id === 'intel') vscodeApi.postMessage({type:'syntax', syntax:'intel'});
        if (e.target.id === 'att') vscodeApi.postMessage({type:'syntax', syntax:'att'});
        if (e.target.id === 'refresh') vscodeApi.postMessage({type:'refresh'});
      });
      window.addEventListener('message', (ev) => {
        const m = ev.data;
        if (m.type === 'highlight') {
          document.querySelectorAll('tr.hl').forEach((r) => r.classList.remove('hl'));
          let first = null;
          document.querySelectorAll('.plist tr[data-line="' + m.line + '"]').forEach((r) => { r.classList.add('hl'); if (!first) first = r; });
          if (first) first.scrollIntoView({ block: 'center', behavior: 'smooth' });
        }
      });
    </script>
  </body></html>`;
}

// ---- panel + source heat decorations --------------------------------------
let panel = null;
let model = null;
let mprofData = null;
let fused = null;
let sourcePath = null;
let syntax = 'intel';
let scope = 'function';
const heatDecos = [];

function ensureHeat() {
  if (heatDecos.length) return;
  for (let i = 0; i < 6; i++) {
    const a = (0.06 + 0.11 * i).toFixed(3);
    heatDecos.push(vscode.window.createTextEditorDecorationType({
      isWholeLine: true,
      backgroundColor: `rgba(224,108,117,${a})`,
      overviewRulerColor: `rgba(224,108,117,${(0.3 + 0.1 * i).toFixed(2)})`,
      overviewRulerLane: vscode.OverviewRulerLane.Full,
      after: { margin: '0 0 0 2.5em', color: new vscode.ThemeColor('editorGhostText.foreground') },
    }));
  }
}

function applyHeat() {
  ensureHeat();
  const buckets = heatDecos.map(() => []);
  if (fused && model && sourcePath) {
    const prog = fused.progCyc || 1;
    const byLine = new Map();
    for (const f of focusFunctions(model, sourcePath)) {
      for (const ln of f._lines || []) {
        const cur = byLine.get(ln.line) || { cyc: 0, count: 0 };
        cur.cyc += ln.cyc; if (ln.count > cur.count) cur.count = ln.count;
        byLine.set(ln.line, cur);
      }
    }
    let maxShare = 0;
    for (const v of byLine.values()) maxShare = Math.max(maxShare, v.cyc / prog);
    for (const [line, v] of byLine) {
      const share = v.cyc / prog;
      if (share < 0.001) continue;
      const b = Math.min(5, Math.floor((maxShare > 0 ? share / maxShare : 0) * 5.999));
      const ln = Math.max(0, line - 1);
      buckets[b].push({
        range: new vscode.Range(ln, 0, ln, 0),
        renderOptions: { after: { contentText: `   ${fmtPct(share)} · ${fmtBig(v.count)}×` } },
        hoverMessage: new vscode.MarkdownString(`**profile:** ${fmtPct(share)} of measured cycles · ${fmtBig(v.count)} executions`),
      });
    }
  }
  for (const editor of vscode.window.visibleTextEditors) {
    const isTarget = sourcePath && path.normalize(editor.document.uri.fsPath).toLowerCase() === path.normalize(sourcePath).toLowerCase();
    heatDecos.forEach((d, i) => editor.setDecorations(d, isTarget ? buckets[i] : []));
  }
}

async function refresh(deps, filePath) {
  const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(filePath));
  const workspaceRoot = folder ? folder.uri.fsPath : path.dirname(filePath);
  const result = await runProfile(deps, filePath, workspaceRoot);
  if (!result.ok) {
    if (panel) panel.webview.html = `<body style="padding:20px;font-family:var(--vscode-font-family);color:var(--vscode-foreground)"><p>Profile failed:</p><pre>${esc(result.error)}</pre></body>`;
    return;
  }
  model = result.json;
  mprofData = result.mprof;
  fused = fuseProfile(model, mprofData);
  sourcePath = filePath;
  if (panel) panel.webview.html = renderProfileHtml(model, mprofData, fused, filePath, syntax, scope);
  applyHeat();
}

function loadingHtml(name) {
  return `<!DOCTYPE html><html><body style="font-family:var(--vscode-font-family);color:var(--vscode-foreground);padding:20px">
    <p>Building and running <strong>${esc(name)}</strong> with <code>--profile-blocks</code> to measure execution...</p>
    <p style="opacity:.55;font-size:.9em">The instrumented binary runs to completion, then its per-block counts are fused with the static cost model.</p></body></html>`;
}

function rerender() {
  if (panel && model && fused && sourcePath) panel.webview.html = renderProfileHtml(model, mprofData, fused, sourcePath, syntax, scope);
}

function syncHighlight(editor) {
  if (!panel || !editor || !sourcePath) return;
  if (path.normalize(editor.document.uri.fsPath).toLowerCase() !== path.normalize(sourcePath).toLowerCase()) return;
  panel.webview.postMessage({ type: 'highlight', line: editor.selection.active.line + 1 });
}

function registerProfile(context, deps) {
  const showProfile = async () => {
    const editor = vscode.window.activeTextEditor;
    const doc = editor?.document;
    if (!doc || doc.languageId !== 'mettle') {
      vscode.window.showInformationMessage('Open a Mettle file to profile its codegen.');
      return;
    }
    if (doc.isDirty) await doc.save();
    const filePath = doc.uri.fsPath;

    if (!panel) {
      panel = vscode.window.createWebviewPanel(
        'mettleProfile', 'Mettle: Codegen Profile',
        { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true },
        { enableScripts: true, retainContextWhenHidden: true }
      );
      panel.onDidDispose(() => { panel = null; model = null; fused = null; sourcePath = null; applyHeat(); }, null, context.subscriptions);
      panel.webview.onDidReceiveMessage(async (msg) => {
        try {
          if (msg.type === 'jump' && sourcePath) {
            const opened = await vscode.window.showTextDocument(vscode.Uri.file(sourcePath), { viewColumn: vscode.ViewColumn.One });
            const ln = Math.max(0, Math.min((msg.line || 1) - 1, opened.document.lineCount - 1));
            const pos = new vscode.Position(ln, 0);
            opened.selection = new vscode.Selection(pos, pos);
            opened.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
          } else if (msg.type === 'syntax') { syntax = msg.syntax === 'att' ? 'att' : 'intel'; rerender(); }
          else if (msg.type === 'scope') { scope = ['function', 'loop', 'line'].includes(msg.scope) ? msg.scope : 'function'; rerender(); }
          else if (msg.type === 'refresh' && sourcePath) { await refresh(deps, sourcePath); }
        } catch (err) {
          vscode.window.showErrorMessage(`Mettle profile action failed: ${err && err.message ? err.message : err}`);
        }
      }, null, context.subscriptions);
    } else {
      panel.reveal(undefined, true);
    }
    panel.webview.html = loadingHtml(path.basename(filePath));
    await refresh(deps, filePath);
  };

  context.subscriptions.push(
    vscode.window.onDidChangeTextEditorSelection((e) => syncHighlight(e.textEditor)),
    vscode.window.onDidChangeVisibleTextEditors(() => applyHeat())
  );

  return { showProfile };
}

module.exports = { runProfile, fuseProfile, renderProfileHtml, buildCallTree, buildInsights, focusFunctions, waterfillInto, registerProfile };
