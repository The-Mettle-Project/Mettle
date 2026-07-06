/**
 * Offline tests for analysis.js (the symbol indexer behind navigation).
 * Plain node, no vscode dependency. Run via `npm run check`.
 */

const assert = require('assert');
const path = require('path');
const fs = require('fs');
const os = require('os');
const analysis = require('../analysis');

// --- fixture -------------------------------------------------------------------

const MAIN_SRC = `// A test module exercising every declaration shape the indexer knows.
import "std/io";
import "lib/vec";

/// Scales a value by an amount.
/// Two doc lines.
@inline fn scale(value: int32, amount: int32) -> int32 {
    return value * amount;
}

export fn entry(a: float32*, n: int64) -> float32 {
    var total: float32 = 0.0;
    var i: int64 = 0;
    while (i < n) {
        total = total + a[i];
        i = i + 1;
    }
    // "fn fake_in_string" inside a comment must not index
    return total;
}

extern fn puts(msg: cstring) -> int32 = "puts";

struct Point {
    x: int32;
    y: int32;
    method dist2(this: Point*) -> int32 {
        return this->x * this->x + this->y * this->y;
    }
}

enum Shape {
    Circle(int32),
    Square,
}

trait Order {
    fn less(self: Self, other: Self) -> bool;
}

export var GRID_W: int32 = 64;
extern var errno_value: int32 = "errno";

fn main() -> int32 {
    var p: Point* = new Point;
    var s: string = "fn not_a_decl() {}";
    var d: int32 = scale(GRID_W, 3);
    for k in 0..10 {
        d = d + k;
    }
    return d - d;
}
`;

const LIB_SRC = `export fn vec_add(a: float32*, b: float32*, n: int64) {
    var i: int64 = 0;
    while (i < n) { a[i] = a[i] + b[i]; i = i + 1; }
}

fn internal_helper() -> int32 {
    return 1;
}
`;

// Materialize on disk so import resolution and the closure walk run for real.
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mettle-analysis-test-'));
const libDir = path.join(tmp, 'lib');
fs.mkdirSync(libDir);
const mainPath = path.join(tmp, 'main.mettle');
const libPath = path.join(libDir, 'vec.mettle');
fs.writeFileSync(mainPath, MAIN_SRC);
fs.writeFileSync(libPath, LIB_SRC);

const env = { stdlibRoot: null, workspaceRoot: tmp, includeDirs: [] };
const lines = MAIN_SRC.split('\n');
const lineOf = (needle) => {
  const i = lines.findIndex((l) => l.includes(needle));
  assert.ok(i >= 0, `fixture line not found: ${needle}`);
  return i;
};

// --- scanModule ------------------------------------------------------------------

const mod = analysis.scanModule(MAIN_SRC, mainPath);

assert.strictEqual(mod.imports.length, 2, 'two imports');
assert.strictEqual(mod.imports[0].raw, 'std/io');
assert.strictEqual(mod.imports[1].raw, 'lib/vec');

const byName = new Map(mod.decls.map((d) => [d.name, d]));
assert.ok(!byName.has('fake_in_string'), 'no decls from comments');
assert.ok(!byName.has('not_a_decl'), 'no decls from strings');

const scale = byName.get('scale');
assert.ok(scale, 'scale indexed');
assert.strictEqual(scale.kind, 'function');
assert.strictEqual(scale.params.length, 2);
assert.deepStrictEqual(scale.params[0], { name: 'value', type: 'int32' });
assert.strictEqual(scale.returnType, 'int32');
assert.strictEqual(scale.doc.length, 2, 'two /// doc lines');
assert.strictEqual(scale.line, lineOf('@inline fn scale'));

const entry = byName.get('entry');
assert.ok(entry.exported, 'entry exported');
assert.strictEqual(entry.endLine, lineOf('return total;') + 1, 'entry block end');

const puts = byName.get('puts');
assert.strictEqual(puts.externName, 'puts', 'extern symbol name');

const point = byName.get('Point');
assert.strictEqual(point.kind, 'struct');
assert.deepStrictEqual(point.fields.map((f) => f.name), ['x', 'y']);
assert.strictEqual(point.fields[0].type, 'int32');
assert.strictEqual(point.methods.length, 1);
assert.strictEqual(point.methods[0].name, 'dist2');
assert.strictEqual(point.methods[0].container, 'Point');

const shape = byName.get('Shape');
assert.strictEqual(shape.kind, 'enum');
assert.deepStrictEqual(shape.variants.map((v) => v.name), ['Circle', 'Square']);
assert.strictEqual(shape.variants[0].payload, 'int32');

assert.strictEqual(byName.get('Order').kind, 'trait');
assert.strictEqual(byName.get('GRID_W').kind, 'global');
assert.ok(byName.get('GRID_W').exported);
assert.strictEqual(byName.get('errno_value').externName, 'errno');

// --- locals ----------------------------------------------------------------------

const inEntry = lineOf('total = total + a[i];');
const totalLocal = analysis.resolveLocal(mod, 'total', inEntry);
assert.ok(totalLocal, 'local total resolves');
assert.strictEqual(totalLocal.type, 'float32');

const paramLocal = analysis.resolveLocal(mod, 'n', inEntry);
assert.ok(paramLocal && paramLocal.param, 'param n resolves as a param');
assert.strictEqual(paramLocal.type, 'int64');

const rangeIv = analysis.resolveLocal(mod, 'k', lineOf('d = d + k;'));
assert.ok(rangeIv, 'range-for induction variable resolves');
assert.strictEqual(rangeIv.type, 'int64');

assert.strictEqual(analysis.resolveLocal(mod, 'total', lineOf('var d: int32')), null,
  'locals do not leak across functions');

// --- enclosing call (signature help) -------------------------------------------------

const callLine = lineOf('var d: int32 = scale(GRID_W, 3);');
const callText = lines[callLine];
const cursor = callText.indexOf(', 3') + 2; // inside the second argument
const call = analysis.enclosingCall(mod, callLine, cursor);
assert.ok(call, 'enclosing call found');
assert.strictEqual(call.name, 'scale');
assert.strictEqual(call.argIndex, 1, 'cursor in the second argument');

// --- project index: closure, visibility, lookup -----------------------------------------

const index = new analysis.ProjectIndex();
const closure = index.closure(mainPath, env);
assert.strictEqual(closure.length, 2, 'main + lib/vec (std/io unresolved without a stdlib)');

const vecHits = index.lookup('vec_add', mainPath, env);
assert.strictEqual(vecHits.length, 1, 'exported import-closure symbol visible');
assert.strictEqual(path.basename(vecHits[0].module.path), 'vec.mettle');

assert.strictEqual(index.lookup('internal_helper', mainPath, env).length, 0,
  'non-exported symbol in an exporting module is invisible to importers');

const methodHits = index.lookup('dist2', mainPath, env);
assert.strictEqual(methodHits.length, 1, 'methods findable by name');
assert.ok(methodHits[0].method);

// live-buffer override wins over disk
index.overrideText(mainPath, MAIN_SRC + '\nfn added_live() -> int32 { return 0; }\n');
assert.strictEqual(index.lookup('added_live', mainPath, env).length, 1, 'override text indexed');
index.clearOverride(mainPath);
assert.strictEqual(index.lookup('added_live', mainPath, env).length, 0, 'override cleared');

// --- occurrences (references/rename) ----------------------------------------------------

const occ = analysis.findOccurrences(mod, 'GRID_W');
assert.strictEqual(occ.length, 2, 'declaration + one use');

// `total` in a comment/string must not count
const totalOcc = analysis.findOccurrences(mod, 'total');
for (const o of totalOcc) {
  assert.ok(!lines[o.line].slice(0, o.col).includes('//') || lines[o.line].trim().startsWith('total'),
    'occurrence not inside a comment');
}

// --- import resolution -------------------------------------------------------------------

assert.strictEqual(
  analysis.resolveImport('lib/vec', mainPath, env, false), libPath, 'workspace-relative import');
assert.strictEqual(
  analysis.resolveImport('std/io', mainPath, env, false), null, 'std import without stdlib root');
assert.strictEqual(
  analysis.resolveImport('std/io', mainPath, { ...env, stdlibRoot: tmp }, false), null,
  'std import with stdlib root but missing file');

// --- COFF reader + extern link-object discovery ---------------------------------------

/** Handcraft a minimal x64 COFF object defining the given external symbols. */
function makeCoff(definedNames) {
  const header = Buffer.alloc(20);
  header.writeUInt16LE(0x8664, 0); // machine
  header.writeUInt16LE(0, 2);      // no sections (parser only reads symbols)
  const symtabOffset = 20;
  header.writeUInt32LE(symtabOffset, 8);
  header.writeUInt32LE(definedNames.length, 12);

  let strings = Buffer.alloc(4); // string table size patched below
  const symbols = [];
  for (const name of definedNames) {
    const sym = Buffer.alloc(18);
    if (name.length <= 8) {
      sym.write(name, 0, 'utf8');
    } else {
      sym.writeUInt32LE(0, 0);
      sym.writeUInt32LE(strings.length, 4);
      strings = Buffer.concat([strings, Buffer.from(name + '\0', 'utf8')]);
    }
    sym.writeInt16LE(1, 12);  // section 1 (defined)
    sym.writeUInt8(2, 16);    // IMAGE_SYM_CLASS_EXTERNAL
    sym.writeUInt8(0, 17);    // no aux records
    symbols.push(sym);
  }
  strings.writeUInt32LE(strings.length, 0);
  return Buffer.concat([header, ...symbols, strings]);
}

const kernelObj = path.join(tmp, 'kernels.o');
const unrelatedObj = path.join(tmp, 'other.obj');
fs.writeFileSync(kernelObj, makeCoff(['gemv_q4k_avx2', 'axpy']));
fs.writeFileSync(unrelatedObj, makeCoff(['something_else']));
fs.writeFileSync(path.join(tmp, 'garbage.o'), Buffer.from('not coff'));

assert.deepStrictEqual(
  analysis.coffDefinedSymbols(kernelObj).sort(), ['axpy', 'gemv_q4k_avx2'],
  'COFF defined symbols parsed (short + long names)');
assert.deepStrictEqual(analysis.coffDefinedSymbols(path.join(tmp, 'garbage.o')), [],
  'non-COFF input tolerated');

const externMain = path.join(tmp, 'kernel_user.mettle');
fs.writeFileSync(externMain,
  'extern fn gemv(base: uint8*, n: int64) -> int32 = "gemv_q4k_avx2";\n' +
  'fn main() -> int32 { return gemv(0, 0); }\n');
const linkObjs = analysis.findExternLinkObjects(new analysis.ProjectIndex(), externMain, env);
assert.deepStrictEqual(linkObjs, [kernelObj],
  'only the object defining a declared extern symbol is selected');

// cleanup
fs.rmSync(tmp, { recursive: true, force: true });

console.log(`test-analysis passed: ${mod.decls.length} decls, ${mod.imports.length} imports, closure ${closure.length} modules`);
