/**
 * Mettle source analysis: a masked-text symbol indexer shared by the
 * navigation providers (definition, references, completion, signature help,
 * outline, rename) in language.js.
 *
 * Deliberately NOT a parser. It scans comment/string-masked source with
 * line-anchored patterns and a brace-depth tracker, which is exactly enough
 * for navigation and survives code the real compiler would reject (the state
 * a file is in most of the time while being edited). No `vscode` import --
 * everything here is testable offline with plain node.
 */

const fs = require('fs');
const path = require('path');

// --- masking -----------------------------------------------------------------

/**
 * Replace comment/string/char contents with spaces, preserving line structure
 * so (line, column) positions survive. Nested block comments are handled the
 * way the compiler handles them.
 */
function maskNonCode(text) {
  let out = '';
  let i = 0;
  let blockDepth = 0;
  let inString = false;
  let inChar = false;
  let inLineComment = false;

  while (i < text.length) {
    const ch = text[i];
    const next = text[i + 1];

    if (ch === '\r' || ch === '\n') {
      out += ch;
      i++;
      inLineComment = false;
      continue;
    }
    if (inLineComment) { out += ' '; i++; continue; }
    if (inString) {
      if (ch === '\\') { out += '  '; i += 2; continue; }
      out += ' ';
      if (ch === '"') inString = false;
      i++;
      continue;
    }
    if (inChar) {
      if (ch === '\\') { out += '  '; i += 2; continue; }
      out += ' ';
      if (ch === "'") inChar = false;
      i++;
      continue;
    }
    if (blockDepth > 0) {
      if (ch === '/' && next === '*') { out += '  '; i += 2; blockDepth++; continue; }
      if (ch === '*' && next === '/') { out += '  '; i += 2; blockDepth--; continue; }
      out += ' ';
      i++;
      continue;
    }
    if (ch === '/' && next === '/') { out += '  '; i += 2; inLineComment = true; continue; }
    if (ch === '/' && next === '*') { out += '  '; i += 2; blockDepth = 1; continue; }
    if (ch === '"') { out += ' '; i++; inString = true; continue; }
    if (ch === "'") { out += ' '; i++; inChar = true; continue; }
    out += ch;
    i++;
  }
  return out;
}

// --- small parsing helpers ----------------------------------------------------

/** Split on `sep` at nesting depth zero of () <> [] {}. */
function splitTopLevel(s, sep) {
  const parts = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < s.length; i++) {
    const ch = s[i];
    if (ch === '(' || ch === '<' || ch === '[' || ch === '{') depth++;
    else if (ch === ')' || ch === '>' || ch === ']' || ch === '}') depth = Math.max(0, depth - 1);
    else if (ch === sep && depth === 0) {
      parts.push(s.slice(start, i));
      start = i + 1;
    }
  }
  parts.push(s.slice(start));
  return parts.map((p) => p.trim()).filter((p) => p.length > 0);
}

/** `a: int32` -> { name: 'a', type: 'int32' }; tolerates missing type. */
function parseParam(text) {
  const m = text.match(/^(\w+)\s*:\s*(.+)$/);
  if (m) return { name: m[1], type: m[2].trim() };
  const bare = text.match(/^(\w+)$/);
  if (bare) return { name: bare[1], type: '' };
  return null;
}

/** Strip pointers/arrays/generics off a type to its base identifier: `Point*` -> `Point`, `uint8[256]` -> `uint8`, `Vec<int32>` -> `Vec`. */
function baseTypeName(type) {
  if (!type) return null;
  const m = String(type).trim().match(/^([A-Za-z_][A-Za-z0-9_]*)/);
  return m ? m[1] : null;
}

/**
 * Starting at `(startLine, startCol)` (an opening delimiter), find the
 * matching closer in masked lines. Returns { line, col } of the closer or null.
 */
function findMatch(lines, startLine, startCol, open, close) {
  let depth = 0;
  for (let ln = startLine; ln < lines.length; ln++) {
    const text = lines[ln];
    for (let c = ln === startLine ? startCol : 0; c < text.length; c++) {
      if (text[c] === open) depth++;
      else if (text[c] === close) {
        depth--;
        if (depth === 0) return { line: ln, col: c };
      }
    }
  }
  return null;
}

/** Collect `/// ` doc lines immediately above `startLine` (original text). */
function collectLeadingDoc(lines, startLine) {
  const docs = [];
  for (let i = startLine - 1; i >= 0; i--) {
    const m = lines[i].trim().match(/^\/\/\/\s?(.*)$/);
    if (!m) break;
    docs.unshift(m[1]);
  }
  return docs;
}

/** Single-line render of a declaration header for hovers/signature help. */
function collapseSignature(originalLines, startLine, endLine) {
  const parts = [];
  for (let i = startLine; i <= Math.min(endLine, startLine + 11); i++) {
    parts.push(originalLines[i].trim());
  }
  return parts.join(' ').replace(/\s+/g, ' ').replace(/\s*\{\s*$/, '').trim();
}

// --- module scanning ----------------------------------------------------------

const DECORATORS = ['inline', 'inline!', 'noinline', 'pure', 'noalloc', 'simd', 'simd!'];

/**
 * Scan one module's text into declarations and imports.
 * All positions are 0-based { line, col }.
 *
 * @returns {{
 *   path: string|null,
 *   imports: { raw: string, line: number, startCol: number, endCol: number, isText: boolean }[],
 *   decls: Decl[],
 * }}
 * Decl: { name, kind, line, col, endLine, signature, doc, exported, externName,
 *         params, returnType, fields, methods, variants, container }
 */
function scanModule(text, filePath) {
  const masked = maskNonCode(text);
  const maskedLines = masked.split(/\r?\n/);
  const originalLines = text.split(/\r?\n/);
  const decls = [];
  const imports = [];

  // import lines come from the ORIGINAL text (the path lives inside a string,
  // which masking blanks out).
  for (let ln = 0; ln < originalLines.length; ln++) {
    const m = originalLines[ln].match(/^\s*(import|import_str)\s+"([^"]*)"/);
    if (m) {
      const startCol = originalLines[ln].indexOf('"') + 1;
      imports.push({
        raw: m[2],
        line: ln,
        startCol,
        endCol: startCol + m[2].length,
        isText: m[1] === 'import_str',
      });
    }
  }

  // Brace depth at the START of each line.
  const depthAt = new Array(maskedLines.length).fill(0);
  {
    let depth = 0;
    for (let ln = 0; ln < maskedLines.length; ln++) {
      depthAt[ln] = depth;
      for (const ch of maskedLines[ln]) {
        if (ch === '{') depth++;
        else if (ch === '}') depth = Math.max(0, depth - 1);
      }
    }
  }

  /** End line of the block opened at/after `headerLine` (decl line when no block). */
  function blockEnd(headerLine) {
    for (let ln = headerLine; ln < Math.min(maskedLines.length, headerLine + 8); ln++) {
      const col = maskedLines[ln].indexOf('{');
      if (col >= 0) {
        const close = findMatch(maskedLines, ln, col, '{', '}');
        return close ? close.line : maskedLines.length - 1;
      }
      if (maskedLines[ln].includes(';')) return ln;
    }
    return headerLine;
  }

  /** Parse the `(...)` and `-> type` of a callable starting at headerLine. */
  function parseCallable(headerLine, nameEndCol) {
    let parenLine = headerLine;
    let parenCol = maskedLines[headerLine].indexOf('(', nameEndCol);
    while (parenCol < 0 && parenLine + 1 < maskedLines.length && parenLine - headerLine < 3) {
      parenLine++;
      parenCol = maskedLines[parenLine].indexOf('(');
    }
    if (parenCol < 0) return { params: [], returnType: 'void', sigEndLine: headerLine };
    const close = findMatch(maskedLines, parenLine, parenCol, '(', ')');
    if (!close) return { params: [], returnType: 'void', sigEndLine: parenLine };

    let inner = '';
    for (let ln = parenLine; ln <= close.line; ln++) {
      const from = ln === parenLine ? parenCol + 1 : 0;
      const to = ln === close.line ? close.col : maskedLines[ln].length;
      inner += maskedLines[ln].slice(from, to) + ' ';
    }
    const params = splitTopLevel(inner, ',').map(parseParam).filter(Boolean);

    const afterClose = maskedLines[close.line].slice(close.col + 1);
    let returnType = 'void';
    let arrow = afterClose.match(/^\s*->\s*([^{;=]+)/);
    if (!arrow && close.line + 1 < maskedLines.length) {
      arrow = maskedLines[close.line + 1].match(/^\s*->\s*([^{;=]+)/);
    }
    if (arrow) returnType = arrow[1].trim();
    return { params, returnType, sigEndLine: close.line };
  }

  const fnRe = /^(\s*)((?:export\s+)?)((?:@\w+!?\s+)*)((?:extern\s+)?)(fn|method|kernel)\s+([A-Za-z_][A-Za-z0-9_]*)/;
  const typeRe = /^(\s*)((?:export\s+)?)(struct|enum|trait)\s+([A-Za-z_][A-Za-z0-9_]*)/;
  const varRe = /^(\s*)((?:export\s+)?)((?:extern\s+)?)var\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^=;]+)/;

  for (let ln = 0; ln < maskedLines.length; ln++) {
    const line = maskedLines[ln];

    let m = line.match(fnRe);
    if (m && depthAt[ln] === 0) {
      const name = m[6];
      const col = line.indexOf(name, m[0].length - name.length);
      const callable = parseCallable(ln, col + name.length);
      const externName = m[4].trim() ? matchExternSymbol(originalLines[callable.sigEndLine]) : null;
      decls.push({
        name,
        kind: m[5] === 'kernel' ? 'kernel' : 'function',
        line: ln,
        col,
        endLine: blockEnd(ln),
        signature: collapseSignature(originalLines, ln, callable.sigEndLine),
        doc: collectLeadingDoc(originalLines, ln),
        exported: m[2].trim().length > 0,
        externName,
        params: callable.params,
        returnType: callable.returnType,
        fields: null,
        methods: null,
        variants: null,
        container: null,
      });
      continue;
    }

    m = line.match(typeRe);
    if (m && depthAt[ln] === 0) {
      const kind = m[3];
      const name = m[4];
      const col = line.indexOf(name, m[0].length - name.length);
      const end = blockEnd(ln);
      const decl = {
        name,
        kind,
        line: ln,
        col,
        endLine: end,
        signature: collapseSignature(originalLines, ln, ln),
        doc: collectLeadingDoc(originalLines, ln),
        exported: m[2].trim().length > 0,
        externName: null,
        params: [],
        returnType: null,
        fields: [],
        methods: [],
        variants: [],
        container: null,
      };

      for (let bl = ln + 1; bl < end; bl++) {
        const bodyLine = maskedLines[bl];
        if (kind === 'enum') {
          const v = bodyLine.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\(([^)]*)\))?\s*,?\s*$/);
          if (v) {
            decl.variants.push({
              name: v[1],
              payload: v[3] ? v[3].trim() : null,
              line: bl,
              col: bodyLine.indexOf(v[1]),
            });
          }
          continue;
        }
        const meth = bodyLine.match(/^\s*(?:export\s+)?(?:@\w+!?\s+)*(?:method|fn)\s+([A-Za-z_][A-Za-z0-9_]*)/);
        if (meth) {
          const mcol = bodyLine.indexOf(meth[1], bodyLine.indexOf(meth[0]));
          const callable = parseCallable(bl, mcol + meth[1].length);
          decl.methods.push({
            name: meth[1],
            line: bl,
            col: mcol,
            params: callable.params,
            returnType: callable.returnType,
            signature: collapseSignature(originalLines, bl, callable.sigEndLine),
            doc: collectLeadingDoc(originalLines, bl),
            container: name,
          });
          bl = blockEnd(bl); // skip the method body
          continue;
        }
        const field = bodyLine.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^;]+);/);
        if (field && depthAt[bl] === depthAt[ln] + 1) {
          decl.fields.push({
            name: field[1],
            type: field[2].trim(),
            line: bl,
            col: bodyLine.indexOf(field[1]),
          });
        }
      }
      decls.push(decl);
      continue;
    }

    m = line.match(varRe);
    if (m && depthAt[ln] === 0) {
      const name = m[4];
      decls.push({
        name,
        kind: 'global',
        line: ln,
        col: line.indexOf(name, m[0].length - name.length - m[5].length - 1),
        endLine: ln,
        signature: collapseSignature(originalLines, ln, ln),
        doc: collectLeadingDoc(originalLines, ln),
        exported: m[2].trim().length > 0,
        externName: m[3].trim() ? matchExternSymbol(originalLines[ln]) : null,
        params: [],
        returnType: m[5].trim(),
        fields: null,
        methods: null,
        variants: null,
        container: null,
      });
      continue;
    }
  }

  return { path: filePath || null, imports, decls, maskedLines, originalLines };
}

function matchExternSymbol(originalLine) {
  const m = (originalLine || '').match(/=\s*"([^"]+)"/);
  return m ? m[1] : null;
}

// --- import resolution ----------------------------------------------------------

/**
 * Mirror of the compiler's import search order: absolute, std/ under stdlib,
 * importing file's directory, workspace root, then -I include dirs.
 * @param {{ stdlibRoot: string|null, workspaceRoot: string|null, includeDirs: string[] }} env
 */
function resolveImport(rawPath, importingFile, env, isText) {
  if (!rawPath) return null;
  const tryFile = (p) => {
    const candidates = [p];
    if (!isText && path.extname(p) === '') candidates.push(`${p}.mettle`);
    for (const c of candidates) {
      try {
        if (fs.existsSync(c) && fs.statSync(c).isFile()) return c;
      } catch (_) { /* unreadable: not a match */ }
    }
    return null;
  };

  if (path.isAbsolute(rawPath)) return tryFile(rawPath);
  if ((rawPath.startsWith('std/') || rawPath.startsWith('std\\')) && env.stdlibRoot) {
    const hit = tryFile(path.join(env.stdlibRoot, rawPath));
    if (hit) return hit;
  }
  if (importingFile) {
    const hit = tryFile(path.join(path.dirname(importingFile), rawPath));
    if (hit) return hit;
  }
  if (env.workspaceRoot) {
    const hit = tryFile(path.join(env.workspaceRoot, rawPath));
    if (hit) return hit;
  }
  for (const dir of env.includeDirs || []) {
    const base = path.isAbsolute(dir) ? dir : path.join(env.workspaceRoot || '', dir);
    const hit = tryFile(path.join(base, rawPath));
    if (hit) return hit;
  }
  return null;
}

// --- project index ----------------------------------------------------------------

/**
 * Caches scanned modules by file mtime and walks import closures. Live editor
 * buffers are layered over disk through `overrideText` so unsaved edits are
 * seen immediately.
 */
class ProjectIndex {
  constructor() {
    /** @type {Map<string, { mtimeMs: number, module: ReturnType<typeof scanModule> }>} */
    this.cache = new Map();
    /** @type {Map<string, string>} normalized path -> live buffer text */
    this.overrides = new Map();
  }

  static normalize(p) {
    return path.normalize(p).toLowerCase();
  }

  /** Layer a live (possibly unsaved) buffer over the disk file. */
  overrideText(filePath, text) {
    this.overrides.set(ProjectIndex.normalize(filePath), text);
  }

  clearOverride(filePath) {
    this.overrides.delete(ProjectIndex.normalize(filePath));
  }

  /** @returns {ReturnType<typeof scanModule> | null} */
  getModule(filePath) {
    const key = ProjectIndex.normalize(filePath);
    const override = this.overrides.get(key);
    if (override !== undefined) {
      // Live buffers are cheap to rescan relative to keystroke frequency at
      // the sizes Mettle files run; correctness beats caching here.
      return scanModule(override, filePath);
    }
    let stat;
    try {
      stat = fs.statSync(filePath);
    } catch (_) {
      this.cache.delete(key);
      return null;
    }
    const cached = this.cache.get(key);
    if (cached && cached.mtimeMs === stat.mtimeMs) return cached.module;
    let text;
    try {
      text = fs.readFileSync(filePath, 'utf8');
    } catch (_) {
      return null;
    }
    const module = scanModule(text, filePath);
    this.cache.set(key, { mtimeMs: stat.mtimeMs, module });
    return module;
  }

  /**
   * The entry module plus everything reachable through `import`.
   * @returns {ReturnType<typeof scanModule>[]} entry module first.
   */
  closure(entryPath, env, maxModules = 256) {
    const seen = new Set();
    const order = [];
    const queue = [entryPath];
    while (queue.length > 0 && order.length < maxModules) {
      const current = queue.shift();
      const key = ProjectIndex.normalize(current);
      if (seen.has(key)) continue;
      seen.add(key);
      const module = this.getModule(current);
      if (!module) continue;
      order.push(module);
      for (const imp of module.imports) {
        if (imp.isText) continue;
        const target = resolveImport(imp.raw, current, env, false);
        if (target) queue.push(target);
      }
    }
    return order;
  }

  /**
   * Find declarations named `name` visible from `entryPath`: own module first
   * (everything), then imported modules (exported-only when the module
   * exports anything -- mirroring the compiler's visibility rule).
   */
  lookup(name, entryPath, env) {
    const modules = this.closure(entryPath, env);
    const results = [];
    for (let i = 0; i < modules.length; i++) {
      const mod = modules[i];
      const hasExports = mod.decls.some((d) => d.exported);
      for (const d of mod.decls) {
        if (d.name !== name) continue;
        if (i > 0 && hasExports && !d.exported) continue;
        results.push({ decl: d, module: mod });
      }
      // methods and fields are reachable through their container, not bare
      // names -- except method names, which member access wants to find.
      for (const d of mod.decls) {
        if (!d.methods) continue;
        for (const meth of d.methods) {
          if (meth.name === name) results.push({ decl: meth, module: mod, method: true });
        }
      }
    }
    return results;
  }

  /** All visible declarations from `entryPath` (for completion). */
  visibleDecls(entryPath, env) {
    const modules = this.closure(entryPath, env);
    const out = [];
    for (let i = 0; i < modules.length; i++) {
      const mod = modules[i];
      const hasExports = mod.decls.some((d) => d.exported);
      for (const d of mod.decls) {
        if (i > 0 && hasExports && !d.exported) continue;
        out.push({ decl: d, module: mod });
      }
    }
    return out;
  }
}

// --- in-function local resolution ----------------------------------------------

/**
 * Resolve a name at (line) inside `module` to a local declaration: a `var`
 * statement above it in the enclosing function, a parameter, or a range-for
 * induction variable. Returns { type, line, col } or null.
 */
function resolveLocal(module, name, atLine) {
  const fn = module.decls.find(
    (d) => (d.kind === 'function' || d.kind === 'kernel') && d.line <= atLine && atLine <= d.endLine
  );
  if (!fn) return null;

  for (const p of fn.params) {
    if (p.name === name) return { type: p.type, line: fn.line, col: 0, param: true };
  }

  const varRe = new RegExp(`\\bvar\\s+(${escapeRe(name)})\\s*:\\s*([^=;]+)`);
  for (let ln = atLine; ln >= fn.line; ln--) {
    const m = module.maskedLines[ln].match(varRe);
    if (m) return { type: m[2].trim(), line: ln, col: module.maskedLines[ln].indexOf(name, m.index) };
    const forM = module.maskedLines[ln].match(new RegExp(`\\bfor\\s+(${escapeRe(name)})\\s+in\\b`));
    if (forM) return { type: 'int64', line: ln, col: module.maskedLines[ln].indexOf(name, forM.index) };
  }
  return null;
}

function escapeRe(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * Find every word-boundary occurrence of `name` in a module's masked text.
 * @returns {{ line: number, col: number }[]}
 */
function findOccurrences(module, name) {
  const re = new RegExp(`\\b${escapeRe(name)}\\b`, 'g');
  const hits = [];
  for (let ln = 0; ln < module.maskedLines.length; ln++) {
    let m;
    re.lastIndex = 0;
    while ((m = re.exec(module.maskedLines[ln])) !== null) {
      hits.push({ line: ln, col: m.index });
    }
  }
  return hits;
}

/**
 * Walk back from (line, col) to describe the innermost unclosed call:
 * { name, argIndex, nameLine, nameCol } or null. Used by signature help.
 */
function enclosingCall(module, line, col) {
  let depth = 0;
  let commas = 0;
  for (let ln = line; ln >= Math.max(0, line - 40); ln--) {
    const text = module.maskedLines[ln];
    const start = ln === line ? Math.min(col, text.length) - 1 : text.length - 1;
    for (let c = start; c >= 0; c--) {
      const ch = text[c];
      if (ch === ')' || ch === ']') depth++;
      else if (ch === '(' || ch === '[') {
        if (depth > 0) { depth--; continue; }
        if (ch === '[') return null;
        const before = text.slice(0, c);
        const nameM = before.match(/([A-Za-z_][A-Za-z0-9_]*)\s*$/);
        if (!nameM) return null;
        return { name: nameM[1], argIndex: commas, nameLine: ln, nameCol: nameM.index };
      } else if (ch === ',' && depth === 0) commas++;
      else if (ch === ';' || ch === '{' || ch === '}') return null;
    }
  }
  return null;
}

// --- extern link-object discovery -----------------------------------------------

/**
 * Minimal COFF reader: the names of EXTERNAL symbols this object DEFINES
 * (storage class 2 with a real section number). Just enough to decide which
 * sibling objects satisfy a program's `extern ... = "symbol"` declarations.
 * Returns [] for anything unreadable or non-COFF.
 */
function coffDefinedSymbols(filePath) {
  let buffer;
  try {
    buffer = fs.readFileSync(filePath);
  } catch (_) {
    return [];
  }
  if (buffer.length < 20) return [];
  const symtabOffset = buffer.readUInt32LE(8);
  const symbolCount = buffer.readUInt32LE(12);
  if (symtabOffset === 0 || symbolCount === 0) return [];
  const stringTableOffset = symtabOffset + symbolCount * 18;
  if (stringTableOffset + 4 > buffer.length) return [];

  const names = [];
  for (let i = 0; i < symbolCount; i++) {
    const offset = symtabOffset + i * 18;
    if (offset + 18 > buffer.length) break;
    const sectionNumber = buffer.readInt16LE(offset + 12);
    const storageClass = buffer.readUInt8(offset + 16);
    const auxCount = buffer.readUInt8(offset + 17);
    if (storageClass === 2 /* IMAGE_SYM_CLASS_EXTERNAL */ && sectionNumber > 0) {
      let name;
      if (buffer.readUInt32LE(offset) === 0) {
        const strOffset = stringTableOffset + buffer.readUInt32LE(offset + 4);
        const end = buffer.indexOf(0, strOffset);
        name = end > strOffset ? buffer.toString('utf8', strOffset, end) : '';
      } else {
        name = buffer.toString('utf8', offset, offset + 8).replace(/\0+$/, '');
      }
      if (name) names.push(name);
    }
    i += auxCount; // skip auxiliary records
  }
  return names;
}

/**
 * Object files near the program that define linker symbols the program's
 * import closure declares via `extern ... = "symbol"` -- e.g. a hand-written
 * llm_kernels.o next to engine.mettle. These must be passed to the link
 * (--link-arg) or the build fails with unresolved externals.
 */
function findExternLinkObjects(index, entryPath, env) {
  const wanted = new Set();
  for (const mod of index.closure(entryPath, env)) {
    for (const d of mod.decls) {
      if (d.externName) wanted.add(d.externName);
    }
  }
  if (wanted.size === 0) return [];

  const dir = path.dirname(entryPath);
  let entries;
  try {
    entries = fs.readdirSync(dir);
  } catch (_) {
    return [];
  }
  const results = [];
  for (const entry of entries) {
    if (!/\.(o|obj)$/i.test(entry)) continue;
    const candidate = path.join(dir, entry);
    const defined = coffDefinedSymbols(candidate);
    if (defined.some((name) => wanted.has(name))) {
      results.push(candidate);
    }
  }
  return results;
}

module.exports = {
  maskNonCode,
  scanModule,
  resolveImport,
  ProjectIndex,
  resolveLocal,
  findOccurrences,
  enclosingCall,
  splitTopLevel,
  baseTypeName,
  DECORATORS,
  coffDefinedSymbols,
  findExternLinkObjects,
};
