/**
 * Mettle navigation and editing intelligence: definition, references, rename,
 * outline/workspace symbols, context-aware completion, signature help,
 * parameter-name inlay hints, import links, and CodeLens (run/build plus
 * per-function optimization stats when the report panel is live).
 *
 * All language understanding lives in analysis.js (pure node, offline-
 * tested); this file is the thin vscode adapter around it.
 */

const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const analysis = require('./analysis');
const explain = require('./explain');

const SELECTOR = { language: 'mettle' };

const KEYWORDS = [
  'function', 'method', 'kernel', 'var', 'struct', 'enum', 'trait', 'impl', 'where',
  'if', 'else', 'while', 'for', 'in', 'switch', 'case', 'default', 'match',
  'break', 'continue', 'return', 'defer', 'errdefer', 'import', 'import_str',
  'export', 'extern', 'new', 'this', 'asm', 'sizeof', 'alignof', 'static_assert',
  'true', 'false', 'dispatch',
];
const TYPES = [
  'int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64',
  'float32', 'float64', 'bool', 'void', 'string', 'cstring', 'Self',
];
const NON_RENAMABLE = new Set([...KEYWORDS, ...TYPES]);

/** Shared module cache; live buffers layered over disk. */
const index = new analysis.ProjectIndex();

// --- environment -------------------------------------------------------------

function searchEnv(document) {
  const cfg = vscode.workspace.getConfiguration('mettle');
  const filePath = document.uri.fsPath;
  const workspaceRoot =
    vscode.workspace.getWorkspaceFolder(document.uri)?.uri?.fsPath || path.dirname(filePath);

  let stdlibRoot = null;
  const configured = cfg.get('linter.stdlibPath', '');
  if (configured) {
    const p = path.isAbsolute(configured) ? configured : path.join(workspaceRoot, configured);
    if (fs.existsSync(p)) stdlibRoot = p;
  }
  if (!stdlibRoot) {
    let dir = path.resolve(path.dirname(filePath));
    for (let depth = 0; depth < 16; depth++) {
      const candidate = path.join(dir, 'stdlib');
      if (fs.existsSync(candidate)) { stdlibRoot = candidate; break; }
      const parent = path.dirname(dir);
      if (parent === dir) break;
      dir = parent;
    }
  }
  if (!stdlibRoot) {
    const candidate = path.join(workspaceRoot, 'stdlib');
    if (fs.existsSync(candidate)) stdlibRoot = candidate;
  }

  const includeDirs = (cfg.get('linter.extraIncludePaths', []) || [])
    .filter((p) => p && typeof p === 'string');
  return { stdlibRoot, workspaceRoot, includeDirs };
}

function liveModule(document) {
  index.overrideText(document.uri.fsPath, document.getText());
  return index.getModule(document.uri.fsPath);
}

function toLocation(modulePath, line, col, length) {
  return new vscode.Location(
    vscode.Uri.file(modulePath),
    new vscode.Range(line, col, line, col + (length || 0))
  );
}

// --- member access resolution --------------------------------------------------

const BUILTIN_STRING_MEMBERS = [
  { name: 'chars', type: 'cstring', detail: 'pointer to the string bytes (uint8*)' },
  { name: 'length', type: 'uint64', detail: 'byte count' },
];

/**
 * For `recv.` / `recv->` at (line, col-of-dot-end): resolve the receiver's
 * struct declaration. Single-link only (one identifier before the accessor);
 * chains would need real type checking.
 */
function resolveReceiverType(document, module, env, line, textBeforeAccessor) {
  const recvM = textBeforeAccessor.match(/([A-Za-z_][A-Za-z0-9_]*)\s*$/);
  if (!recvM) return null;
  const recv = recvM[1];

  let typeText = null;
  if (recv === 'this') {
    const fn = module.decls.find(
      (d) => (d.kind === 'function' || d.kind === 'kernel') && d.line <= line && line <= d.endLine
    );
    const thisParam = fn && fn.params.find((p) => p.name === 'this');
    if (thisParam) typeText = thisParam.type;
    if (!typeText) {
      // method inside a struct block: the container is the type
      for (const d of module.decls) {
        if (!d.methods) continue;
        const meth = d.methods.find((mm) => mm.line <= line && line <= mm.line + 200);
        if (meth && d.line <= line && line <= d.endLine) { typeText = d.name; break; }
      }
    }
  } else {
    const local = analysis.resolveLocal(module, recv, line);
    if (local) typeText = local.type;
    if (!typeText) {
      const hits = index.lookup(recv, document.uri.fsPath, env);
      const g = hits.find((h) => h.decl.kind === 'global');
      if (g) typeText = g.decl.returnType;
    }
  }
  if (!typeText) return null;

  const base = analysis.baseTypeName(typeText);
  if (!base) return null;
  if (base === 'string') return { builtin: 'string' };

  const typeHits = index.lookup(base, document.uri.fsPath, env);
  const structHit = typeHits.find((h) => h.decl.kind === 'struct' || h.decl.kind === 'enum');
  return structHit ? { struct: structHit.decl, module: structHit.module } : null;
}

// --- definition ----------------------------------------------------------------

class MettleDefinitionProvider {
  provideDefinition(document, position) {
    const module = liveModule(document);
    if (!module) return null;
    const env = searchEnv(document);

    // import path under the cursor -> the resolved module file
    const imp = module.imports.find(
      (i) => i.line === position.line && position.character >= i.startCol && position.character <= i.endCol
    );
    if (imp) {
      const target = analysis.resolveImport(imp.raw, document.uri.fsPath, env, imp.isText);
      return target ? toLocation(target, 0, 0, 0) : null;
    }

    const wordRange = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
    if (!wordRange) return null;
    const word = document.getText(wordRange);

    // member access: jump to the field/method inside its struct
    const before = document.lineAt(position.line).text.slice(0, wordRange.start.character);
    if (/(\.|->)\s*$/.test(before)) {
      const recvText = before.replace(/(\.|->)\s*$/, '');
      const recv = resolveReceiverType(document, module, env, position.line, recvText);
      if (recv && recv.struct) {
        const field = (recv.struct.fields || []).find((f) => f.name === word);
        if (field) return toLocation(recv.module.path, field.line, field.col, word.length);
        const meth = (recv.struct.methods || []).find((mm) => mm.name === word);
        if (meth) return toLocation(recv.module.path, meth.line, meth.col, word.length);
      }
      // fall through: maybe a same-named top-level symbol
    }

    const local = analysis.resolveLocal(module, word, position.line);
    if (local && !(local.line === position.line && local.col === wordRange.start.character)) {
      return toLocation(document.uri.fsPath, local.line, Math.max(0, local.col), word.length);
    }

    const hits = index.lookup(word, document.uri.fsPath, env);
    return hits.map((h) => toLocation(h.module.path, h.decl.line, h.decl.col, word.length));
  }
}

// --- references / rename ---------------------------------------------------------

function collectProjectOccurrences(document, env, word) {
  const results = [];
  const entry = document.uri.fsPath;
  const modules = index.closure(entry, env);
  for (const mod of modules) {
    for (const occ of analysis.findOccurrences(mod, word)) {
      results.push({ path: mod.path, line: occ.line, col: occ.col });
    }
  }
  return results;
}

class MettleReferenceProvider {
  provideReferences(document, position) {
    const module = liveModule(document);
    if (!module) return null;
    const wordRange = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
    if (!wordRange) return null;
    const word = document.getText(wordRange);
    if (NON_RENAMABLE.has(word)) return null;
    const env = searchEnv(document);
    return collectProjectOccurrences(document, env, word)
      .map((o) => toLocation(o.path, o.line, o.col, word.length));
  }
}

class MettleRenameProvider {
  prepareRename(document, position) {
    const wordRange = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
    if (!wordRange) throw new Error('Place the cursor on an identifier to rename it.');
    const word = document.getText(wordRange);
    if (NON_RENAMABLE.has(word)) throw new Error(`\`${word}\` is a Mettle keyword or built-in type.`);
    const module = liveModule(document);
    const env = searchEnv(document);
    const local = module && analysis.resolveLocal(module, word, position.line);
    if (!local && index.lookup(word, document.uri.fsPath, env).length === 0) {
      throw new Error(`No declaration of \`${word}\` found in this file or its imports.`);
    }
    return wordRange;
  }

  provideRenameEdits(document, position, newName) {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(newName)) {
      throw new Error('The new name must be a valid Mettle identifier.');
    }
    if (NON_RENAMABLE.has(newName)) {
      throw new Error(`\`${newName}\` is a Mettle keyword or built-in type.`);
    }
    const wordRange = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
    if (!wordRange) return null;
    const word = document.getText(wordRange);
    const env = searchEnv(document);
    liveModule(document);

    const edit = new vscode.WorkspaceEdit();
    for (const o of collectProjectOccurrences(document, env, word)) {
      edit.replace(
        vscode.Uri.file(o.path),
        new vscode.Range(o.line, o.col, o.line, o.col + word.length),
        newName
      );
    }
    return edit;
  }
}

// --- document / workspace symbols --------------------------------------------------

const SYMBOL_KIND = {
  function: vscode.SymbolKind.Function,
  kernel: vscode.SymbolKind.Function,
  struct: vscode.SymbolKind.Struct,
  enum: vscode.SymbolKind.Enum,
  trait: vscode.SymbolKind.Interface,
  global: vscode.SymbolKind.Variable,
};

class MettleDocumentSymbolProvider {
  provideDocumentSymbols(document) {
    const module = liveModule(document);
    if (!module) return [];
    const symbols = [];
    for (const d of module.decls) {
      const fullRange = new vscode.Range(d.line, 0, d.endLine, 0);
      const nameRange = new vscode.Range(d.line, d.col, d.line, d.col + d.name.length);
      const detail =
        d.kind === 'function' || d.kind === 'kernel'
          ? `(${d.params.map((p) => p.type).join(', ')})${d.returnType && d.returnType !== 'void' ? ' -> ' + d.returnType : ''}`
          : d.kind === 'global' ? (d.returnType || '') : '';
      const sym = new vscode.DocumentSymbol(
        d.name, detail, SYMBOL_KIND[d.kind] || vscode.SymbolKind.Object, fullRange, nameRange
      );
      for (const f of d.fields || []) {
        sym.children.push(new vscode.DocumentSymbol(
          f.name, f.type, vscode.SymbolKind.Field,
          new vscode.Range(f.line, f.col, f.line, f.col + f.name.length),
          new vscode.Range(f.line, f.col, f.line, f.col + f.name.length)
        ));
      }
      for (const mm of d.methods || []) {
        sym.children.push(new vscode.DocumentSymbol(
          mm.name, `(${mm.params.map((p) => p.type).join(', ')})`, vscode.SymbolKind.Method,
          new vscode.Range(mm.line, mm.col, mm.line, mm.col + mm.name.length),
          new vscode.Range(mm.line, mm.col, mm.line, mm.col + mm.name.length)
        ));
      }
      for (const v of d.variants || []) {
        sym.children.push(new vscode.DocumentSymbol(
          v.name, v.payload ? `(${v.payload})` : '', vscode.SymbolKind.EnumMember,
          new vscode.Range(v.line, v.col, v.line, v.col + v.name.length),
          new vscode.Range(v.line, v.col, v.line, v.col + v.name.length)
        ));
      }
      symbols.push(sym);
    }
    return symbols;
  }
}

class MettleWorkspaceSymbolProvider {
  async provideWorkspaceSymbols(query) {
    const files = await vscode.workspace.findFiles('**/*.mettle', '**/{node_modules,obj,bin}/**', 800);
    const q = query.toLowerCase();
    const out = [];
    for (const uri of files) {
      const module = index.getModule(uri.fsPath);
      if (!module) continue;
      for (const d of module.decls) {
        if (q && !d.name.toLowerCase().includes(q)) continue;
        out.push(new vscode.SymbolInformation(
          d.name, SYMBOL_KIND[d.kind] || vscode.SymbolKind.Object,
          path.basename(uri.fsPath),
          toLocation(uri.fsPath, d.line, d.col, d.name.length)
        ));
        if (out.length >= 2000) return out;
      }
    }
    return out;
  }
}

// --- completion ------------------------------------------------------------------

function declCompletionItem(d) {
  const kindMap = {
    function: vscode.CompletionItemKind.Function,
    kernel: vscode.CompletionItemKind.Function,
    struct: vscode.CompletionItemKind.Struct,
    enum: vscode.CompletionItemKind.Enum,
    trait: vscode.CompletionItemKind.Interface,
    global: vscode.CompletionItemKind.Variable,
  };
  const item = new vscode.CompletionItem(d.name, kindMap[d.kind] || vscode.CompletionItemKind.Value);
  item.detail = d.signature;
  if (d.doc && d.doc.length > 0) item.documentation = d.doc.join('\n');
  if (d.kind === 'function' || d.kind === 'kernel') {
    item.insertText = new vscode.SnippetString(
      d.params.length > 0 ? `${d.name}($0)` : `${d.name}()`
    );
    if (d.params.length > 0) item.command = { command: 'editor.action.triggerParameterHints', title: '' };
  }
  return item;
}

class MettleCompletionProvider {
  provideCompletionItems(document, position) {
    const lineText = document.lineAt(position.line).text;
    const before = lineText.slice(0, position.character);
    const module = liveModule(document);
    if (!module) return [];
    const env = searchEnv(document);

    // decorators: `@` before a function
    const deco = before.match(/@(\w*)$/);
    if (deco) {
      return analysis.DECORATORS.map((name) => {
        const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Keyword);
        item.detail = `@${name}`;
        return item;
      });
    }

    // import path completion inside the string
    const imp = before.match(/^\s*(import|import_str)\s+"([^"]*)$/);
    if (imp) {
      return this.importPathItems(document, env, imp[2]);
    }

    // member access: fields/methods of the receiver's struct
    const member = before.match(/^(.*?)(\.|->)\s*(\w*)$/);
    if (member && !/^\s*import/.test(before)) {
      const recv = resolveReceiverType(document, module, env, position.line, member[1]);
      if (recv && recv.builtin === 'string') {
        return BUILTIN_STRING_MEMBERS.map((f) => {
          const item = new vscode.CompletionItem(f.name, vscode.CompletionItemKind.Field);
          item.detail = `${f.type} -- ${f.detail}`;
          return item;
        });
      }
      if (recv && recv.struct) {
        const items = [];
        for (const f of recv.struct.fields || []) {
          const item = new vscode.CompletionItem(f.name, vscode.CompletionItemKind.Field);
          item.detail = f.type;
          items.push(item);
        }
        for (const mm of recv.struct.methods || []) {
          const item = new vscode.CompletionItem(mm.name, vscode.CompletionItemKind.Method);
          item.detail = mm.signature;
          item.insertText = new vscode.SnippetString(`${mm.name}($0)`);
          items.push(item);
        }
        return items;
      }
      return [];
    }

    // general identifiers: locals + visible declarations + keywords + types
    const items = [];
    const seen = new Set();
    const fn = module.decls.find(
      (d) => (d.kind === 'function' || d.kind === 'kernel') && d.line <= position.line && position.line <= d.endLine
    );
    if (fn) {
      for (const p of fn.params) {
        if (seen.has(p.name)) continue;
        seen.add(p.name);
        const item = new vscode.CompletionItem(p.name, vscode.CompletionItemKind.Variable);
        item.detail = `parameter: ${p.type}`;
        items.push(item);
      }
      const varRe = /\bvar\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^=;]+)/g;
      for (let ln = fn.line; ln <= Math.min(position.line, fn.endLine); ln++) {
        let m;
        varRe.lastIndex = 0;
        while ((m = varRe.exec(module.maskedLines[ln])) !== null) {
          if (seen.has(m[1])) continue;
          seen.add(m[1]);
          const item = new vscode.CompletionItem(m[1], vscode.CompletionItemKind.Variable);
          item.detail = `local: ${m[2].trim()}`;
          items.push(item);
        }
      }
    }
    for (const { decl } of index.visibleDecls(document.uri.fsPath, env)) {
      if (seen.has(decl.name)) continue;
      seen.add(decl.name);
      items.push(declCompletionItem(decl));
      for (const v of decl.variants || []) {
        if (seen.has(v.name)) continue;
        seen.add(v.name);
        const item = new vscode.CompletionItem(v.name, vscode.CompletionItemKind.EnumMember);
        item.detail = `${decl.name}::${v.name}${v.payload ? `(${v.payload})` : ''}`;
        items.push(item);
      }
    }
    for (const kw of KEYWORDS) {
      if (seen.has(kw)) continue;
      items.push(new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword));
    }
    for (const t of TYPES) {
      if (seen.has(t)) continue;
      items.push(new vscode.CompletionItem(t, vscode.CompletionItemKind.TypeParameter));
    }
    return items;
  }

  importPathItems(document, env, typed) {
    const items = [];
    const addDir = (root, prefix) => {
      let entries;
      try {
        entries = fs.readdirSync(root, { withFileTypes: true });
      } catch (_) {
        return;
      }
      for (const e of entries) {
        if (e.isDirectory()) {
          const item = new vscode.CompletionItem(`${prefix}${e.name}/`, vscode.CompletionItemKind.Folder);
          item.sortText = `0${e.name}`;
          items.push(item);
        } else if (e.name.endsWith('.mettle')) {
          const stem = e.name.slice(0, -'.mettle'.length);
          const item = new vscode.CompletionItem(`${prefix}${stem}`, vscode.CompletionItemKind.Module);
          item.sortText = `1${stem}`;
          items.push(item);
        }
      }
    };

    // `std/...` completes against the stdlib; everything else against the
    // typed directory relative to the importing file.
    if (typed.startsWith('std/')) {
      if (env.stdlibRoot) {
        const sub = typed.slice(0, typed.lastIndexOf('/') + 1);
        addDir(path.join(env.stdlibRoot, sub), sub);
      }
      return items;
    }
    if (env.stdlibRoot && fs.existsSync(path.join(env.stdlibRoot, 'std'))) {
      items.push(Object.assign(new vscode.CompletionItem('std/', vscode.CompletionItemKind.Folder), { sortText: '0std' }));
    }
    const slash = typed.lastIndexOf('/');
    const sub = slash >= 0 ? typed.slice(0, slash + 1) : '';
    addDir(path.join(path.dirname(document.uri.fsPath), sub), sub);
    return items;
  }
}

// --- signature help -----------------------------------------------------------------

class MettleSignatureHelpProvider {
  provideSignatureHelp(document, position) {
    const module = liveModule(document);
    if (!module) return null;
    const call = analysis.enclosingCall(module, position.line, position.character);
    if (!call) return null;
    const env = searchEnv(document);

    const hits = index.lookup(call.name, document.uri.fsPath, env)
      .filter((h) => h.decl.kind === 'function' || h.decl.kind === 'kernel' || h.method);
    if (hits.length === 0) return null;

    const help = new vscode.SignatureHelp();
    for (const h of hits) {
      const d = h.decl;
      const label = d.signature ||
        `${d.name}(${d.params.map((p) => `${p.name}: ${p.type}`).join(', ')})` +
        (d.returnType && d.returnType !== 'void' ? ` -> ${d.returnType}` : '');
      const sig = new vscode.SignatureInformation(label);
      if (d.doc && d.doc.length > 0) sig.documentation = d.doc.join('\n');
      for (const p of d.params) {
        sig.parameters.push(new vscode.ParameterInformation(`${p.name}: ${p.type}`));
      }
      help.signatures.push(sig);
    }
    help.activeSignature = 0;
    help.activeParameter = Math.min(call.argIndex, Math.max(0, (hits[0].decl.params.length || 1) - 1));
    return help;
  }
}

// --- inlay hints (parameter names) ------------------------------------------------

function isLiteralArg(text) {
  const t = text.trim();
  return /^-?(0[xX][0-9a-fA-F]+|0[bB][01]+|\d+(\.\d+)?([eE][-+]?\d+)?|"[^"]*"|'[^']*'|true|false)$/.test(t);
}

class MettleInlayHintsProvider {
  provideInlayHints(document, range) {
    const mode = vscode.workspace.getConfiguration('mettle').get('hints.parameterNames', 'literals');
    if (mode === 'off') return [];
    const module = liveModule(document);
    if (!module) return [];
    const env = searchEnv(document);
    const hints = [];

    const callRe = /\b([A-Za-z_][A-Za-z0-9_]*)\s*\(/g;
    for (let ln = range.start.line; ln <= Math.min(range.end.line, module.maskedLines.length - 1); ln++) {
      const text = module.maskedLines[ln];
      let m;
      callRe.lastIndex = 0;
      while ((m = callRe.exec(text)) !== null) {
        const name = m[1];
        if (KEYWORDS.includes(name) || TYPES.includes(name)) continue;
        const openCol = m.index + m[0].length - 1;
        const close = matchParenSameLine(text, openCol);
        if (close < 0) continue; // multi-line calls: skip, keep this cheap

        const hits = index.lookup(name, document.uri.fsPath, env)
          .filter((h) => h.decl.kind === 'function' || h.decl.kind === 'kernel' || h.method);
        if (hits.length !== 1) continue; // ambiguous: no hints
        const params = hits[0].decl.params;
        if (params.length === 0) continue;

        const inner = text.slice(openCol + 1, close);
        const args = splitArgsWithOffsets(inner);
        for (let i = 0; i < args.length && i < params.length; i++) {
          const arg = args[i];
          if (arg.text.trim() === params[i].name) continue; // already self-documenting
          if (mode === 'literals' && !isLiteralArg(arg.text)) continue;
          const hint = new vscode.InlayHint(
            new vscode.Position(ln, openCol + 1 + arg.start + leadingSpaces(arg.text)),
            `${params[i].name}:`,
            vscode.InlayHintKind.Parameter
          );
          hint.paddingRight = true;
          hints.push(hint);
        }
      }
    }
    return hints;
  }
}

function matchParenSameLine(text, openCol) {
  let depth = 0;
  for (let c = openCol; c < text.length; c++) {
    if (text[c] === '(') depth++;
    else if (text[c] === ')') {
      depth--;
      if (depth === 0) return c;
    }
  }
  return -1;
}

function splitArgsWithOffsets(inner) {
  const args = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < inner.length; i++) {
    const ch = inner[i];
    if (ch === '(' || ch === '[' || ch === '<') depth++;
    else if (ch === ')' || ch === ']' || ch === '>') depth = Math.max(0, depth - 1);
    else if (ch === ',' && depth === 0) {
      args.push({ text: inner.slice(start, i), start });
      start = i + 1;
    }
  }
  if (inner.slice(start).trim().length > 0) args.push({ text: inner.slice(start), start });
  return args;
}

function leadingSpaces(s) {
  const m = s.match(/^\s*/);
  return m ? m[0].length : 0;
}

// --- import document links ------------------------------------------------------------

class MettleImportLinkProvider {
  provideDocumentLinks(document) {
    const module = liveModule(document);
    if (!module) return [];
    const env = searchEnv(document);
    const links = [];
    for (const imp of module.imports) {
      const target = analysis.resolveImport(imp.raw, document.uri.fsPath, env, imp.isText);
      if (!target) continue;
      const link = new vscode.DocumentLink(
        new vscode.Range(imp.line, imp.startCol, imp.line, imp.endCol),
        vscode.Uri.file(target)
      );
      link.tooltip = `Open ${path.basename(target)}`;
      links.push(link);
    }
    return links;
  }
}

// --- CodeLens: run/build + per-function optimization stats ------------------------------

class MettleCodeLensProvider {
  constructor() {
    this._onDidChange = new vscode.EventEmitter();
    this.onDidChangeCodeLenses = this._onDidChange.event;
    explain.onReportChanged(() => this._onDidChange.fire());
  }

  provideCodeLenses(document) {
    if (!vscode.workspace.getConfiguration('mettle').get('codeLens.enabled', true)) return [];
    const module = liveModule(document);
    if (!module) return [];
    const lenses = [];

    const main = module.decls.find((d) => d.kind === 'function' && d.name === 'main');
    if (main) {
      const range = new vscode.Range(main.line, 0, main.line, 0);
      lenses.push(new vscode.CodeLens(range, {
        title: 'Run', command: 'mettle.runFile', arguments: [document.uri],
      }));
      lenses.push(new vscode.CodeLens(range, {
        title: 'Debug', command: 'mettle.debugFile', arguments: [document.uri],
      }));
      lenses.push(new vscode.CodeLens(range, {
        title: 'Build', command: 'mettle.buildFile', arguments: [document.uri],
      }));
      lenses.push(new vscode.CodeLens(range, {
        title: 'Optimization Report', command: 'mettle.showOptimizationReport',
      }));
    }

    // Per-function loop stats from the live report (1-based remark lines).
    const { model, sourcePath } = explain.getReportState();
    if (model && sourcePath &&
        path.normalize(sourcePath).toLowerCase() === path.normalize(document.uri.fsPath).toLowerCase()) {
      for (const d of module.decls) {
        if (d.kind !== 'function' && d.kind !== 'kernel') continue;
        let vec = 0, scalar = 0, fixable = 0;
        for (const r of model.remarks) {
          if (r.kind !== 'loop' || r.line === null) continue;
          const line0 = r.line - 1;
          if (line0 < d.line || line0 > d.endLine) continue;
          if (r.positive) vec++;
          else {
            scalar++;
            if (r.applicable && r.verified) fixable++;
          }
        }
        if (vec + scalar === 0) continue;
        const parts = [];
        if (vec > 0) parts.push(`${vec} loop${vec === 1 ? '' : 's'} vectorized`);
        if (scalar > 0) parts.push(`${scalar} scalar`);
        if (fixable > 0) parts.push(`${fixable} verified fix${fixable === 1 ? '' : 'es'}`);
        lenses.push(new vscode.CodeLens(
          new vscode.Range(d.line, 0, d.line, 0),
          { title: parts.join(' · '), command: 'mettle.showOptimizationReport' }
        ));
      }
    }
    return lenses;
  }
}

// --- run / build -----------------------------------------------------------------------

let mettleTerminal = null;

function getTerminal() {
  if (!mettleTerminal || mettleTerminal.exitStatus !== undefined) {
    mettleTerminal = vscode.window.createTerminal('Mettle');
  }
  return mettleTerminal;
}

function quoted(p) {
  return /[ "']/.test(p) ? `"${p}"` : p;
}

/** Build the active (or given) file; returns the output path or null. */
async function buildFile(deps, uri, log) {
  const doc = uri
    ? await vscode.workspace.openTextDocument(uri)
    : vscode.window.activeTextEditor?.document;
  if (!doc || doc.languageId !== 'mettle') {
    vscode.window.showInformationMessage('Open a Mettle file to build it.');
    return null;
  }
  if (doc.isDirty) await doc.save();

  const filePath = doc.uri.fsPath;
  const workspaceRoot =
    vscode.workspace.getWorkspaceFolder(doc.uri)?.uri?.fsPath || path.dirname(filePath);
  const compiler = deps.findCompiler(workspaceRoot, filePath);
  const cfg = vscode.workspace.getConfiguration('mettle');
  const release = cfg.get('run.release', true);
  const extraArgs = (cfg.get('run.extraArgs', []) || []).filter((a) => typeof a === 'string');

  const exeName = path.basename(filePath, '.mettle') + (process.platform === 'win32' ? '.exe' : '');
  const outPath = path.join(path.dirname(filePath), exeName);

  const parts = [
    quoted(compiler), quoted(filePath), '-o', quoted(outPath), '--build',
    '-I', quoted(path.dirname(filePath)), '-I', quoted(workspaceRoot),
  ];
  if (release) parts.push('--release');
  for (const a of extraArgs) parts.push(quoted(a));

  // Hand-written kernel objects next to the source (extern ... = "symbol"):
  // pass any sibling .o/.obj that defines a declared extern symbol.
  const env = searchEnv(doc);
  index.overrideText(filePath, doc.getText());
  for (const obj of analysis.findExternLinkObjects(index, filePath, env)) {
    parts.push('--link-arg', quoted(obj));
    if (log) log(`[mettle] linking extern object: ${obj}`);
  }

  const terminal = getTerminal();
  terminal.show(true);
  terminal.sendText(parts.join(' '), true);
  if (log) log(`[mettle] build: ${parts.join(' ')}`);
  return outPath;
}

async function runFile(deps, uri, log) {
  const outPath = await buildFile(deps, uri, log);
  if (!outPath) return;
  const terminal = getTerminal();
  // The shell runs commands in order; the run line only matters if the build
  // produced the binary (the compiler's error output is right above it).
  terminal.sendText(
    process.platform === 'win32' ? `& ${quoted(outPath)}` : quoted(outPath),
    true
  );
}

// --- registration -------------------------------------------------------------------------

/**
 * @param {vscode.ExtensionContext} context
 * @param {{ findCompiler: (workspaceRoot: string, filePath: string) => string,
 *           log?: (line: string) => void }} deps
 */
function registerLanguageFeatures(context, deps) {
  context.subscriptions.push(
    vscode.languages.registerDefinitionProvider(SELECTOR, new MettleDefinitionProvider()),
    vscode.languages.registerReferenceProvider(SELECTOR, new MettleReferenceProvider()),
    vscode.languages.registerRenameProvider(SELECTOR, new MettleRenameProvider()),
    vscode.languages.registerDocumentSymbolProvider(SELECTOR, new MettleDocumentSymbolProvider()),
    vscode.languages.registerWorkspaceSymbolProvider(new MettleWorkspaceSymbolProvider()),
    vscode.languages.registerCompletionItemProvider(
      SELECTOR, new MettleCompletionProvider(), '.', '>', '@', '"', '/'
    ),
    vscode.languages.registerSignatureHelpProvider(
      SELECTOR, new MettleSignatureHelpProvider(), '(', ','
    ),
    vscode.languages.registerInlayHintsProvider(SELECTOR, new MettleInlayHintsProvider()),
    vscode.languages.registerDocumentLinkProvider(SELECTOR, new MettleImportLinkProvider()),
    vscode.languages.registerCodeLensProvider(SELECTOR, new MettleCodeLensProvider()),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (doc.languageId === 'mettle') index.clearOverride(doc.uri.fsPath);
    }),
    vscode.window.onDidCloseTerminal((t) => {
      if (t === mettleTerminal) mettleTerminal = null;
    })
  );

  return {
    runFile: (uri) => runFile(deps, uri, deps.log),
    buildFile: (uri) => buildFile(deps, uri, deps.log),
  };
}

/** Extern link objects for `program`, using the shared index and the same
 * search environment the navigation layer uses. For the debug adapter. */
function externLinkObjectsFor(program) {
  const env = searchEnv({ uri: vscode.Uri.file(program) });
  return analysis.findExternLinkObjects(index, program, env);
}

module.exports = {
  registerLanguageFeatures,
  externLinkObjectsFor,
  // exposed for the offline tests in scripts/test-language.js
  _test: {
    MettleDefinitionProvider,
    MettleReferenceProvider,
    MettleRenameProvider,
    MettleDocumentSymbolProvider,
    MettleCompletionProvider,
    MettleSignatureHelpProvider,
    MettleInlayHintsProvider,
    MettleImportLinkProvider,
    index,
  },
};
