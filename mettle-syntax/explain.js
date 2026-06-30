/**
 * Mettle Optimization Report: panel, one-click verified fixes, inline hints,
 * quick fixes, status bar, and .explain.txt sidecar support.
 *
 * The compiler's --explain report doesn't guess: fix suggestions marked
 * `verified:` were applied to an internal clone and re-accepted by the
 * optimizer itself. This module closes the last gap: those proven fixes
 * become one-click source edits:
 *
 *   panel card "Apply fix"  ─┐
 *   editor lightbulb         ├─> WorkspaceEdit -> save -> recompile -> the
 *   "Apply all" banner      ─┘   loop's card and inline hint flip green.
 *
 * Everything renders from one parsed model: webview panel (dashboard bars,
 * function-grouped cards, filters), editor decorations (per-loop outcome at
 * end of line), code actions, and the status bar summary.
 */

const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { execFile } = require('child_process');

/** @type {vscode.WebviewPanel | null} */
let reportPanel = null;
/** Absolute path of the source file the current panel describes. */
let reportSourcePath = null;
/** Parsed report for the current panel (drives every surface). */
let reportModel = null;
let decorationsEnabled = true;
/** @type {vscode.StatusBarItem | null} */
let statusItem = null;
/** Single-flight refresh: a request during a running compile is coalesced
 * into one follow-up run instead of stacking compiles (rapid saves used to
 * queue several multi-second compiles and read as a hang). */
let refreshInFlight = false;
let refreshQueued = false;
/** Set while showReport saves the document itself, so the save handler does
 * not start a second compile for the same action. */
let suppressSaveRefresh = false;

const vectorizedDecoration = vscode.window.createTextEditorDecorationType({
  isWholeLine: true,
  after: { margin: '0 0 0 2em', color: new vscode.ThemeColor('editorGhostText.foreground') },
  overviewRulerColor: 'rgba(78, 201, 148, 0.7)',
  overviewRulerLane: vscode.OverviewRulerLane.Right,
});
const scalarDecoration = vscode.window.createTextEditorDecorationType({
  isWholeLine: true,
  after: { margin: '0 0 0 2em', color: new vscode.ThemeColor('editorGhostText.foreground') },
  overviewRulerColor: 'rgba(224, 108, 117, 0.55)',
  overviewRulerLane: vscode.OverviewRulerLane.Right,
});
const fixableDecoration = vscode.window.createTextEditorDecorationType({
  isWholeLine: true,
  after: { margin: '0 0 0 2em', color: new vscode.ThemeColor('editorGhostText.foreground') },
  overviewRulerColor: 'rgba(215, 186, 125, 0.85)',
  overviewRulerLane: vscode.OverviewRulerLane.Right,
});

// --- report acquisition ---

function runExplain(deps, filePath, workspaceRoot) {
  const cfg = vscode.workspace.getConfiguration('mettle');
  const compiler = deps.findCompiler(workspaceRoot, filePath);
  const timeoutMs = Math.max(2000, Number(cfg.get('explain.timeoutMs', 30000)) || 30000);

  // STABLE per-source output dir (not a fresh mkdtemp): the compiler keeps a
  // `.explain.base` baseline next to the output and reports what CHANGED
  // since the previous compile -- that only works if the path survives
  // between refreshes.
  const stem = path.basename(filePath, '.mettle').replace(/[^A-Za-z0-9_-]/g, '_');
  const hash = require('crypto').createHash('sha1').update(filePath.toLowerCase()).digest('hex').slice(0, 10);
  let tempDir;
  try {
    tempDir = path.join(os.tmpdir(), 'mettle-explain', `${stem}-${hash}`);
    fs.mkdirSync(tempDir, { recursive: true });
  } catch (err) {
    return Promise.resolve({ ok: false, error: `temp dir: ${err.message}`, output: '' });
  }
  const tempOut = path.join(tempDir, 'explain.obj');
  const jsonPath = path.join(tempDir, 'explain.explain.json');

  const args = [
    '-i', filePath,
    '-o', tempOut,
    '--release',
    '--explain',
    '--explain-json',
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
      maxBuffer: 16 * 1024 * 1024,
      cwd: workspaceRoot,
      env: { ...process.env, METTLE_EXPLAIN_REPORT_LINES: '0', NO_COLOR: '1' },
    }, (err, stdout, stderr) => {
      const output = (stderr || '') + (stdout || '');
      if (err && /** @type {NodeJS.ErrnoException} */ (err).code === 'ENOENT') {
        resolve({ ok: false, error: `Compiler not found: ${compiler}. Set mettle.linter.compilerPath.`, output });
        return;
      }
      if (!/-- optimization report:/.test(output)) {
        resolve({ ok: false, error: err ? 'Compilation failed before the report.' : 'No report in compiler output.', output });
        return;
      }
      // The structured sidecar is the primary parse source; prose is the
      // fallback (and stays the format of saved .explain.txt files).
      let json = null;
      try {
        json = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
      } catch (_) { /* fall back to prose */ }
      resolve({ ok: true, error: null, output, json });
    });
  });
}

// --- report parsing ---

function parseReport(raw) {
  const text = raw.replace(/\x1b\[[0-9;]*m/g, '');
  const lines = text.split(/\r?\n/);
  const model = {
    sourceName: null,
    remarks: [],
    memory: [],
    backend: { summary: [], groups: [] },
    stats: {
      vectorized: 0, scalar: 0, verified: 0,
      inlined: 0, refused: 0, unrolled: 0,
      backendOk: null, backendTotal: null, weightedPct: null,
    },
  };

  let section = null;
  let current = null;
  let currentGroup = null;

  for (const line of lines) {
    const header = line.match(/^-- (optimization|backend|memory) report: (\S+)/);
    if (header) {
      section = header[1];
      model.sourceName = model.sourceName || header[2];
      current = null;
      currentGroup = null;
      continue;
    }
    if (section === 'optimization') {
      const remark = line.match(/^  (\S+) \((.+?)\): (.+)$/);
      if (remark) {
        const entityText = remark[2];
        const r = {
          fn: remark[1],
          entity: entityText,
          kind: 'other',
          callee: null,
          line: null,
          lineEnd: null,
          count: 1,
          headline: remark[3],
          positive: !/^NOT /.test(remark[3]),
          reason: null, fix: null, verified: null, calls: null,
        };
        let m;
        if ((m = entityText.match(/^loop @ line (\d+)$/))) {
          r.kind = 'loop';
          r.line = parseInt(m[1], 10);
        } else if ((m = entityText.match(/^call to `(.+)` @ line (\d+)$/))) {
          r.kind = 'call';
          r.callee = m[1];
          r.line = parseInt(m[2], 10);
        } else if ((m = entityText.match(/^function @ line (\d+)$/))) {
          r.kind = 'function';
          r.line = parseInt(m[1], 10);
        } else if ((m = entityText.match(/^(\d+) calls, lines (\d+)-(\d+)$/))) {
          r.kind = 'calls-folded';
          r.count = parseInt(m[1], 10);
          r.line = parseInt(m[2], 10);
          r.lineEnd = parseInt(m[3], 10);
        }
        model.remarks.push(r);
        current = r;

        if (r.kind === 'loop') {
          if (/^vectorized/.test(r.headline)) model.stats.vectorized++;
          else if (/^NOT vectorized/.test(r.headline)) model.stats.scalar++;
          else if (/unrolled|eliminated/.test(r.headline)) model.stats.unrolled++;
        } else if (r.kind === 'call') {
          if (r.headline === 'inlined') model.stats.inlined++;
          else if (/^NOT inlined/.test(r.headline)) model.stats.refused++;
        } else if (r.kind === 'calls-folded' && /^NOT inlined/.test(r.headline)) {
          model.stats.refused += r.count;
        }
        continue;
      }
      const sub = line.match(/^      \\_ (reason|fix|verified|calls|consequence): (.+)$/);
      if (sub && current) {
        current[sub[1] === 'consequence' ? 'reason' : sub[1]] = sub[2];
        if (sub[1] === 'verified') model.stats.verified++;
        continue;
      }
      continue;
    }
    if (section === 'backend') {
      let m;
      if ((m = line.match(/^  (\d+)\/(\d+) functions reaching codegen/))) {
        model.stats.backendOk = parseInt(m[1], 10);
        model.stats.backendTotal = parseInt(m[2], 10);
        model.backend.summary.push(line.trim());
        continue;
      }
      if ((m = line.match(/^  ([\d.]+)% of the program's ([\d,]+) optimized IR instructions/))) {
        model.stats.weightedPct = parseFloat(m[1]);
        model.backend.summary.push(line.trim());
        continue;
      }
      if ((m = line.match(/^  (.+) \((\d+) functions?, (\d+) instructions\):$/))) {
        currentGroup = {
          reason: m[1],
          functions: parseInt(m[2], 10),
          instructions: parseInt(m[3], 10),
          consequence: null,
          fix: null,
          members: null,
        };
        model.backend.groups.push(currentGroup);
        continue;
      }
      const sub = line.match(/^      \\_ (consequence|fix): (.+)$/);
      if (sub && currentGroup) {
        currentGroup[sub[1]] = sub[2];
        continue;
      }
      const members = line.match(/^      \\_ (?!consequence:|fix:)(.+)$/);
      if (members && currentGroup) {
        currentGroup.members = (currentGroup.members ? currentGroup.members + ' ' : '') + members[1];
        continue;
      }
      if (/^  \d+ functions? use baseline/.test(line)) {
        model.backend.summary.push(line.trim());
      }
      continue;
    }
    if (section === 'memory') {
      const diag = line.match(/^  (warning|error) \(line (\d+)\): (.+)$/);
      if (diag) {
        current = {
          severity: diag[1],
          line: parseInt(diag[2], 10),
          headline: diag[3],
          fix: null,
        };
        model.memory.push(current);
        continue;
      }
      const sub = line.match(/^      \\_ fix: (.+)$/);
      if (sub && current && current.severity) {
        current.fix = sub[1];
        continue;
      }
      continue;
    }
  }
  return model;
}

/**
 * Build the panel model from the compiler's --explain-json sidecar (the
 * structured twin of parseReport; no prose regexes involved).
 */
function modelFromJson(json) {
  const model = {
    sourceName: json.source || null,
    remarks: [],
    memory: [],
    backend: { summary: [], groups: [] },
    changes: null,
    stats: {
      vectorized: json.stats ? json.stats.loopsVectorized : 0,
      scalar: json.stats ? json.stats.loopsScalar : 0,
      verified: json.stats ? json.stats.fixesVerified : 0,
      inlined: json.stats ? json.stats.callsInlined : 0,
      refused: json.stats ? json.stats.callsRefused : 0,
      unrolled: 0,
      backendOk: null, backendTotal: null, weightedPct: null,
    },
  };
  for (const r of json.remarks || []) {
    model.remarks.push({
      fn: r.fn,
      entity: r.entity || r.kind,
      kind: r.kind,
      callee: r.callee || null,
      line: typeof r.line === 'number' && r.line > 0 ? r.line : null,
      lineEnd: r.lineEnd || null,
      count: r.count || 1,
      headline: r.headline,
      positive: !!r.positive,
      reason: r.reason || null,
      fix: r.fix || null,
      verified: r.verified || null,
      calls: r.calls || null,
      depth: r.depth || 0,
    });
    if (r.kind === 'loop' && /unrolled|eliminated/.test(r.headline || '')) {
      model.stats.unrolled++;
    }
  }
  for (const m of json.memory || []) {
    model.memory.push({
      severity: m.severity === 'error' ? 'error' : 'warning',
      line: typeof m.line === 'number' && m.line > 0 ? m.line : null,
      headline: m.headline || '',
      fix: m.fix || null,
    });
  }
  if (json.backend) {
    model.stats.backendOk = json.backend.ok;
    model.stats.backendTotal = json.backend.total;
    if (json.backend.instructions > 0) {
      model.stats.weightedPct = 100 * json.backend.okInstructions / json.backend.instructions;
      model.backend.summary.push(
        `${json.backend.ok}/${json.backend.total} functions reaching codegen (after inlining) compiled with the register-allocating backend`,
        `${model.stats.weightedPct.toFixed(1)}% of the program's ${json.backend.instructions} optimized IR instructions are in register-allocated code`);
    }
    for (const g of json.backend.groups || []) {
      model.backend.groups.push({
        reason: g.reason,
        functions: g.functions,
        instructions: g.instructions,
        consequence: g.consequence || null,
        fix: g.fix || null,
        members: (g.members || []).map((m) => `${m.fn} (${m.instructions})`).join(', ') || null,
      });
    }
  }
  if (json.changes && json.changes.baseline) {
    model.changes = {
      hadBaseline: true,
      entries: json.changes.entries || [],
      improved: json.stats ? json.stats.changesImproved : 0,
      regressed: json.stats ? json.stats.changesRegressed : 0,
    };
  }
  return model;
}

// --- fix synthesis ----------------------------------------------------------
// Turn the compiler's fix text into a concrete source edit. Only fixes whose
// shape is mechanical are synthesized; everything else keeps jump+explain.
// A remark with `verified:` AND a synthesized edit is the gold path: the
// optimizer already re-checked the change on a clone, so applying it is safe
// by construction.

/** Find the 0-based line of `fn <name>(` at or above `belowLine`. */
function findFunctionStart(lines, fnName, belowLine) {
  const re = new RegExp(`^\\s*(?:export\\s+)?(?:@\\w+!?\\s+)*fn\\s+${escapeRe(fnName)}\\s*[(<]`);
  for (let i = Math.min(belowLine, lines.length - 1); i >= 0; i--) {
    if (re.test(lines[i])) return i;
  }
  return -1;
}

/** Find `fn <name>(` anywhere in the file (for callee-side edits). */
function findFunctionAnywhere(lines, fnName) {
  const re = new RegExp(`^(\\s*)((?:export\\s+)?(?:@\\w+!?\\s+)*)fn\\s+${escapeRe(fnName)}\\s*[(<]`);
  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(re);
    if (m) return { line: i, indent: m[1], decorators: m[2] };
  }
  return null;
}

function escapeRe(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * @returns {{ title: string, edits: {line: number, start: number, end: number, newText: string}[] } | null}
 * Lines/columns are 0-based; edits never span lines.
 */
function synthesizeFix(remark, lines) {
  if (!remark.fix) return null;

  // "declare the accumulator `s` as int64" (int32 sum) and
  // "declare the accumulator as int64 (sum bytes as `total = total + (int64)data[i]`)" (byte sum)
  let m = remark.fix.match(/declare the accumulator `(\w+)` as int64/) ||
    remark.fix.match(/declare the accumulator as int64 \(sum bytes as `(\w+) =/);
  if (m && remark.line !== null) {
    const acc = m[1];
    const isByteSum = /sum bytes/.test(remark.fix);
    const fnStart = findFunctionStart(lines, remark.fn, remark.line - 1);
    const declRe = new RegExp(`^(\\s*var\\s+${escapeRe(acc)}\\s*:\\s*)(u?int(?:8|16|32))\\b`);
    for (let i = remark.line - 1; i >= Math.max(0, fnStart); i--) {
      const dm = lines[i].match(declRe);
      if (!dm) continue;
      const edits = [{
        line: i,
        start: dm[1].length,
        end: dm[1].length + dm[2].length,
        newText: 'int64',
      }];
      if (isByteSum) {
        // The byte-sum kernel requires the cast form: retarget an existing
        // `(intNN)` cast in the accumulation, or insert `(int64)` after `+`.
        const accumRe = new RegExp(`${escapeRe(acc)}\\s*=\\s*${escapeRe(acc)}\\s*\\+\\s*`);
        for (let j = remark.line - 1; j < Math.min(lines.length, remark.line + 24); j++) {
          const am = lines[j].match(accumRe);
          if (!am) continue;
          const after = am.index + am[0].length;
          const castM = lines[j].slice(after).match(/^\((?:u?int(?:8|16|32))\)/);
          if (castM) {
            edits.push({ line: j, start: after, end: after + castM[0].length, newText: '(int64)' });
          } else {
            edits.push({ line: j, start: after, end: after, newText: '(int64)' });
          }
          break;
        }
      }
      return { title: `Declare accumulator \`${acc}\` as int64`, edits };
    }
    return null;
  }

  // "remove `@noinline` from `damp` (it blocks this loop's vectorization)"
  m = remark.fix.match(/remove `@noinline` from `(\w+)`/);
  if (m) {
    const target = findFunctionAnywhere(lines, m[1]);
    if (!target) return null;
    const line = lines[target.line];
    const dm = line.match(/@noinline\s*/);
    if (!dm) return null;
    return {
      title: `Remove @noinline from \`${m[1]}\``,
      edits: [{ line: target.line, start: dm.index, end: dm.index + dm[0].length, newText: '' }],
    };
  }

  // "mark the callee @inline to override the caller budget" /
  // "make `X` inline-eligible (small body, or mark it @inline), ..."
  m = remark.fix.match(/make `(\w+)` inline-eligible.*mark it @inline/) ||
    (/mark the callee @inline/.test(remark.fix) && remark.callee
      ? [null, remark.callee] : null);
  if (m) {
    const target = findFunctionAnywhere(lines, m[1]);
    if (!target || /@inline\b/.test(target.decorators) || /@noinline\b/.test(target.decorators)) {
      return null;
    }
    const insertAt = target.indent.length + (target.decorators ? target.decorators.length : 0);
    return {
      title: `Mark \`${m[1]}\` @inline`,
      edits: [{ line: target.line, start: insertAt, end: insertAt, newText: '@inline ' }],
    };
  }

  return null;
}

/** Attach synthesized fixes to the model against the CURRENT source text. */
function annotateApplicableFixes(model, sourcePath) {
  let lines;
  try {
    lines = fs.readFileSync(sourcePath, 'utf8').split(/\r?\n/);
  } catch (_) {
    return;
  }
  for (const r of model.remarks) {
    r.applicable = null;
    if (r.positive || !r.fix) continue;
    const synthesized = synthesizeFix(r, lines);
    if (synthesized) r.applicable = synthesized;
  }
}

/**
 * Apply fixes and ALWAYS report the outcome -- a click that silently does
 * nothing is worse than an error. Returns per-remark results for messaging.
 */
async function applyFixes(remarks, sourcePath, log) {
  const uri = vscode.Uri.file(sourcePath);
  const doc = await vscode.workspace.openTextDocument(uri);
  const lines = doc.getText().split(/\r?\n/);

  // Re-synthesize against the live buffer, then apply bottom-up so earlier
  // edits cannot shift later coordinates.
  const allEdits = [];
  const titles = [];
  const skipped = [];
  for (const r of remarks) {
    const synthesized = synthesizeFix(r, lines);
    if (!synthesized) {
      skipped.push(`${r.fn} (${r.entity}): the source no longer matches this report`);
      if (log) log(`[explain] could not re-synthesize fix for ${r.fn} (${r.entity}); fix text: ${r.fix}`);
      continue;
    }
    titles.push(synthesized.title);
    allEdits.push(...synthesized.edits);
    if (log) log(`[explain] applying: ${synthesized.title} (${synthesized.edits.length} edit(s))`);
  }
  if (allEdits.length === 0) return { applied: 0, titles, skipped, editFailed: false };

  allEdits.sort((a, b) => (b.line - a.line) || (b.start - a.start));
  const edit = new vscode.WorkspaceEdit();
  const seen = new Set();
  for (const e of allEdits) {
    const key = `${e.line}:${e.start}:${e.end}`;
    if (seen.has(key)) continue;
    seen.add(key);
    edit.replace(uri, new vscode.Range(e.line, e.start, e.line, e.end), e.newText);
  }
  const ok = await vscode.workspace.applyEdit(edit);
  if (!ok && log) log('[explain] workspace.applyEdit returned false');
  if (ok) await doc.save(); // save triggers the panel's recompile
  return { applied: ok ? titles.length : 0, titles, skipped, editFailed: !ok };
}

/** Shared outcome messaging for single-fix and apply-all paths. */
function reportApplyOutcome(result) {
  if (result.applied > 0) {
    const detail = result.applied === 1 ? `"${result.titles[0]}"` : `${result.applied} fixes`;
    vscode.window.setStatusBarMessage(`Mettle: applied ${detail}; recompiling`, 4000);
    if (result.skipped.length > 0) {
      vscode.window.showWarningMessage(
        `Mettle: ${result.skipped.length} fix(es) skipped: ${result.skipped[0]}`);
    }
    return;
  }
  if (result.editFailed) {
    vscode.window.showErrorMessage(
      'Mettle: the editor rejected the fix edits (workspace.applyEdit failed). The file may be read-only.');
    return;
  }
  vscode.window.showWarningMessage(
    'Mettle: no fixes could be applied -- the source has changed since this report was produced. Refresh and retry.'
  );
}

// --- editor decorations (inline hints) ---

function truncate(s, n) {
  if (!s) return '';
  return s.length > n ? s.slice(0, n - 3) + '...' : s;
}

function applyDecorations() {
  for (const editor of vscode.window.visibleTextEditors) {
    if (!reportModel || !reportSourcePath || !decorationsEnabled ||
      path.normalize(editor.document.uri.fsPath).toLowerCase() !==
      path.normalize(reportSourcePath).toLowerCase()) {
      editor.setDecorations(vectorizedDecoration, []);
      editor.setDecorations(scalarDecoration, []);
      editor.setDecorations(fixableDecoration, []);
      continue;
    }
    const good = [];
    const bad = [];
    const fixable = [];
    for (const r of reportModel.remarks) {
      if (r.kind !== 'loop' || r.line === null) continue;
      const line = Math.min(Math.max(0, r.line - 1), editor.document.lineCount - 1);
      const range = editor.document.lineAt(line).range;
      if (r.positive) {
        good.push({
          range,
          renderOptions: { after: { contentText: truncate(r.headline, 72) } },
          hoverMessage: hoverFor(r),
        });
      } else if (r.applicable && r.verified) {
        fixable.push({
          range,
          renderOptions: { after: { contentText: 'fix available: ' + truncate(r.applicable.title, 56) } },
          hoverMessage: hoverFor(r),
        });
      } else {
        bad.push({
          range,
          renderOptions: { after: { contentText: 'not vectorized: ' + truncate(r.reason || r.headline, 64) } },
          hoverMessage: hoverFor(r),
        });
      }
    }
    editor.setDecorations(vectorizedDecoration, good);
    editor.setDecorations(scalarDecoration, bad);
    editor.setDecorations(fixableDecoration, fixable);
  }
}

function hoverFor(remark) {
  const md = new vscode.MarkdownString(undefined, true);
  md.isTrusted = false;
  md.appendMarkdown(`**Mettle optimizer** -- ${escapeMd(remark.headline)}\n\n`);
  if (remark.reason) md.appendMarkdown(`**reason:** ${escapeMd(remark.reason)}\n\n`);
  if (remark.fix) md.appendMarkdown(`**fix:** ${escapeMd(remark.fix)}\n\n`);
  if (remark.verified) md.appendMarkdown(`**verified:** ${escapeMd(remark.verified)}\n\n`);
  if (remark.applicable && remark.verified) md.appendMarkdown(`Quick Fix available: ${escapeMd(remark.applicable.title)}\n`);
  return md;
}

function escapeMd(value) {
  return String(value).replace(/[\\`*_{}[\]()#+\-.!|>]/g, '\\$&');
}

// --- status bar ---

function updateStatusBar() {
  if (!statusItem) return;
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== 'mettle') {
    statusItem.hide();
    return;
  }
  const isCurrent = reportModel && reportSourcePath &&
    path.normalize(editor.document.uri.fsPath).toLowerCase() ===
    path.normalize(reportSourcePath).toLowerCase();
  if (!isCurrent) {
    statusItem.text = '$(graph) Mettle report';
    statusItem.tooltip = 'Show the optimization report for this file';
    statusItem.show();
    return;
  }
  const s = reportModel.stats;
  const fixableCount = reportModel.remarks.filter((r) => r.applicable && r.verified).length;
  const loopTotal = s.vectorized + s.scalar;
  const parts = [`$(graph) ${s.vectorized}/${loopTotal} vec`];
  if (fixableCount > 0) parts.push(`${fixableCount} fix${fixableCount === 1 ? '' : 'es'}`);
  statusItem.text = parts.join(' · ');
  statusItem.tooltip = `Mettle: ${s.vectorized} of ${loopTotal} loops vectorized` +
    (fixableCount > 0 ? `; ${fixableCount} verified fix${fixableCount === 1 ? '' : 'es'} available` : '') +
    '. Click to open the optimization report.';
  statusItem.show();
}

// --- webview panel ---

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

/**
 * The Mettle mark (icons/mettle-*.svg) as inline SVG. The five pieces carry
 * classes p0..p4 so the busy state can pulse them in sequence; fill is
 * currentColor so it follows the editor theme.
 */
function logoSvg(size) {
  return `<svg class="logo" width="${size}" height="${size}" viewBox="0 0 150 150" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
  <polygon class="p p0" fill="currentColor" points="56.4 63.7 69.5 34.8 113.7 32.3 133.5 51 122.3 74.9 130.2 88.3 147.6 48 119.6 20.8 62.4 20.9 42.4 63.7"/>
  <polygon class="p p1" fill="currentColor" points="60.6 63.7 68.9 57.7 95.6 57.7 108.9 63.6"/>
  <polygon class="p p2" fill="currentColor" points="47.9 67 109.2 88.8 114.3 97.6 138 109.5 114.2 66.9"/>
  <path class="p p3" fill="currentColor" d="m140.4 114.6-94.1-45.1-2.9 6 43.7 41.8-23.6 0.1-20.8-40.3-24.2 51.9 129.7-0.1-7.8-14.3z"/>
  <path class="p p4" fill="currentColor" d="m14.2 63-12.1 5.5 1.4 2.1 23.1 32.4 13.9-29.1-26.3-10.9zm14.7 22.3-17.3-19.9 24.4 10.3-7.1 9.6z"/>
</svg>`;
}

/** Shared logo animation rules: a quiet staggered pulse while busy, a single
 * short settle when a fresh result lands. Disabled entirely under
 * prefers-reduced-motion. */
const LOGO_CSS = `
  .logo { color: var(--vscode-foreground); display: block; }
  .logo .p { opacity: 0.92; }
  .busy .logo .p { animation: mettle-pulse 1.5s ease-in-out infinite; }
  .busy .logo .p1 { animation-delay: 0.12s; }
  .busy .logo .p2 { animation-delay: 0.24s; }
  .busy .logo .p3 { animation-delay: 0.36s; }
  .busy .logo .p4 { animation-delay: 0.48s; }
  .settle .logo .p { animation: mettle-settle 0.45s ease-out 1; }
  @keyframes mettle-pulse { 0%, 100% { opacity: 0.25; } 40% { opacity: 0.92; } }
  @keyframes mettle-settle { from { opacity: 0.35; } to { opacity: 0.92; } }
  @media (prefers-reduced-motion: reduce) {
    .busy .logo .p, .settle .logo .p { animation: none; }
    .busy .logo .p { opacity: 0.45; }
  }`;

/** The interstitial shown while the compiler runs from a cold open. */
function loadingHtml(fileLabel) {
  return `<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline';">
<style>
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); display: flex;
         flex-direction: column; align-items: center; justify-content: center; height: 90vh; gap: 14px; }
  .hint { opacity: 0.6; font-size: 0.9em; }
  .hint code { font-family: var(--vscode-editor-font-family); }
  ${LOGO_CSS}
</style></head>
<body class="busy">
  ${logoSvg(64)}
  <div class="hint">Compiling <code>${escapeHtml(fileLabel)}</code> with <code>--release --explain</code></div>
</body></html>`;
}

function stateOf(r) {
  if (r.positive) return r.kind === 'loop' && /^vectorized/.test(r.headline) ? 'vec' : 'ok';
  if (r.applicable && r.verified) return 'fixable';
  if (r.verified) return 'provable';
  return 'declined';
}

function remarkCard(r, index) {
  const state = stateOf(r);
  const jumpLine = r.line !== null ? ` data-line="${r.line}"` : '';
  const kindLabel = r.kind === 'calls-folded'
    ? `${r.count} calls`
    : r.kind === 'call' ? `call ${escapeHtml(r.callee)}` : r.kind;
  const loc = r.kind === 'calls-folded'
    ? ` lines ${r.line}-${r.lineEnd}`
    : r.line !== null ? `:${r.line}` : '';
  let detail = '';
  if (r.reason) detail += `<div class="sub"><span class="k">reason</span><span>${escapeHtml(r.reason)}</span></div>`;
  if (r.fix) detail += `<div class="sub"><span class="k">fix</span><span>${escapeHtml(r.fix)}</span></div>`;
  if (r.verified) detail += `<div class="sub verified"><span class="k">verified</span><span>${escapeHtml(r.verified)}</span></div>`;
  if (r.calls) detail += `<div class="sub"><span class="k">calls</span><span>${escapeHtml(r.calls)}</span></div>`;
  // Apply requires BOTH a synthesizable edit and the optimizer's verified
  // simulation -- an unverified one-click edit is a promise the compiler
  // hasn't kept, and clicking it "does nothing" from the user's seat.
  const applyBtn = r.applicable && r.verified
    ? `<button class="apply" data-apply="${index}" title="${escapeHtml(r.applicable.title)} (verified by the optimizer)">Apply fix</button>`
    : '';
  // A deeply nested scalar loop is a better optimization target than a
  // top-level one (its body runs multiplicatively more often).
  const depthChip = r.kind === 'loop' && !r.positive && (r.depth || 0) >= 2
    ? `<span class="depth-chip" title="This loop is ${r.depth} levels deep; its body runs multiplicatively more often than a top-level loop's.">nest depth ${r.depth}</span>`
    : '';
  return `<div class="card s-${state}" data-kind="${r.kind}" data-state="${state}" data-index="${index}"${jumpLine}>
    <div class="head">
      <span class="meta">${escapeHtml(kindLabel)}${escapeHtml(loc)}</span>
      <span class="headline">${escapeHtml(r.headline)}</span>
      ${depthChip}
      ${applyBtn}
    </div>
    ${detail ? `<div class="detail">${detail}</div>` : ''}
  </div>`;
}

function bar(parts) {
  const total = parts.reduce((acc, p) => acc + p.value, 0);
  if (total === 0) return '<div class="bar"><div class="seg dim" style="width:100%"></div></div>';
  const segs = parts
    .filter((p) => p.value > 0)
    .map((p) => `<div class="seg ${p.cls}" style="width:${(100 * p.value / total).toFixed(1)}%" title="${escapeHtml(p.title)}"></div>`)
    .join('');
  return `<div class="bar">${segs}</div>`;
}

function dashboard(model, loopFixCount, callFixCount) {
  const s = model.stats;
  const loopTotal = s.vectorized + s.scalar;
  const loopBar = bar([
    { value: s.vectorized, cls: 'good', title: `${s.vectorized} vectorized` },
    { value: loopFixCount, cls: 'gold', title: `${loopFixCount} with a verified fix that makes the loop vectorize` },
    { value: Math.max(0, s.scalar - loopFixCount), cls: 'bad', title: `${s.scalar - loopFixCount} scalar` },
  ]);
  const backendBar = s.weightedPct !== null
    ? bar([
      { value: s.weightedPct, cls: 'good', title: `${s.weightedPct}% of instructions register-allocated` },
      { value: 100 - s.weightedPct, cls: 'bad', title: `${(100 - s.weightedPct).toFixed(1)}% baseline codegen` },
    ])
    : '';
  return `<div class="dash">
    <div class="dash-item">
      <div class="dash-label">Loops <span class="dash-num">${s.vectorized}/${loopTotal} vectorized${s.unrolled ? ` · ${s.unrolled} unrolled` : ''}</span></div>
      ${loopBar}
    </div>
    ${backendBar ? `<div class="dash-item">
      <div class="dash-label">Backend <span class="dash-num">${s.backendOk}/${s.backendTotal} functions · ${s.weightedPct}% of instructions register-allocated</span></div>
      ${backendBar}
    </div>` : ''}
    <div class="dash-item">
      <div class="dash-label">Calls <span class="dash-num">${s.inlined} inlined · ${s.refused} kept as real calls${callFixCount > 0 ? ` · ${callFixCount} with a verified inline fix` : ''}</span></div>
    </div>
  </div>`;
}

function backendSection(model) {
  if (model.backend.summary.length === 0 && model.backend.groups.length === 0) return '';
  let html = '<h2>Backend fallbacks</h2>';
  if (model.backend.groups.length === 0) {
    html += '<div class="empty">Every function compiled with the register-allocating backend.</div>';
    return html;
  }
  for (const g of model.backend.groups) {
    html += `<div class="card s-neutral">
      <div class="head"><span class="headline">${escapeHtml(g.reason)}</span>
        <span class="meta">${g.functions} fn · ${g.instructions} instrs</span></div>
      <div class="detail">
      ${g.consequence ? `<div class="sub"><span class="k">consequence</span><span>${escapeHtml(g.consequence)}</span></div>` : ''}
      ${g.fix ? `<div class="sub"><span class="k">fix</span><span>${escapeHtml(g.fix)}</span></div>` : ''}
      ${g.members ? `<div class="sub"><span class="k">functions</span><span>${escapeHtml(g.members)}</span></div>` : ''}
      </div>
    </div>`;
  }
  return html;
}

function memoryCard(m) {
  // Errors share the declined (red) accent; warnings the provable (gold) one.
  const cls = m.severity === 'error' ? 's-declined' : 's-provable';
  const jumpLine = m.line !== null ? ` data-line="${m.line}"` : '';
  const loc = m.line !== null ? `:${m.line}` : '';
  const detail = m.fix
    ? `<div class="detail"><div class="sub"><span class="k">fix</span><span>${escapeHtml(m.fix)}</span></div></div>`
    : '';
  return `<div class="card ${cls}"${jumpLine}>
    <div class="head">
      <span class="meta">${m.severity}${escapeHtml(loc)}</span>
      <span class="headline">${escapeHtml(m.headline)}</span>
    </div>
    ${detail}
  </div>`;
}

function memorySection(model) {
  const mem = model.memory || [];
  if (mem.length === 0) {
    return `<div class="empty">No memory diagnostics in this file. The analyzer flags
      use-after-free, double free, leaks, dangling borrows, null dereference, and
      out-of-bounds access, but only when it can prove them, so anything here is real.</div>`;
  }
  const errors = mem.filter((m) => m.severity === 'error');
  const warnings = mem.filter((m) => m.severity === 'warning');
  const parts = [
    errors.length ? `${errors.length} error${errors.length === 1 ? '' : 's'}` : '',
    warnings.length ? `${warnings.length} warning${warnings.length === 1 ? '' : 's'}` : '',
  ].filter(Boolean).join(', ');
  let html = `<div class="mem-summary">${mem.length} issue${mem.length === 1 ? '' : 's'}: ${parts}.
    Each is proven; the analyzer stays silent when it isn't certain.</div>`;
  if (errors.length) html += '<h2>Errors</h2>' + errors.map(memoryCard).join('\n');
  if (warnings.length) html += '<h2>Warnings</h2>' + warnings.map(memoryCard).join('\n');
  return html;
}

function groupByFunction(model) {
  /** @type {Map<string, {remarks: {r: any, index: number}[], vec: number, scalar: number, fixable: number, minLine: number}>} */
  const groups = new Map();
  model.remarks.forEach((r, index) => {
    if (!groups.has(r.fn)) {
      groups.set(r.fn, { remarks: [], vec: 0, scalar: 0, fixable: 0, minLine: r.line || 0 });
    }
    const g = groups.get(r.fn);
    g.remarks.push({ r, index });
    if (r.line !== null) g.minLine = Math.min(g.minLine || r.line, r.line);
    if (r.kind === 'loop') {
      if (r.positive && /^vectorized/.test(r.headline)) g.vec++;
      else if (!r.positive) g.scalar++;
    }
    if (r.applicable && r.verified) g.fixable++;
  });
  return [...groups.entries()].sort((a, b) => (a[1].minLine || 0) - (b[1].minLine || 0));
}

function renderHtml(model, sourceLabel) {
  const s = model.stats;
  const fixableRemarks = model.remarks.filter((r) => r.applicable && r.verified);
  const fixableCount = fixableRemarks.length;
  // A fix's verified claim names what it improves: loop fixes make a loop
  // vectorize; call fixes make a call inline. The dashboard and banner must
  // not promise one when offering the other (an applied inlining fix moves
  // the CALLS line, not the LOOPS line).
  const loopFixCount = fixableRemarks.filter((r) => r.kind === 'loop').length;
  const callFixCount = fixableCount - loopFixCount;
  const groups = groupByFunction(model);

  // Memory tab badge: red when any error, gold for warning-only, hidden when clean.
  const mem = model.memory || [];
  const memErrors = mem.filter((m) => m.severity === 'error').length;
  const memBadge = mem.length
    ? `<span class="tab-badge ${memErrors ? 'err' : 'warn'}">${mem.length}</span>`
    : '';

  const fixSummary = [
    loopFixCount > 0 ? `${loopFixCount} make${loopFixCount === 1 ? 's' : ''} a loop vectorize` : '',
    callFixCount > 0 ? `${callFixCount} inline${callFixCount === 1 ? 's' : ''} a call` : '',
  ].filter(Boolean).join(', ');
  const banner = fixableCount > 0
    ? `<div class="banner">
        <div class="banner-text"><strong>${fixableCount} verified fix${fixableCount === 1 ? '' : 'es'} available: ${fixSummary}.</strong>
        Each change was applied to an internal copy and re-accepted by the optimizer before being suggested.</div>
        <button class="apply-all">Apply all</button>
      </div>`
    : '';

  // What the last edit did to the optimizer's decisions, regressions first.
  let changesHtml = '';
  if (model.changes) {
    const entries = model.changes.entries || [];
    if (entries.length > 0) {
      const rows = [...entries]
        .sort((a, b) => (a.direction === 'regressed' ? 0 : 1) - (b.direction === 'regressed' ? 0 : 1))
        .map((e) => {
          const regressed = e.direction === 'regressed';
          const what = e.kind === 'loop'
            ? (regressed ? 'was vectorized, now scalar' : 'now vectorized')
            : (regressed ? 'was inlined, now a real call' : 'now inlined');
          return `<div class="change ${regressed ? 'regressed' : 'improved'}" data-jump="${e.line || 1}">
            <span class="change-sign">${regressed ? '-' : '+'}</span>
            <span><strong>${escapeHtml(e.fn)}</strong> (${escapeHtml(e.kind)} @ line ${e.line}): ${regressed ? '<strong>REGRESSED</strong> — ' : ''}${what}${regressed && e.reason ? `<div class="change-reason">${escapeHtml(e.reason)}</div>` : ''}</span>
          </div>`;
        }).join('\n');
      changesHtml = `<div class="changes ${model.changes.regressed > 0 ? 'has-regression' : ''}">
        <div class="changes-title">Since the last compile</div>
        ${rows}
      </div>`;
    } else {
      changesHtml = '<div class="changes-none">No optimization changes since the last compile.</div>';
    }
  }

  const groupHtml = groups.map(([fn, g]) => {
    const rollup = [
      g.vec > 0 ? `<span class="pill good">${g.vec} vec</span>` : '',
      g.fixable > 0 ? `<span class="pill gold">${g.fixable} fix${g.fixable === 1 ? '' : 'es'}</span>` : '',
      g.scalar - g.fixable > 0 ? `<span class="pill bad">${g.scalar - g.fixable} scalar</span>` : '',
    ].join('');
    const attention = g.fixable > 0 || g.scalar > 0;
    return `<details class="fn-group"${attention ? ' open' : ' open'}>
      <summary><span class="fn">${escapeHtml(fn)}</span>${rollup}<span class="count">${g.remarks.length}</span></summary>
      ${g.remarks.map(({ r, index }) => remarkCard(r, index)).join('\n')}
    </details>`;
  }).join('\n');

  return `<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
<style>
  :root {
    --good: #4ec994; --bad: #e06c75; --gold: #d7ba7d;
    --border: var(--vscode-panel-border);
  }
  * { box-sizing: border-box; }
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 0 16px 32px; font-size: 13px; }
  h1 { font-size: 1.05em; display: flex; align-items: center; gap: 9px; margin-bottom: 6px; }
  h1 .src { opacity: 0.65; font-weight: normal; font-size: 0.85em; font-family: var(--vscode-editor-font-family); }
  ${LOGO_CSS}
  #content { transition: opacity 0.2s ease; }
  .busy #content { opacity: 0.55; pointer-events: none; }

  .tabs { display: flex; gap: 2px; margin: 12px 0 0; border-bottom: 1px solid var(--border); }
  .tab { background: transparent; border: none; border-bottom: 2px solid transparent; margin-bottom: -1px;
         color: var(--vscode-foreground); padding: 6px 14px; cursor: pointer; font-size: 0.9em; opacity: 0.65; }
  .tab:hover { opacity: 1; }
  .tab.active { opacity: 1; border-bottom-color: var(--vscode-focusBorder); font-weight: 600; }
  .tab-badge { display: inline-block; margin-left: 7px; font-size: 0.74em; border-radius: 8px; padding: 0 7px;
               vertical-align: middle; background: color-mix(in srgb, var(--bad) 16%, transparent); color: var(--bad); }
  .tab-badge.warn { background: color-mix(in srgb, var(--gold) 18%, transparent); color: var(--gold); }
  .tab-panel.hidden { display: none; }
  .mem-summary { margin: 14px 0 2px; line-height: 1.45; opacity: 0.85; }
  h2 { font-size: 0.95em; margin-top: 26px; border-top: 1px solid var(--border); padding-top: 16px; opacity: 0.9;
       text-transform: uppercase; letter-spacing: 0.06em; font-weight: 600; }

  .dash { display: flex; flex-direction: column; gap: 8px; margin: 10px 0 4px; }
  .dash-item { }
  .dash-label { display: flex; justify-content: space-between; font-size: 0.88em; margin-bottom: 3px;
                text-transform: uppercase; letter-spacing: 0.05em; opacity: 0.75; }
  .dash-num { text-transform: none; letter-spacing: 0; font-family: var(--vscode-editor-font-family); }
  .bar { display: flex; height: 7px; border-radius: 4px; overflow: hidden; background: var(--vscode-input-background); }
  .seg.good { background: var(--good); } .seg.bad { background: color-mix(in srgb, var(--bad) 55%, transparent); }
  .seg.gold { background: var(--gold); } .seg.dim { background: var(--vscode-input-background); }

  .banner { display: flex; gap: 12px; align-items: center; margin: 14px 0 6px; padding: 9px 13px;
            border: 1px solid var(--border); border-left: 3px solid var(--gold); border-radius: 6px; }
  .banner-text { flex: 1; line-height: 1.45; }
  .apply-all { background: var(--vscode-button-background); color: var(--vscode-button-foreground);
               border: none; border-radius: 4px; padding: 5px 14px; cursor: pointer; white-space: nowrap; }
  .apply-all:hover { background: var(--vscode-button-hoverBackground); }

  .toolbar { display: flex; flex-wrap: wrap; gap: 6px; margin: 12px 0; align-items: center; position: sticky; top: 0;
             background: var(--vscode-editor-background); padding: 8px 0; z-index: 2;
             border-bottom: 1px solid var(--border); }
  .chip { border: 1px solid var(--border); border-radius: 10px; padding: 3px 11px; cursor: pointer;
          background: transparent; color: var(--vscode-foreground); font-size: 0.85em; }
  .chip:hover { background: var(--vscode-list-hoverBackground); }
  .chip.active { outline: 1px solid var(--vscode-focusBorder); background: var(--vscode-list-activeSelectionBackground); }
  input.search { flex: 1; min-width: 130px; background: var(--vscode-input-background); color: var(--vscode-input-foreground);
                 border: 1px solid var(--vscode-input-border, transparent); border-radius: 4px; padding: 4px 9px; }
  button.refresh { background: var(--vscode-button-background); color: var(--vscode-button-foreground); border: none;
                   border-radius: 4px; padding: 5px 13px; cursor: pointer; }

  details.fn-group { margin: 10px 0; border: 1px solid var(--border); border-radius: 8px; padding: 0; overflow: hidden; }
  details.fn-group > summary { cursor: pointer; list-style: none; display: flex; align-items: center; gap: 8px;
                               padding: 7px 12px; background: var(--vscode-sideBar-background, transparent);
                               user-select: none; }
  details.fn-group > summary::before { content: '▸'; opacity: 0.6; transition: transform 0.12s; }
  details.fn-group[open] > summary::before { transform: rotate(90deg); }
  details.fn-group > summary:hover { background: var(--vscode-list-hoverBackground); }
  .fn { font-weight: 600; font-family: var(--vscode-editor-font-family); }
  .pill { font-size: 0.78em; border-radius: 8px; padding: 1px 8px; }
  .pill.good { background: color-mix(in srgb, var(--good) 16%, transparent); color: var(--good); }
  .pill.bad { background: color-mix(in srgb, var(--bad) 14%, transparent); color: var(--bad); }
  .pill.gold { background: color-mix(in srgb, var(--gold) 18%, transparent); color: var(--gold); }
  summary .count { margin-left: auto; opacity: 0.45; font-size: 0.8em; }

  .card { padding: 7px 12px 7px 14px; border-top: 1px solid var(--border); cursor: pointer; position: relative; }
  .card::before { content: ''; position: absolute; left: 0; top: 0; bottom: 0; width: 3px; }
  .card.s-vec::before, .card.s-ok::before { background: var(--good); }
  .card.s-fixable::before, .card.s-provable::before { background: var(--gold); }
  .card.s-declined::before { background: color-mix(in srgb, var(--bad) 60%, transparent); }
  .card.s-neutral::before { background: var(--border); }
  .card:hover { background: var(--vscode-list-hoverBackground); }
  .head { display: flex; gap: 9px; align-items: baseline; }
  .meta { opacity: 0.55; font-size: 0.85em; font-family: var(--vscode-editor-font-family); white-space: nowrap; }
  .headline { font-family: var(--vscode-editor-font-family); font-size: 0.92em; flex: 1; }
  button.apply { background: transparent; border: 1px solid var(--gold); color: var(--gold); border-radius: 5px;
                 padding: 2px 10px; cursor: pointer; font-size: 0.84em; white-space: nowrap; }
  button.apply:hover { background: color-mix(in srgb, var(--gold) 18%, transparent); }
  .detail { margin-top: 5px; }
  .sub { display: flex; gap: 8px; margin-top: 3px; font-size: 0.88em; opacity: 0.92; line-height: 1.45; }
  .sub .k { flex: 0 0 86px; opacity: 0.55; text-transform: uppercase; font-size: 0.78em; letter-spacing: 0.06em;
            padding-top: 2px; text-align: right; }
  .sub.verified { color: var(--good); }
  .empty { opacity: 0.6; font-style: italic; margin-top: 16px; }

  .changes { margin: 12px 0 4px; border: 1px solid var(--border); border-radius: 6px; padding: 9px 13px;
             border-left: 3px solid var(--good); }
  .changes.has-regression { border-left-color: var(--bad); }
  .changes-title { font-size: 0.82em; text-transform: uppercase; letter-spacing: 0.06em; opacity: 0.7;
                   margin-bottom: 6px; }
  .change { display: flex; gap: 9px; padding: 3px 0; cursor: pointer; line-height: 1.45; }
  .change:hover { background: var(--vscode-list-hoverBackground); }
  .change-sign { font-family: var(--vscode-editor-font-family); font-weight: 700; }
  .change.improved .change-sign { color: var(--good); }
  .change.regressed .change-sign, .change.regressed strong ~ strong { color: var(--bad); }
  .change-reason { opacity: 0.75; font-size: 0.9em; margin-top: 2px; }
  .changes-none { margin: 12px 0 4px; opacity: 0.55; font-size: 0.88em; font-style: italic; }
  .depth-chip { font-size: 0.75em; border-radius: 7px; padding: 1px 7px; white-space: nowrap;
                background: color-mix(in srgb, var(--bad) 10%, transparent); opacity: 0.85; }
</style></head>
<body class="settle">
  <h1>${logoSvg(18)} Optimization report <span class="src">${escapeHtml(sourceLabel)}</span></h1>
  <div id="content">
  <div class="tabs">
    <button class="tab active" data-tab="opt">Optimizations</button>
    <button class="tab" data-tab="mem">Memory${memBadge}</button>
  </div>
  <div class="tab-panel" data-panel="opt">
  ${changesHtml}
  ${dashboard(model, loopFixCount, callFixCount)}
  ${banner}
  <div class="toolbar">
    <button class="chip" data-filter="attention">Needs attention</button>
    <button class="chip" data-filter="vectorized">Vectorized</button>
    <button class="chip" data-filter="fixes">Fixes</button>
    <button class="chip" data-filter="calls">Calls</button>
    <input class="search" type="text" placeholder="filter (function, kernel, reason)">
    <button class="refresh" title="Recompile with --release --explain">Refresh</button>
  </div>
  <div id="groups">${groupHtml || '<div class="empty">No loops or calls to report.</div>'}</div>
  ${backendSection(model)}
  </div>
  <div class="tab-panel hidden" data-panel="mem">
  ${memorySection(model)}
  </div>
  </div>
  <script>
    const vscode = acquireVsCodeApi();
    // In-place refresh: the extension posts 'busy' before recompiling and
    // 'idle' after. The idle message matters because assigning identical
    // HTML to a webview is a no-op (VS Code does not reload the document),
    // so an unchanged report would otherwise keep pulsing forever.
    window.addEventListener('message', (e) => {
      if (!e.data) return;
      if (e.data.type === 'busy') {
        document.body.classList.remove('settle');
        document.body.classList.add('busy');
      } else if (e.data.type === 'idle') {
        document.body.classList.remove('busy');
        void document.body.offsetWidth; // restart the settle animation
        document.body.classList.add('settle');
      }
    });
    let activeFilter = 'all';
    const search = document.querySelector('input.search');
    function matchesFilter(card) {
      const kind = card.dataset.kind, state = card.dataset.state;
      switch (activeFilter) {
        case 'attention': return state === 'declined' || state === 'fixable' || state === 'provable';
        case 'vectorized': return kind === 'loop' && state === 'vec';
        case 'fixes': return state === 'fixable' || state === 'provable';
        case 'calls': return kind === 'call' || kind === 'calls-folded';
        default: return true;
      }
    }
    function applyFilters() {
      const q = search.value.trim().toLowerCase();
      for (const group of document.querySelectorAll('details.fn-group')) {
        let visible = 0;
        for (const card of group.querySelectorAll('.card')) {
          const show = matchesFilter(card) && (!q || card.textContent.toLowerCase().includes(q) ||
            group.querySelector('.fn').textContent.toLowerCase().includes(q));
          card.style.display = show ? '' : 'none';
          if (show) visible++;
        }
        group.style.display = visible > 0 ? '' : 'none';
      }
    }
    for (const chip of document.querySelectorAll('.chip')) {
      chip.addEventListener('click', () => {
        activeFilter = (activeFilter === chip.dataset.filter) ? 'all' : chip.dataset.filter;
        for (const c of document.querySelectorAll('.chip')) c.classList.toggle('active', c.dataset.filter === activeFilter);
        applyFilters();
      });
    }
    search.addEventListener('input', applyFilters);
    document.querySelector('button.refresh').addEventListener('click', () => vscode.postMessage({ type: 'refresh' }));
    const applyAll = document.querySelector('.apply-all');
    if (applyAll) applyAll.addEventListener('click', () => vscode.postMessage({ type: 'applyAll' }));
    document.getElementById('groups').addEventListener('click', (e) => {
      const btn = e.target.closest('button.apply');
      if (btn) {
        e.stopPropagation();
        vscode.postMessage({ type: 'applyFix', index: parseInt(btn.dataset.apply, 10) });
        return;
      }
      const card = e.target.closest('.card');
      if (card && card.dataset.line) vscode.postMessage({ type: 'jump', line: parseInt(card.dataset.line, 10) });
    });
    for (const change of document.querySelectorAll('.change')) {
      change.addEventListener('click', () => {
        vscode.postMessage({ type: 'jump', line: parseInt(change.dataset.jump, 10) || 1 });
      });
    }
    // Tab switching between the Optimizations and Memory panels.
    for (const tab of document.querySelectorAll('.tab')) {
      tab.addEventListener('click', () => {
        for (const t of document.querySelectorAll('.tab')) t.classList.toggle('active', t === tab);
        for (const p of document.querySelectorAll('.tab-panel')) p.classList.toggle('hidden', p.dataset.panel !== tab.dataset.tab);
      });
    }
    // Memory cards jump to the diagnostic's source line, like optimization cards.
    const memPanel = document.querySelector('.tab-panel[data-panel="mem"]');
    if (memPanel) memPanel.addEventListener('click', (e) => {
      const card = e.target.closest('.card');
      if (card && card.dataset.line) vscode.postMessage({ type: 'jump', line: parseInt(card.dataset.line, 10) });
    });
  </script>
</body></html>`;
}

async function refreshReport(deps, filePath) {
  if (refreshInFlight) {
    refreshQueued = true;
    return;
  }
  refreshInFlight = true;
  try {
    await refreshReportNow(deps, filePath);
  } catch (err) {
    // A silent rejection here would leave the panel dimmed and pulsing with
    // no way out; surface the failure instead.
    if (reportPanel) {
      reportPanel.webview.html = `<!DOCTYPE html><html><head>
        <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline';">
        <style>body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 16px; }</style>
        </head><body><h3>Optimization report failed</h3>
        <pre style="white-space: pre-wrap; opacity: 0.8;">${escapeHtml(err && err.stack ? err.stack : String(err))}</pre>
        </body></html>`;
    }
    updateStatusBar();
  } finally {
    refreshInFlight = false;
  }
  if (refreshQueued) {
    refreshQueued = false;
    // One follow-up run picks up whatever changed during the compile.
    await refreshReport(deps, reportSourcePath || filePath);
  }
}

async function refreshReportNow(deps, filePath) {
  const workspaceRoot = vscode.workspace.workspaceFolders?.find((f) =>
    filePath.toLowerCase().startsWith(f.uri.fsPath.toLowerCase()))?.uri.fsPath || path.dirname(filePath);

  if (reportPanel) {
    reportPanel.title = `Mettle: ${path.basename(filePath)}`;
    // In-place feedback: keep the current report visible (dimmed) and let
    // the header mark pulse while the compiler runs.
    reportPanel.webview.postMessage({ type: 'busy' });
  }
  if (statusItem) {
    statusItem.text = '$(sync~spin) mettle --explain';
    statusItem.show();
  }
  const result = await runExplain(deps, filePath, workspaceRoot);
  if (!result.ok) {
    if (reportPanel) {
      const detail = result.output ? `<pre style="white-space: pre-wrap; opacity: 0.8;">${escapeHtml(result.output.slice(0, 4000))}</pre>` : '';
      reportPanel.webview.html = `<!DOCTYPE html><html><head>
        <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline';">
        <style>body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 16px; }
        h3 { display: flex; align-items: center; gap: 9px; font-size: 1em; } ${LOGO_CSS}</style></head>
        <body><h3>${logoSvg(18)} Could not produce a report</h3>
        <p>${escapeHtml(result.error)}</p>${detail}</body></html>`;
    }
    reportModel = null;
    applyDecorations();
    updateStatusBar();
    notifyReportChanged();
    return;
  }
  reportModel = result.json ? modelFromJson(result.json) : parseReport(result.output);
  reportSourcePath = filePath;
  annotateApplicableFixes(reportModel, filePath);
  notifyReportChanged();
  if (reportPanel) {
    reportPanel.webview.html = renderHtml(reportModel, path.basename(filePath));
    // If the rendered HTML is identical to the current document (a refresh
    // with no source change), the assignment above does NOT reload the
    // webview -- clear the busy state explicitly. Harmless when the page
    // did reload (the fresh document is idle already).
    reportPanel.webview.postMessage({ type: 'idle' });
  }
  applyDecorations();
  updateStatusBar();
}

// --- quick fixes (editor lightbulb) ---

class MettleExplainCodeActions {
  provideCodeActions(document, range) {
    if (!reportModel || !reportSourcePath) return [];
    if (path.normalize(document.uri.fsPath).toLowerCase() !==
      path.normalize(reportSourcePath).toLowerCase()) return [];
    const actions = [];
    const lines = document.getText().split(/\r?\n/);
    for (const r of reportModel.remarks) {
      if (r.line === null || r.positive || !r.verified) continue;
      if (r.line - 1 < range.start.line || r.line - 1 > range.end.line) continue;
      const synthesized = synthesizeFix(r, lines);
      if (!synthesized) continue;
      const title = `Mettle: ${synthesized.title} (verified by the optimizer)`;
      const action = new vscode.CodeAction(title, vscode.CodeActionKind.QuickFix);
      action.isPreferred = true;
      const edit = new vscode.WorkspaceEdit();
      for (const e of synthesized.edits) {
        edit.replace(document.uri, new vscode.Range(e.line, e.start, e.line, e.end), e.newText);
      }
      action.edit = edit;
      actions.push(action);
    }
    return actions;
  }
}

/**
 * @param {vscode.ExtensionContext} context
 * @param {{ findCompiler: (workspaceRoot: string, filePath: string) => string }} deps
 */
function registerExplain(context, deps) {
  decorationsEnabled = vscode.workspace.getConfiguration('mettle').get('explain.inlineHints', true);

  statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 95);
  statusItem.command = 'mettle.showOptimizationReport';
  context.subscriptions.push(statusItem);
  updateStatusBar();

  const showReport = async () => {
    const editor = vscode.window.activeTextEditor;
    const doc = editor?.document;
    if (!doc || doc.languageId !== 'mettle') {
      vscode.window.showInformationMessage('Open a Mettle file to show its optimization report.');
      return;
    }
    if (doc.isDirty) {
      suppressSaveRefresh = true;
      try { await doc.save(); } finally { suppressSaveRefresh = false; }
    }
    const filePath = doc.uri.fsPath;

    if (!reportPanel) {
      reportPanel = vscode.window.createWebviewPanel(
        'mettleOptimizationReport',
        'Mettle: Optimization Report',
        { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true },
        { enableScripts: true, retainContextWhenHidden: true }
      );
      reportPanel.onDidDispose(() => {
        reportPanel = null;
        reportModel = null;
        reportSourcePath = null;
        applyDecorations();
        updateStatusBar();
        notifyReportChanged();
      }, null, context.subscriptions);
      reportPanel.webview.onDidReceiveMessage(async (msg) => {
        try {
          if (!reportSourcePath) return;
          if (msg.type === 'jump') {
            const target = vscode.Uri.file(reportSourcePath);
            const opened = await vscode.window.showTextDocument(target, { viewColumn: vscode.ViewColumn.One });
            const line = Math.max(0, (msg.line || 1) - 1);
            const pos = new vscode.Position(Math.min(line, opened.document.lineCount - 1), 0);
            opened.selection = new vscode.Selection(pos, pos);
            opened.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
          } else if (msg.type === 'refresh') {
            await refreshReport(deps, reportSourcePath);
          } else if (msg.type === 'applyFix' && reportModel) {
            const remark = reportModel.remarks[msg.index];
            if (!remark) return;
            reportApplyOutcome(await applyFixes([remark], reportSourcePath, deps.log));
          } else if (msg.type === 'applyAll' && reportModel) {
            const fixables = reportModel.remarks.filter((r) => r.applicable && r.verified);
            reportApplyOutcome(await applyFixes(fixables, reportSourcePath, deps.log));
          }
        } catch (err) {
          vscode.window.showErrorMessage(`Mettle report action failed: ${err && err.message ? err.message : err}`);
          if (deps.log) deps.log(`[explain] action '${msg && msg.type}' failed: ${err && err.stack ? err.stack : err}`);
        }
      }, null, context.subscriptions);
    } else {
      reportPanel.reveal(undefined, true);
    }
    // Full interstitial only on a cold open or when switching files; an
    // in-place refresh keeps the existing report visible instead.
    if (reportSourcePath !== filePath || !reportModel) {
      reportPanel.webview.html = loadingHtml(path.basename(filePath));
    }
    await refreshReport(deps, filePath);
  };

  context.subscriptions.push(
    vscode.workspace.onDidSaveTextDocument(async (doc) => {
      if (!reportSourcePath || suppressSaveRefresh) return;
      if (!vscode.workspace.getConfiguration('mettle').get('explain.refreshOnSave', true)) return;
      if (path.normalize(doc.uri.fsPath).toLowerCase() === path.normalize(reportSourcePath).toLowerCase()) {
        await refreshReport(deps, reportSourcePath);
      }
    }),
    vscode.window.onDidChangeVisibleTextEditors(() => applyDecorations()),
    vscode.window.onDidChangeActiveTextEditor(() => updateStatusBar()),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('mettle.explain.inlineHints')) {
        decorationsEnabled = vscode.workspace.getConfiguration('mettle').get('explain.inlineHints', true);
        applyDecorations();
      }
    }),
    vscode.languages.registerCodeActionsProvider({ language: 'mettle' }, new MettleExplainCodeActions(), {
      providedCodeActionKinds: [vscode.CodeActionKind.QuickFix],
    }),
    // .explain.txt sidecars: `@ line N` becomes a link into the source file
    // named in the report header (resolved in the workspace by basename).
    vscode.languages.registerDocumentLinkProvider({ language: 'mettle-explain' }, {
      async provideDocumentLinks(document) {
        const text = document.getText();
        const headerMatch = text.match(/-- optimization report: (\S+)/);
        if (!headerMatch) return [];
        const sourceUri = await resolveReportSource(document.uri, headerMatch[1]);
        if (!sourceUri) return [];
        const links = [];
        const re = /@ line (\d+)|lines (\d+)-(\d+)/g;
        let m;
        while ((m = re.exec(text)) !== null) {
          const line = parseInt(m[1] || m[2], 10);
          const start = document.positionAt(m.index);
          const end = document.positionAt(m.index + m[0].length);
          const link = new vscode.DocumentLink(
            new vscode.Range(start, end),
            sourceUri.with({ fragment: `L${line}` })
          );
          link.tooltip = 'Open in source';
          links.push(link);
        }
        return links;
      },
    })
  );

  return { showReport };
}

/** Resolve the source file a sidecar report describes: same directory as the
 * sidecar first (typical for -o next to the source), then a workspace search. */
async function resolveReportSource(sidecarUri, basename) {
  const sibling = path.join(path.dirname(sidecarUri.fsPath), basename);
  if (fs.existsSync(sibling)) return vscode.Uri.file(sibling);
  const found = await vscode.workspace.findFiles(`**/${basename}`, '**/node_modules/**', 1);
  return found.length > 0 ? found[0] : null;
}

// --- report state for other extension features (CodeLens) ---

const reportListeners = [];
function notifyReportChanged() {
  for (const cb of reportListeners) {
    try { cb(); } catch (_) { /* a listener must not break the report */ }
  }
}
/** Subscribe to report model changes (load, refresh, panel close). */
function onReportChanged(cb) {
  reportListeners.push(cb);
}
/** The current parsed report and which source file produced it. */
function getReportState() {
  return { model: reportModel, sourcePath: reportSourcePath };
}

module.exports = {
  registerExplain, parseReport, modelFromJson, synthesizeFix, renderHtml,
  loadingHtml, onReportChanged, getReportState,
};
