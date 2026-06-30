/**
 * Offline tests for language.js: the navigation providers run against fake
 * vscode documents, with the `vscode` module stubbed (it only exists inside
 * the editor). Covers outline symbols, member completion, signature help,
 * definition, references, inlay hints, and import links.
 */

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const os = require('os');
const Module = require('module');

// --- vscode stub -----------------------------------------------------------------

class Position {
  constructor(line, character) { this.line = line; this.character = character; }
}
class Range {
  constructor(a, b, c, d) {
    if (typeof a === 'number') {
      this.start = new Position(a, b);
      this.end = new Position(c, d);
    } else {
      this.start = a;
      this.end = b;
    }
  }
}
class Location {
  constructor(uri, range) { this.uri = uri; this.range = range; }
}
class DocumentSymbol {
  constructor(name, detail, kind, range, selectionRange) {
    Object.assign(this, { name, detail, kind, range, selectionRange, children: [] });
  }
}
class CompletionItem {
  constructor(label, kind) { this.label = label; this.kind = kind; }
}
class SignatureHelp { constructor() { this.signatures = []; } }
class SignatureInformation {
  constructor(label) { this.label = label; this.parameters = []; }
}
class ParameterInformation { constructor(label) { this.label = label; } }
class InlayHint {
  constructor(position, label, kind) { Object.assign(this, { position, label, kind }); }
}
class DocumentLink {
  constructor(range, target) { this.range = range; this.target = target; }
}
class EventEmitter {
  constructor() { this.event = () => ({ dispose() {} }); }
  fire() {}
}

const vscodeStub = {
  window: { createTextEditorDecorationType: () => ({}) },
  ThemeColor: class { constructor(id) { this.id = id; } },
  OverviewRulerLane: { Right: 1 },
  workspace: {
    getConfiguration: () => ({ get: (_k, dflt) => dflt }),
    getWorkspaceFolder: () => null,
    onDidCloseTextDocument: () => ({ dispose() {} }),
  },
  languages: {},
  StatusBarAlignment: { Right: 2 },
  CodeActionKind: { QuickFix: 'quickfix' },
  SymbolKind: { Function: 11, Struct: 22, Enum: 9, Interface: 10, Variable: 12, Object: 18, Field: 7, Method: 5, EnumMember: 21 },
  CompletionItemKind: { Function: 2, Struct: 21, Enum: 12, Interface: 7, Variable: 5, Value: 11, Field: 4, Method: 1, Keyword: 13, TypeParameter: 24, Folder: 18, Module: 8, EnumMember: 19 },
  InlayHintKind: { Parameter: 2 },
  Position, Range, Location, DocumentSymbol, CompletionItem,
  SignatureHelp, SignatureInformation, ParameterInformation,
  InlayHint, DocumentLink, EventEmitter,
  SnippetString: class { constructor(value) { this.value = value; } },
  WorkspaceEdit: class {
    constructor() { this.edits = []; }
    replace(uri, range, newText) { this.edits.push({ uri, range, newText }); }
  },
  Uri: { file: (p) => ({ fsPath: p, toString: () => `file://${p}` }) },
  SymbolInformation: class {
    constructor(name, kind, containerName, location) {
      Object.assign(this, { name, kind, containerName, location });
    }
  },
  CodeLens: class { constructor(range, command) { this.range = range; this.command = command; } },
};

const originalResolve = Module._resolveFilename;
Module._resolveFilename = function (request, ...rest) {
  if (request === 'vscode') return 'vscode-stub';
  return originalResolve.call(this, request, ...rest);
};
require.cache['vscode-stub'] = {
  id: 'vscode-stub', filename: 'vscode-stub', loaded: true, exports: vscodeStub,
};

const { _test } = require(path.join(__dirname, '..', 'language.js'));

// --- fake document ----------------------------------------------------------------

function fakeDocument(filePath, text) {
  const lines = text.split(/\r?\n/);
  return {
    uri: vscodeStub.Uri.file(filePath),
    languageId: 'mettle',
    lineCount: lines.length,
    getText: (range) => {
      if (!range) return text;
      // single-line ranges only (all the providers use word ranges)
      return lines[range.start.line].slice(range.start.character, range.end.character);
    },
    lineAt: (line) => ({ text: lines[line] }),
    getWordRangeAtPosition: (pos, re) => {
      const lineText = lines[pos.line];
      const r = new RegExp(re.source, 'g');
      let m;
      while ((m = r.exec(lineText)) !== null) {
        if (m.index <= pos.character && pos.character <= m.index + m[0].length) {
          return new Range(pos.line, m.index, pos.line, m.index + m[0].length);
        }
      }
      return undefined;
    },
  };
}

// --- fixture on disk (import resolution is real) -------------------------------------

const SRC = `import "lib/geo";

struct Vec2 {
    x: float32;
    y: float32;
    method norm2(this: Vec2*) -> float32 {
        return this->x * this->x + this->y * this->y;
    }
}

fn blend(a: float32, b: float32, t: float32) -> float32 {
    return a + (b - a) * t;
}

fn main() -> int32 {
    var v: Vec2;
    v.x = 1.0;
    var r: float32 = blend(1.0, 2.0, 0.5);
    var s: float32 = area(3.0, 4.0);
    return 0;
}
`;
const GEO = `export fn area(w: float32, h: float32) -> float32 {
    return w * h;
}
`;

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mettle-language-test-'));
fs.mkdirSync(path.join(tmp, 'lib'));
const mainPath = path.join(tmp, 'main.mettle');
fs.writeFileSync(mainPath, SRC);
fs.writeFileSync(path.join(tmp, 'lib', 'geo.mettle'), GEO);

const doc = fakeDocument(mainPath, SRC);
const lines = SRC.split('\n');
const lineOf = (needle) => {
  const i = lines.findIndex((l) => l.includes(needle));
  assert.ok(i >= 0, `fixture line missing: ${needle}`);
  return i;
};

// --- document symbols -----------------------------------------------------------------

const symbols = new _test.MettleDocumentSymbolProvider().provideDocumentSymbols(doc);
assert.deepStrictEqual(symbols.map((s) => s.name), ['Vec2', 'blend', 'main']);
const vec2 = symbols[0];
assert.deepStrictEqual(vec2.children.map((c) => c.name), ['x', 'y', 'norm2']);
assert.strictEqual(symbols[1].detail, '(float32, float32, float32) -> float32');

// --- member completion: v. -> fields + methods of Vec2 -----------------------------------

const completion = new _test.MettleCompletionProvider();
const dotLine = lineOf('v.x = 1.0;');
const dotCol = lines[dotLine].indexOf('.') + 1;
const memberItems = completion.provideCompletionItems(doc, new Position(dotLine, dotCol));
const memberLabels = memberItems.map((i) => i.label);
assert.ok(memberLabels.includes('x') && memberLabels.includes('y') && memberLabels.includes('norm2'),
  `member completion lists Vec2 members, got: ${memberLabels.join(', ')}`);
assert.ok(!memberLabels.includes('blend'), 'member completion does not leak top-level names');

// general completion includes locals, file decls, and the imported exported fn
const genLine = lineOf('return 0;');
const genItems = completion.provideCompletionItems(doc, new Position(genLine, 4));
const genLabels = genItems.map((i) => i.label);
for (const expected of ['v', 'r', 'blend', 'Vec2', 'area', 'while', 'int32']) {
  assert.ok(genLabels.includes(expected), `general completion includes ${expected}`);
}

// --- signature help inside blend(...) -----------------------------------------------------

const sigLine = lineOf('blend(1.0, 2.0, 0.5)');
const sigCol = lines[sigLine].indexOf('2.0');
const help = new _test.MettleSignatureHelpProvider().provideSignatureHelp(doc, new Position(sigLine, sigCol));
assert.ok(help, 'signature help produced');
assert.strictEqual(help.signatures[0].parameters.length, 3);
assert.strictEqual(help.activeParameter, 1, 'cursor in second argument');

// --- definition: cross-file (area -> lib/geo.mettle) and member (v.x -> field) -----------------

const defProvider = new _test.MettleDefinitionProvider();
const areaLine = lineOf('area(3.0, 4.0)');
const areaCol = lines[areaLine].indexOf('area') + 1;
const areaDefs = defProvider.provideDefinition(doc, new Position(areaLine, areaCol));
assert.ok(Array.isArray(areaDefs) && areaDefs.length === 1, 'area definition found');
assert.ok(areaDefs[0].uri.fsPath.endsWith('geo.mettle'), 'area resolves into the imported module');

const fieldDef = defProvider.provideDefinition(
  doc, new Position(dotLine, lines[dotLine].indexOf('.x') + 1));
assert.ok(fieldDef && !Array.isArray(fieldDef), 'v.x resolves to a single field location');
assert.strictEqual(fieldDef.range.start.line, lineOf('x: float32;'), 'v.x jumps to the field');

// import path under cursor -> module file
const importDef = defProvider.provideDefinition(doc, new Position(0, 10));
assert.ok(importDef && importDef.uri.fsPath.endsWith('geo.mettle'), 'import path resolves');

// --- references across the closure ----------------------------------------------------------

const refs = new _test.MettleReferenceProvider().provideReferences(doc, new Position(areaLine, areaCol));
assert.strictEqual(refs.length, 2, 'area: declaration + one use across files');
const refFiles = new Set(refs.map((r) => path.basename(r.uri.fsPath)));
assert.ok(refFiles.has('main.mettle') && refFiles.has('geo.mettle'));

// --- rename produces edits in both files -------------------------------------------------------

const renameEdit = new _test.MettleRenameProvider().provideRenameEdits(
  doc, new Position(areaLine, areaCol), 'rect_area');
assert.strictEqual(renameEdit.edits.length, 2, 'rename touches both occurrences');

// --- inlay hints: literal args get parameter names ----------------------------------------------

const hints = new _test.MettleInlayHintsProvider().provideInlayHints(
  doc, new Range(0, 0, lines.length - 1, 0));
const hintLabels = hints.map((h) => h.label);
assert.ok(hintLabels.includes('a:') && hintLabels.includes('b:') && hintLabels.includes('t:'),
  `blend literal args hinted, got: ${hintLabels.join(' ')}`);
assert.ok(hintLabels.includes('w:') && hintLabels.includes('h:'), 'imported fn args hinted');

// --- import links --------------------------------------------------------------------------------

const links = new _test.MettleImportLinkProvider().provideDocumentLinks(doc);
assert.strictEqual(links.length, 1);
assert.ok(links[0].target.fsPath.endsWith('geo.mettle'));

// --- debug adapter: loads under the stub and answers initialize -------------------

const { MettleDebugAdapter } = require(path.join(__dirname, '..', 'debugAdapter.js'));
const adapter = new MettleDebugAdapter({ findCompiler: () => 'mettle.exe' });
const sentMessages = [];
adapter._onDidSendMessage.fire = (m) => sentMessages.push(m);
adapter.handleMessage({ seq: 1, type: 'request', command: 'initialize', arguments: {} });
assert.strictEqual(sentMessages.length, 1, 'initialize answered');
assert.strictEqual(sentMessages[0].command, 'initialize');
assert.ok(sentMessages[0].body.supportsSetVariable, 'advertises live variable writes');
assert.ok(sentMessages[0].body.supportsConfigurationDoneRequest);
adapter.handleMessage({ seq: 2, type: 'request', command: 'threads' });
assert.strictEqual(sentMessages[1].body.threads[0].name, 'main');

fs.rmSync(tmp, { recursive: true, force: true });
console.log(`test-language passed: ${symbols.length} symbols, ${memberItems.length} member items, ${hints.length} hints, ${refs.length} refs, debug adapter ok`);
