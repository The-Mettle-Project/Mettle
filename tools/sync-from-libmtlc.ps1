# Refresh this repository from a libmtlc checkout.
#
# Mettle and libmtlc are developed together in the libmtlc monorepo
# (https://github.com/The-Mettle-Project/libmtlc), which carries the backend
# AND the reference Mettle frontend. This repository is the Mettle half: the
# language, its frontend and driver, the standard library, docs, examples and
# tests. The backend arrives as a dependency (see get-libmtlc.ps1).
#
# This script copies the Mettle half out of a libmtlc checkout and rewrites the
# frontend's `#include "../..."` forms into include-path form, because in this
# repository the backend headers live under libmtlc/src rather than beside the
# frontend. That rewrite is the ONLY source difference between the two trees.
#
#   .\tools\sync-from-libmtlc.ps1 -LibmtlcDir ..\libmtlc
#   .\tools\sync-from-libmtlc.ps1 -LibmtlcDir ..\libmtlc -DryRun
#
# The file lists below are the frontend/backend boundary, in executable form.
# Adding a frontend translation unit to libmtlc means adding it here too.
[CmdletBinding()]
param(
  # A libmtlc checkout to copy from. Defaults to $env:LIBMTLC_SRC, then to a
  # sibling ..\libmtlc or ..\mettle-core.
  [string]$LibmtlcDir = $env:LIBMTLC_SRC,
  # Report what would change without writing anything.
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Say($m)   { Write-Host $m -ForegroundColor Blue }
function Ok($m)    { Write-Host "ok $m" -ForegroundColor Green }
function Warn($m)  { Write-Host "warning: $m" -ForegroundColor Yellow }

# --------------------------------------------------------------------------
# Locate the source checkout.
# --------------------------------------------------------------------------
if (-not $LibmtlcDir) {
  foreach ($cand in @("..\libmtlc", "..\mettle-core", "..\MettleToolchain")) {
    $p = Join-Path (Split-Path -Parent $root) (Split-Path -Leaf $cand)
    if (Test-Path (Join-Path $p "include\mtlc\mtlc.h")) { $LibmtlcDir = $p; break }
  }
}
if (-not $LibmtlcDir) {
  throw "no libmtlc checkout found. Pass -LibmtlcDir <path> or set `$env:LIBMTLC_SRC."
}
$LibmtlcDir = (Resolve-Path $LibmtlcDir).Path
foreach ($probe in @("include\mtlc\mtlc.h", "src\main.c", "src\ir\ir.h")) {
  if (-not (Test-Path (Join-Path $LibmtlcDir $probe))) {
    throw "$LibmtlcDir does not look like a libmtlc checkout (missing $probe)."
  }
}
Say "Syncing from $LibmtlcDir"

# --------------------------------------------------------------------------
# The Mettle frontend: every translation unit and header this repository owns.
# Everything else under src/ is libmtlc's and is NOT copied.
# --------------------------------------------------------------------------
$FrontendFiles = @(
  # Driver
  "src/main.c"
  "src/main.h"
  "src/mettle_alloc.c"
  "src/mettle_alloc.h"
  "src/tracy_build.c"
  "src/tracy_build.h"
  # Lexer
  "src/lexer/lexer.c"
  "src/lexer/lexer.h"
  # Parser and AST
  "src/parser/ast.c"
  "src/parser/ast.h"
  "src/parser/parser.c"
  "src/parser/parser.h"
  # Semantic analysis
  "src/semantic/import_resolver.c"
  "src/semantic/import_resolver.h"
  "src/semantic/monomorphize.c"
  "src/semantic/monomorphize.h"
  "src/semantic/register_allocator.c"
  "src/semantic/register_allocator.h"
  "src/semantic/symbol_table.c"
  "src/semantic/symbol_table.h"
  "src/semantic/type_checker.c"
  "src/semantic/type_checker.h"
  "src/semantic/type_checker_aggregate.c"
  "src/semantic/type_checker_decl.c"
  "src/semantic/type_checker_errors.c"
  "src/semantic/type_checker_expr.c"
  "src/semantic/type_checker_init_tracker.c"
  "src/semantic/type_checker_internal.h"
  "src/semantic/type_checker_match.c"
  "src/semantic/type_checker_memory.c"
  "src/semantic/type_checker_safety.c"
  "src/semantic/type_checker_stmt.c"
  "src/semantic/type_checker_tensor_epilogue.c"
  "src/semantic/type_checker_types.c"
  # AST -> IR lowering. A frontend concern: it consumes the AST and the
  # frontend type system, so libmtlc links it into the driver, not the archive.
  "src/ir/ir_lowering.c"
  "src/ir/ir_lowering.h"
  "src/ir/ir_lowering_internal.h"
  "src/ir/ir_lower_address.c"
  "src/ir/ir_lower_defer.c"
  "src/ir/ir_lower_expr.c"
  "src/ir/ir_lower_stmt.c"
  "src/ir/ir_lower_support.c"
  "src/ir/ir_lower_switch_match.c"
  "src/ir/ir_lower_types.c"
  # Frontend-to-backend adapters (frontend Type -> MtlcType, module tables)
  "src/frontend/mtlc_frontend.h"
  "src/frontend/mtlc_lower_module.c"
  "src/frontend/mtlc_lower_module.h"
  "src/frontend/mtlc_type_from_frontend.c"
  # Driver-side diagnostics rendering. error_reporter.c is frontend-neutral
  # and belongs to libmtlc; the --explain renderer is ours.
  "src/error/error_explain.c"
  "src/error/error_explain.h"
)

# Whole directories this repository owns, copied recursively.
#   src/runtime  the Mettle language runtime, linked into compiled programs
$FrontendDirs = @("src/runtime")

# --------------------------------------------------------------------------
# Everything outside src/: copied unless a rule below excludes it.
# --------------------------------------------------------------------------

# Paths that belong to libmtlc, or that this repository maintains itself.
$ExcludePrefixes = @(
  "include/"                    # libmtlc's public API, arrives via get-libmtlc
  "docs/libmtlc/"               # backend reference, lives in the libmtlc repo
  "examples/calc/"              # libmtlc's non-Mettle demo frontend
  "promotion/"                  # promo pipeline, not part of the language
  ".github/"                    # this repo has its own workflows
  ".idea/"
  ".elfwork/"
  "obj/"
  "bin/"
  "dist/"
)
$ExcludeFiles = @(
  # The backend's own architecture and embedding guides. docs/mettle-and-libmtlc.md
  # in this repo points at them instead of duplicating them.
  "docs/ARCHITECTURE.md"
  "docs/embedding.md"
  # Index pages that describe what a repository *is*. Upstream's say "this
  # repository is libmtlc plus the reference frontend" and point at
  # examples/calc, neither of which is true here.
  "docs/README.md"
  "examples/README.md"
  "tools/dist-libmtlc.ps1"      # packages the backend; libmtlc's job
  # One-off refactoring scripts that rewrite backend translation units. They
  # only make sense inside the libmtlc tree.
  "tools/assemble_ir_optimize.py"
  "tools/fix_binary_codegen.py"
  "tools/fix_ptr_fusion_order.py"
  "tools/regen_binary_header.py"
  "tools/split_binary_codegen.py"
  # The test runner is repo-local: it takes -LibmtlcDir, points the harnesses
  # that compile backend translation units at the dependency, and drops
  # libmtlc's own boundary audits (archive self-containment, the calc frontend).
  # New cases written upstream have to be ported by hand -- see the note the
  # sync prints at the end.
  "tests/run_tests.ps1"
  # Build and packaging files this repository defines for itself, because they
  # differ: ours build the frontend only and link the libmtlc dependency.
  "Makefile"
  "build.bat"
  "install.ps1"
  "install.sh"
  "get-libmtlc.ps1"
  "get-libmtlc.sh"
  "get-libmtlc-docs.ps1"
  "README.md"
  "CONTRIBUTING.md"
  ".gitignore"
  ".gitattributes"
)

# --------------------------------------------------------------------------
# Include rewrites applied to every copied .c/.h under src/ and tests/.
#
# libmtlc compiles the frontend with -Isrc, so a frontend file can reach a
# backend header with a relative "../ir/ir.h", and a test harness can reach one
# with "../src/compiler/compiler_context.h". Here the backend lives in
# libmtlc/src, and a relative include only ever looks beside the including file
# -- which is our tree. Turning them into include-path lookups fixes that: they
# resolve against -Isrc first (our own headers) and then -Ilibmtlc/src (the
# backend's).
#
# Order matters: the "../src/" rule has to run before the general "../" one.
# --------------------------------------------------------------------------
$IncludeRewrites = @(
  # tests/*.c reaching into the compiler: "../src/compiler/x.h" -> "compiler/x.h"
  @{ Pattern = '(?m)^(\s*#\s*include\s*")\.\./src/'; Replacement = '$1' }
  # src/*: "../ir/ir.h" -> "ir/ir.h"
  @{ Pattern = '(?m)^(\s*#\s*include\s*")\.\./'; Replacement = '$1' }
  # ir_lowering.h sits beside the backend's ir.h in libmtlc's tree and includes
  # it by bare name; here it has to name the directory.
  @{ Pattern = '(?m)^(\s*#\s*include\s*")ir\.h(")'; Replacement = '$1ir/ir.h$2' }
)

# Markdown links into backend source. Upstream they are relative paths that
# resolve inside its tree; here those files do not exist, so point them at the
# libmtlc repository instead. Only unambiguously-backend directories are
# rewritten: src/ir holds our lowering pass as well as the backend's IR, so it
# is deliberately not in this list.
$DocLinkRewrites = @(
  @{ Pattern = '\]\((?:\.\./)+src/(codegen|linker|compiler|debug)/([^)]+)\)'
     Replacement = '](https://github.com/The-Mettle-Project/libmtlc/blob/main/src/$1/$2)' }
)

# --------------------------------------------------------------------------
# Copy helpers. Preserve the byte content exactly apart from the rewrites, and
# preserve a UTF-8 BOM if the source had one: some sources in this tree mix
# CRLF and LF, and a blanket re-encode would explode the diff.
# --------------------------------------------------------------------------
$script:copied = 0
$script:rewritten = 0
$script:unchanged = 0
$script:sha = [Security.Cryptography.SHA256]::Create()

function Digest([byte[]]$b) { [BitConverter]::ToString($script:sha.ComputeHash($b)) }

function Copy-Synced([string]$rel) {
  $srcPath = Join-Path $LibmtlcDir ($rel -replace '/', '\')
  $dstPath = Join-Path $root ($rel -replace '/', '\')
  if (-not (Test-Path $srcPath)) { Warn "missing in source: $rel"; return }

  $bytes = [IO.File]::ReadAllBytes($srcPath)
  $isSource = ($rel -like "src/*" -or $rel -like "tests/*") -and
              ($rel -like "*.c" -or $rel -like "*.h")
  $isDoc = $rel -like "*.md"
  $rules = @()
  if ($isSource) { $rules = $IncludeRewrites }
  elseif ($isDoc) { $rules = $DocLinkRewrites }

  if ($rules.Count -gt 0) {
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $text = [Text.Encoding]::UTF8.GetString($bytes)
    if ($hasBom) { $text = $text.Substring(1) }
    $before = $text
    foreach ($r in $rules) {
      $text = [Regex]::Replace($text, $r.Pattern, $r.Replacement)
    }
    if ($text -ne $before) { $script:rewritten++ }
    # UTF8Encoding's BOM flag only affects GetPreamble() and StreamWriter --
    # GetBytes() never emits it -- so put the preamble back by hand. Without
    # this the sync silently strips the BOM from every file that has one, and
    # the two trees then differ by more than the include rewrite.
    $enc = New-Object Text.UTF8Encoding($false)
    $body = $enc.GetBytes($text)
    if ($hasBom) {
      $bytes = [byte[]]::new($body.Length + 3)
      [Array]::Copy([byte[]](0xEF, 0xBB, 0xBF), 0, $bytes, 0, 3)
      [Array]::Copy($body, 0, $bytes, 3, $body.Length)
    } else {
      $bytes = $body
    }
  }

  if (Test-Path $dstPath) {
    $old = [IO.File]::ReadAllBytes($dstPath)
    if ($old.Length -eq $bytes.Length -and (Digest $old) -eq (Digest $bytes)) {
      $script:unchanged++
      return
    }
  }
  if ($DryRun) { Write-Host "  would write $rel"; $script:copied++; return }
  $dir = Split-Path -Parent $dstPath
  if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
  [IO.File]::WriteAllBytes($dstPath, $bytes)
  $script:copied++
}

# --------------------------------------------------------------------------
# Build the file universe from git: tracked files plus new-but-not-ignored
# ones, which is the real source tree and excludes every build artifact.
# --------------------------------------------------------------------------
Push-Location $LibmtlcDir
try {
  $universe = & git ls-files --cached --others --exclude-standard
  if ($LASTEXITCODE -ne 0) { throw "git ls-files failed in $LibmtlcDir" }
} finally { Pop-Location }
Say "$($universe.Count) source files in the libmtlc tree"

$frontendSet = [Collections.Generic.HashSet[string]]::new(
  [string[]]$FrontendFiles, [StringComparer]::OrdinalIgnoreCase)

$skippedBackend = 0
foreach ($rel in $universe) {
  $rel = $rel -replace '\\', '/'

  if ($rel -like "src/*") {
    # Under src/, copy only what the manifest claims. Everything else is the
    # backend and comes from the libmtlc dependency.
    $isDir = $false
    foreach ($d in $FrontendDirs) { if ($rel -like "$d/*") { $isDir = $true; break } }
    if ($frontendSet.Contains($rel) -or $isDir) { Copy-Synced $rel } else { $skippedBackend++ }
    continue
  }

  $skip = $false
  foreach ($p in $ExcludePrefixes) { if ($rel -like "$p*") { $skip = $true; break } }
  if (-not $skip) { foreach ($f in $ExcludeFiles) { if ($rel -ieq $f) { $skip = $true; break } } }
  if ($skip) { continue }

  Copy-Synced $rel
}

# Anything the manifest names but git did not list (e.g. a file the source
# checkout has not staged) is worth shouting about rather than silently missing.
foreach ($rel in $FrontendFiles) {
  if ($universe -notcontains ($rel -replace '/', '\') -and $universe -notcontains $rel) {
    Warn "$rel is in the manifest but not in the libmtlc tree's file list"
  }
}

Write-Host ""
Ok "$script:copied written, $script:unchanged already current, $script:rewritten had includes rewritten"
Say "$skippedBackend backend files skipped (they come from the libmtlc dependency)"
if ($DryRun) { Warn "dry run: nothing was written" }

# tests/run_tests.ps1 is deliberately not synced, so say so rather than letting
# new upstream cases go missing silently.
$ours = Join-Path $root "tests\run_tests.ps1"
$theirs = Join-Path $LibmtlcDir "tests\run_tests.ps1"
if ((Test-Path $ours) -and (Test-Path $theirs)) {
  $oursN = (Get-Content $ours).Count
  $theirsN = (Get-Content $theirs).Count
  Write-Host ""
  Say ("tests\run_tests.ps1 is repo-local and was NOT synced " +
       "(ours $oursN lines, libmtlc's $theirsN). Port new cases by hand:")
  Write-Host "  git -C `"$LibmtlcDir`" log --oneline -- tests/run_tests.ps1"
}

Write-Host ""
Write-Host "Next:"
Write-Host "  .\get-libmtlc.ps1     # fetch/refresh the backend"
Write-Host "  .\build.bat           # build the driver against it"
