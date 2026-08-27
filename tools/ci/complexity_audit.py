#!/usr/bin/env python3

import argparse
import bisect
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUDGET_PATH = os.path.join(ROOT, "tools", "ci", "complexity-budget.json")
DEFAULT_ROOTS = ("src", "include")
DEFAULT_CEILING = 40
DEFAULT_ALARM = 120
ISSUE_LABEL = "complexity-budget"
ISSUE_MARKER = "<!-- mettle-complexity-audit -->"

TOKEN_RE = re.compile(r"[A-Za-z_]\w*|&&|\|\||[?{}();]")
IDENTIFIER_RE = re.compile(r"^[A-Za-z_]\w*$")

C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
    "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
    "void", "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool",
    "_Complex", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local",
}

DECLARATOR_NOISE = {"__attribute__", "__declspec", "__asm", "__asm__", "asm"}

BRANCH_KEYWORDS = {"if", "for", "while"}
SHORT_CIRCUIT = {"&&", "||", "?"}


def blank_span(chars, source, start, end):
    for index in range(start, end):
        if source[index] != "\n":
            chars[index] = " "


def blank_noise(source):
    chars = list(source)
    length = len(source)
    index = 0
    at_line_start = True
    while index < length:
        char = source[index]
        if char == "/" and index + 1 < length and source[index + 1] == "*":
            end = source.find("*/", index + 2)
            end = length if end < 0 else end + 2
            blank_span(chars, source, index, end)
            index = end
            at_line_start = False
            continue
        if char == "/" and index + 1 < length and source[index + 1] == "/":
            end = source.find("\n", index)
            end = length if end < 0 else end
            blank_span(chars, source, index, end)
            index = end
            continue
        if char == '"' or char == "'":
            end = index + 1
            while end < length:
                if source[end] == "\\":
                    end += 2
                    continue
                if source[end] == char:
                    end += 1
                    break
                if source[end] == "\n":
                    break
                end += 1
            blank_span(chars, source, index, end)
            index = end
            at_line_start = False
            continue
        if char == "#" and at_line_start:
            end = index
            while end < length:
                line_end = source.find("\n", end)
                if line_end < 0:
                    end = length
                    break
                if source[end:line_end].rstrip("\r").endswith("\\"):
                    end = line_end + 1
                    continue
                end = line_end
                break
            blank_span(chars, source, index, end)
            index = end
            continue
        if char == "\n":
            at_line_start = True
        elif not char.isspace():
            at_line_start = False
        index += 1
    return "".join(chars)


def matching_close_brace(tokens, open_index):
    depth = 0
    for index in range(open_index, len(tokens)):
        text = tokens[index][0]
        if text == "{":
            depth += 1
        elif text == "}":
            depth -= 1
            if depth == 0:
                return index
    return None


def matching_open_paren(tokens, close_index, floor_index):
    depth = 0
    for index in range(close_index, floor_index - 1, -1):
        text = tokens[index][0]
        if text == ")":
            depth += 1
        elif text == "(":
            depth -= 1
            if depth == 0:
                return index
    return None


def function_name(tokens, statement_start, brace_index):
    cursor = brace_index - 1
    while cursor > statement_start:
        if tokens[cursor][0] != ")":
            return None
        open_index = matching_open_paren(tokens, cursor, statement_start)
        if open_index is None or open_index - 1 < statement_start:
            return None
        candidate = tokens[open_index - 1][0]
        if candidate in DECLARATOR_NOISE:
            cursor = open_index - 2
            continue
        if candidate == ")":
            cursor = open_index - 1
            continue
        if not IDENTIFIER_RE.match(candidate) or candidate in C_KEYWORDS:
            return None
        if open_index - 1 == statement_start:
            return None
        return candidate
    return None


def body_measurements(tokens, start_index, end_index):
    complexity = 1
    cases = 0
    switches = 0
    for index in range(start_index, end_index):
        text = tokens[index][0]
        if text == "case":
            complexity += 1
            cases += 1
        elif text in BRANCH_KEYWORDS:
            complexity += 1
        elif text == "switch":
            switches += 1
        elif text in SHORT_CIRCUIT:
            complexity += 1
    return complexity, cases, switches


def scan_source(relative_path, source):
    clean = blank_noise(source)
    newline_offsets = [match.start() for match in re.finditer("\n", clean)]
    tokens = [(match.group(0), match.start())
              for match in TOKEN_RE.finditer(clean)]

    def line_of(position):
        return bisect.bisect_right(newline_offsets, position) + 1

    functions = []
    unbalanced = False
    statement_start = 0
    index = 0
    count = len(tokens)
    while index < count:
        text = tokens[index][0]
        if text == ";":
            statement_start = index + 1
        elif text == "{":
            close_index = matching_close_brace(tokens, index)
            if close_index is None:
                unbalanced = True
                break
            name = function_name(tokens, statement_start, index)
            if name is not None:
                complexity, cases, switches = body_measurements(
                    tokens, index + 1, close_index)
                functions.append({
                    "file": relative_path,
                    "name": name,
                    "line": line_of(tokens[index][1]),
                    "end_line": line_of(tokens[close_index][1]),
                    "complexity": complexity,
                    "modified": complexity - cases + switches,
                    "cases": cases,
                    "switches": switches,
                })
            index = close_index + 1
            statement_start = index
            continue
        elif text == "}":
            unbalanced = True
            break
        index += 1
    return functions, unbalanced


def source_files(roots):
    paths = []
    for root in roots:
        absolute_root = os.path.join(ROOT, root)
        if os.path.isfile(absolute_root):
            paths.append(absolute_root)
            continue
        for directory, subdirectories, names in os.walk(absolute_root):
            subdirectories[:] = sorted(
                name for name in subdirectories if not name.startswith("."))
            for name in sorted(names):
                if name.endswith(".c") or name.endswith(".h"):
                    paths.append(os.path.join(directory, name))
    return paths


def relative(path):
    try:
        candidate = os.path.relpath(path, ROOT)
    except ValueError:
        return os.path.abspath(path).replace(os.sep, "/")
    if candidate.startswith(".." + os.sep) or candidate == "..":
        return os.path.abspath(path).replace(os.sep, "/")
    return candidate.replace(os.sep, "/")


def collect(roots):
    functions = []
    unbalanced_files = []
    files = source_files(roots)
    for path in files:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            source = handle.read()
        found, unbalanced = scan_source(relative(path), source)
        functions.extend(found)
        if unbalanced:
            unbalanced_files.append(relative(path))
    functions.sort(key=lambda item: (-item["complexity"], item["file"],
                                     item["line"]))
    return files, functions, unbalanced_files


def key_of(function):
    return function["file"] + "::" + function["name"]


def worst_by_key(functions):
    worst = {}
    for function in functions:
        key = key_of(function)
        if key not in worst or function["complexity"] > worst[key]["complexity"]:
            worst[key] = function
    return worst


def percentile(sorted_values, fraction):
    if not sorted_values:
        return 0
    position = int(round(fraction * (len(sorted_values) - 1)))
    return sorted_values[position]


BUCKETS = [(1, 5), (6, 10), (11, 20), (21, 30), (31, 40), (41, 60), (61, 100),
           (101, None)]


def distribution(values):
    rows = []
    for low, high in BUCKETS:
        count = sum(1 for value in values
                    if value >= low and (high is None or value <= high))
        label = "{}+".format(low) if high is None else "{}-{}".format(low, high)
        rows.append((label, count))
    return rows


def statistics(functions, ceiling, alarm):
    values = sorted(function["complexity"] for function in functions)
    total = len(values)
    return {
        "functions": total,
        "mean": (sum(values) / float(total)) if total else 0.0,
        "median": percentile(values, 0.50),
        "p90": percentile(values, 0.90),
        "p99": percentile(values, 0.99),
        "maximum": values[-1] if total else 0,
        "over_ceiling": sum(1 for value in values if value > ceiling),
        "over_alarm": sum(1 for value in values if value >= alarm),
        "branch_points": sum(value - 1 for value in values),
    }


def directory_weights(functions):
    weights = {}
    for function in functions:
        directory = os.path.dirname(function["file"]) or "."
        entry = weights.setdefault(directory, [0, 0, 0])
        entry[0] += 1
        entry[1] += function["complexity"] - 1
        entry[2] = max(entry[2], function["complexity"])
    return sorted(weights.items(), key=lambda item: -item[1][1])


def print_report(files, functions, unbalanced_files, ceiling, alarm, top):
    stats = statistics(functions, ceiling, alarm)
    print("Cyclomatic complexity audit")
    print("  files scanned          {}".format(len(files)))
    print("  functions measured     {}".format(stats["functions"]))
    if not stats["functions"]:
        return
    print("  mean                   {:.1f}".format(stats["mean"]))
    print("  median                 {}".format(stats["median"]))
    print("  90th percentile        {}".format(stats["p90"]))
    print("  99th percentile        {}".format(stats["p99"]))
    print("  maximum                {}".format(stats["maximum"]))
    print("  over the ceiling {:<5} {} ({:.1f}%)".format(
        ceiling, stats["over_ceiling"],
        100.0 * stats["over_ceiling"] / stats["functions"]))
    print("  past the alarm {:<7} {}".format(alarm, stats["over_alarm"]))
    print("")
    print("Distribution")
    rows = distribution([function["complexity"] for function in functions])
    widest = max(count for label, count in rows) or 1
    for label, count in rows:
        print("  {:>8}  {:>6}  {}".format(
            label, count, "#" * int(round(40.0 * count / widest))))
    print("")
    print("Heaviest functions")
    print("  {:>5}  {:>5}  {}".format("cc", "mcc", "function"))
    for function in functions[:top]:
        print("  {:>5}  {:>5}  {}:{} {}".format(
            function["complexity"], function["modified"], function["file"],
            function["line"], function["name"]))
    print("")
    print("Where the weight sits")
    print("  {:>6}  {:>8}  {:>5}  {}".format(
        "fns", "branches", "worst", "directory"))
    for directory, entry in directory_weights(functions)[:12]:
        print("  {:>6}  {:>8}  {:>5}  {}".format(
            entry[0], entry[1], entry[2], directory))
    if unbalanced_files:
        print("")
        print("Files the scanner could not brace-match: {}".format(
            len(unbalanced_files)))
        for path in unbalanced_files[:10]:
            print("  {}".format(path))


def load_budget():
    if not os.path.exists(BUDGET_PATH):
        return {"ceiling": DEFAULT_CEILING, "alarm": DEFAULT_ALARM,
                "roots": list(DEFAULT_ROOTS), "over_ceiling": {}}
    with open(BUDGET_PATH, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_budget(budget):
    with open(BUDGET_PATH, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(budget, handle, indent=2, sort_keys=True)
        handle.write("\n")


def build_budget(functions, ceiling, alarm, roots):
    recorded = {}
    for key, function in worst_by_key(functions).items():
        if function["complexity"] > ceiling:
            recorded[key] = function["complexity"]
    return {
        "ceiling": ceiling,
        "alarm": alarm,
        "roots": list(roots),
        "over_ceiling": recorded,
    }


def evaluate(functions, budget, unbalanced_files):
    ceiling = budget["ceiling"]
    alarm = budget.get("alarm", DEFAULT_ALARM)
    recorded = budget.get("over_ceiling", {})
    worst = worst_by_key(functions)

    introduced = []
    regressed = []
    stale = []
    for key in sorted(worst):
        function = worst[key]
        value = function["complexity"]
        if value <= ceiling:
            if key in recorded:
                stale.append((key, recorded[key], value, function))
            continue
        if key not in recorded:
            introduced.append((key, value, function))
        elif value > recorded[key]:
            regressed.append((key, recorded[key], value, function))
        elif value < recorded[key]:
            stale.append((key, recorded[key], value, function))
    for key in sorted(recorded):
        if key not in worst:
            stale.append((key, recorded[key], None, None))

    alarming = [entry for entry in introduced if entry[1] >= alarm]
    alarming += [entry for entry in regressed if entry[2] >= alarm]

    return {
        "ceiling": ceiling,
        "alarm": alarm,
        "introduced": introduced,
        "regressed": regressed,
        "stale": stale,
        "alarming": alarming,
        "unbalanced": unbalanced_files,
        "failing": bool(introduced or regressed or unbalanced_files),
    }


def describe(function, key):
    if function is None:
        return key
    return "{} ({}:{})".format(key, function["file"], function["line"])


def check(verdict):
    ceiling = verdict["ceiling"]
    if verdict["introduced"]:
        print("Functions over the ceiling of {} with no budget entry:".format(
            ceiling))
        for key, value, function in verdict["introduced"]:
            print("  cc {:>4}  {}".format(value, describe(function, key)))
        print("")
    if verdict["regressed"]:
        print("Budgeted functions that grew:")
        for key, before, after, function in verdict["regressed"]:
            print("  cc {:>4} -> {:<4}  {}".format(
                before, after, describe(function, key)))
        print("")
    if verdict["unbalanced"]:
        print("The scanner lost brace balance in {} file(s), so their "
              "functions went unmeasured:".format(len(verdict["unbalanced"])))
        for path in verdict["unbalanced"]:
            print("  {}".format(path))
        print("")
    if verdict["failing"]:
        print("Split the function, or record the debt with")
        print("  python3 tools/ci/complexity_audit.py --update-budget")
        print("")
    if verdict["stale"]:
        print("Budget entries that improved and can be tightened ({}):".format(
            len(verdict["stale"])))
        for key, before, after, function in verdict["stale"][:20]:
            target = "gone" if after is None else str(after)
            print("  cc {:>4} -> {:<4}  {}".format(
                before, target, describe(function, key)))
        if len(verdict["stale"]) > 20:
            print("  and {} more".format(len(verdict["stale"]) - 20))
        print("  Refresh with --update-budget. This does not fail the gate.")
        print("")
    if not verdict["failing"]:
        print("Complexity budget holds.")
    return 1 if verdict["failing"] else 0


def repository_context():
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    sha = os.environ.get("GITHUB_SHA", "")
    if not sha:
        try:
            sha = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT,
                stderr=subprocess.DEVNULL).decode().strip()
        except (OSError, subprocess.CalledProcessError):
            sha = ""
    return server, repository, sha


def source_link(function):
    server, repository, sha = repository_context()
    label = "`{}`".format(function["name"])
    if not repository or not sha:
        return "{} <br>`{}:{}`".format(label, function["file"],
                                       function["line"])
    return "[{}]({}/{}/blob/{}/{}#L{}-L{})".format(
        label, server, repository, sha, function["file"], function["line"],
        function["end_line"])


def markdown_table(header, alignment, rows):
    lines = ["| " + " | ".join(header) + " |",
             "|" + "|".join(alignment) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(str(cell) for cell in row) + " |")
    return "\n".join(lines)


def issue_title(verdict):
    alarming = len(verdict["alarming"])
    if alarming:
        return "Complexity alarm: {} function{} past cc {}".format(
            alarming, "" if alarming == 1 else "s", verdict["alarm"])
    breaches = len(verdict["introduced"]) + len(verdict["regressed"])
    if not breaches:
        return "Complexity gate could not measure {} file{}".format(
            len(verdict["unbalanced"]),
            "" if len(verdict["unbalanced"]) == 1 else "s")
    return "Complexity budget breached by {} function{}".format(
        breaches, "" if breaches == 1 else "s")


def issue_body(verdict, functions, files):
    server, repository, sha = repository_context()
    ceiling = verdict["ceiling"]
    alarm = verdict["alarm"]
    stats = statistics(functions, ceiling, alarm)
    parts = []

    if verdict["alarming"]:
        count = len(verdict["alarming"])
        parts.append(
            "The complexity gate on the default branch found **{} {} past "
            "the alarm line of {}**. A function that branchy holds more "
            "independent paths than a reader can carry, and more than a test "
            "suite covers by accident.".format(
                count, "function" if count == 1 else "functions", alarm))
    elif verdict["introduced"] or verdict["regressed"]:
        parts.append(
            "The complexity gate on the default branch found new debt "
            "against a ceiling of **{}** branches per function.".format(
                ceiling))
    else:
        parts.append(
            "The complexity gate lost brace balance while reading the tree, "
            "so the functions in the files listed below went unmeasured and "
            "the numbers here are incomplete.")

    if repository and sha:
        parts.append("Measured on [`{}`]({}/{}/commit/{}) over `{}`.".format(
            sha[:8], server, repository, sha,
            "`, `".join(verdict.get("roots", DEFAULT_ROOTS))))

    parts.append(markdown_table(
        ["Measure", "Value"], ["---", "---:"],
        [["Functions measured", "{:,}".format(stats["functions"])],
         ["Files scanned", "{:,}".format(files)],
         ["Median", stats["median"]],
         ["90th percentile", stats["p90"]],
         ["99th percentile", stats["p99"]],
         ["Worst function", stats["maximum"]],
         ["Over the ceiling of {}".format(ceiling), stats["over_ceiling"]],
         ["Past the alarm of {}".format(alarm), stats["over_alarm"]]]))

    if verdict["alarming"]:
        parts.append("### Past the alarm line")
        rows = []
        for entry in verdict["alarming"]:
            function = entry[-1]
            rows.append([entry[1] if len(entry) == 3 else entry[2],
                         function["modified"],
                         function["end_line"] - function["line"] + 1,
                         source_link(function)])
        parts.append(markdown_table(
            ["cc", "mcc", "lines", "Function"],
            ["---:", "---:", "---:", "---"], rows))

    alarmed = set(entry[0] for entry in verdict["alarming"])
    introduced = [entry for entry in verdict["introduced"]
                  if entry[0] not in alarmed]
    if introduced:
        parts.append("### New functions over the ceiling")
        rows = [[value, function["modified"],
                 function["end_line"] - function["line"] + 1,
                 source_link(function)]
                for key, value, function in introduced]
        parts.append(markdown_table(
            ["cc", "mcc", "lines", "Function"],
            ["---:", "---:", "---:", "---"], rows))

    regressed = [entry for entry in verdict["regressed"]
                 if entry[0] not in alarmed]
    if regressed:
        parts.append("### Budgeted functions that grew")
        rows = [[before, after, "+{}".format(after - before),
                 source_link(function)]
                for key, before, after, function in regressed]
        parts.append(markdown_table(
            ["Budgeted", "Now", "Change", "Function"],
            ["---:", "---:", "---:", "---"], rows))

    if verdict["unbalanced"]:
        parts.append("### Files the scanner could not read")
        parts.append("\n".join(
            "- `{}`".format(path) for path in verdict["unbalanced"]))

    rows = distribution([function["complexity"] for function in functions])
    widest = max(count for label, count in rows) or 1
    chart = ["{:>8}  {:>6}  {}".format(
        label, count, "#" * int(round(40.0 * count / widest)))
        for label, count in rows]
    parts.append("### Distribution across the tree")
    parts.append("```\n" + "\n".join(chart) + "\n```")

    parts.append("### Where the weight sits")
    parts.append(markdown_table(
        ["Directory", "Functions", "Branch points", "Worst"],
        ["---", "---:", "---:", "---:"],
        [["`{}`".format(directory), entry[0], "{:,}".format(entry[1]),
          entry[2]]
         for directory, entry in directory_weights(functions)[:10]]))

    parts.append(
        "<details>\n<summary>How to clear this</summary>\n\n"
        "Reproduce locally:\n\n"
        "```bash\n"
        "python3 tools/ci/complexity_audit.py\n"
        "python3 tools/ci/complexity_audit.py --check\n"
        "```\n\n"
        "The fix is to split the function. A long `switch` that dispatches "
        "one case per opcode reads far worse in `cc` than it does on the "
        "page, so the `mcc` column counts each `switch` once: where `cc` is "
        "high and `mcc` is close to it, the branching is real.\n\n"
        "If the debt is deliberate and is going to stay, record it:\n\n"
        "```bash\n"
        "python3 tools/ci/complexity_audit.py --update-budget\n"
        "```\n\n"
        "That writes `tools/ci/complexity-budget.json`, which pins every "
        "function already over the ceiling at today's number. From then on "
        "those functions may shrink and may not grow.\n\n"
        "This issue is opened and updated by the `Complexity budget` job in "
        "`.github/workflows/ci.yml`. It closes itself when the default "
        "branch comes back under budget.\n</details>")

    parts.append(ISSUE_MARKER)
    return "\n\n".join(parts)


def run_gh(arguments, capture):
    command = ["gh"] + arguments
    try:
        if capture:
            completed = subprocess.run(command, cwd=ROOT, check=False,
                                       stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
        else:
            completed = subprocess.run(command, cwd=ROOT, check=False,
                                       stderr=subprocess.PIPE)
    except OSError as error:
        print("gh is not available: {}".format(error))
        return None, ""
    if completed.returncode != 0:
        message = completed.stderr.decode("utf-8", "replace").strip()
        print("gh {} failed: {}".format(" ".join(arguments[:2]), message))
        return None, ""
    output = completed.stdout.decode("utf-8", "replace") if capture else ""
    return completed.returncode, output


def find_open_issue():
    code, output = run_gh(
        ["issue", "list", "--label", ISSUE_LABEL, "--state", "open",
         "--limit", "50", "--json", "number,body"], True)
    if code is None:
        return None
    try:
        issues = json.loads(output or "[]")
    except ValueError:
        return None
    for issue in issues:
        if ISSUE_MARKER in (issue.get("body") or ""):
            return issue["number"]
    return None


def ensure_label():
    run_gh(["label", "create", ISSUE_LABEL, "--color", "B60205",
            "--description", "Raised by the cyclomatic complexity gate"], True)


def sync_issue(verdict, functions, files, dry_run):
    body = issue_body(verdict, functions, files)
    title = issue_title(verdict)
    if dry_run:
        print("--- title ---")
        print(title)
        print("--- body ---")
        print(body)
        print("--- would {} ---".format(
            "open or update the issue" if verdict["failing"]
            else "close the issue"))
        return 0

    ensure_label()
    existing = find_open_issue()
    if not verdict["failing"]:
        if existing is None:
            print("Under budget and no audit issue is open.")
            return 0
        code, _ = run_gh(
            ["issue", "close", str(existing), "--comment",
             "The default branch is back under the complexity budget."], True)
        if code is None:
            return 1
        print("Closed #{}.".format(existing))
        return 0

    handle = tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", encoding="utf-8", newline="\n", delete=False)
    path = handle.name
    try:
        handle.write(body)
        handle.close()
        if existing is None:
            code, output = run_gh(
                ["issue", "create", "--title", title, "--body-file", path,
                 "--label", ISSUE_LABEL], True)
            if code is None:
                return 1
            print("Opened {}".format(output.strip()))
        else:
            code, _ = run_gh(["issue", "edit", str(existing), "--title", title,
                              "--body-file", path], True)
            if code is None:
                return 1
            run_gh(["issue", "comment", str(existing), "--body",
                    "The budget is still breached as of the latest commit on "
                    "the default branch. The report above has been "
                    "refreshed."], True)
            print("Updated #{}.".format(existing))
    finally:
        handle.close()
        if os.path.exists(path):
            os.remove(path)
    return 0


SELF_TEST_CASES = [
    ("plain", "int f(void) { return 0; }", [("f", 1)]),
    ("branches", "int f(int a) { if (a) { return 1; } else { return 2; } }",
     [("f", 2)]),
    ("short_circuit", "int f(int a, int b) { return a && b ? 1 : 0; }",
     [("f", 3)]),
    ("loops", "void f(void) { for (;;) { while (1) { } } do { } while (0); }",
     [("f", 4)]),
    ("switch_cases",
     "int f(int a) { switch (a) { case 1: return 1; case 2: return 2; "
     "default: return 0; } }", [("f", 3)]),
    ("comment_is_not_code",
     "/* if (a) && || ? case */ int f(void) { return 0; }", [("f", 1)]),
    ("string_is_not_code",
     'int f(void) { const char *s = "if (a && b) case"; return s[0]; }',
     [("f", 1)]),
    ("escaped_quote_in_string",
     'int f(void) { const char *s = "\\" if (a)"; return 0; }', [("f", 1)]),
    ("char_literal_brace", "int f(void) { char c = '}'; return c; }",
     [("f", 1)]),
    ("directives_are_not_code",
     "#define M(x) do { if (x) { } } while (0)\nint f(void) { return 0; }",
     [("f", 1)]),
    ("multiline_directive",
     "#define M(x) \\\n  if (x) { } else { }\nint f(void) { return 0; }",
     [("f", 1)]),
    ("struct_is_not_a_function",
     "struct S { int a; };\nstatic const int t[] = {1, 2};\n"
     "int f(void) { return 0; }", [("f", 1)]),
    ("multiline_signature",
     "static int f(int a,\n              int b) {\n  if (a) { return b; }\n"
     "  return 0;\n}", [("f", 2)]),
    ("attribute_after_parameters",
     "static void f(void) __attribute__((noreturn));\n"
     "static void f(void) __attribute__((noreturn)) { if (1) { } }",
     [("f", 2)]),
    ("two_functions",
     "int a(void) { if (1) { } return 0; }\nint b(void) { return 0; }",
     [("a", 2), ("b", 1)]),
    ("nested_blocks",
     "int f(int a) { { if (a) { for (int i = 0; i < a; i++) { } } } "
     "return 0; }", [("f", 3)]),
]


def self_test():
    failures = 0
    for name, source, expected in SELF_TEST_CASES:
        found, unbalanced = scan_source("t.c", source)
        actual = [(function["name"], function["complexity"])
                  for function in found]
        if unbalanced or actual != expected:
            failures += 1
            print("  FAIL {}: expected {}, measured {}{}".format(
                name, expected, actual,
                " (unbalanced)" if unbalanced else ""))
        else:
            print("  ok   {}".format(name))
    if failures:
        print("{} of {} scanner cases failed.".format(
            failures, len(SELF_TEST_CASES)))
        return 1
    print("All {} scanner cases pass.".format(len(SELF_TEST_CASES)))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="Measure and gate the cyclomatic complexity of the C "
                    "sources.")
    parser.add_argument("--check", action="store_true",
                        help="gate against tools/ci/complexity-budget.json "
                             "and exit non-zero on a breach")
    parser.add_argument("--update-budget", action="store_true",
                        help="rewrite the budget file from what the sources "
                             "measure today")
    parser.add_argument("--sync-issue", action="store_true",
                        help="open, refresh or close the GitHub issue that "
                             "tracks a breach, using the gh CLI")
    parser.add_argument("--dry-run", action="store_true",
                        help="with --sync-issue, print the report instead of "
                             "touching GitHub")
    parser.add_argument("--self-test", action="store_true",
                        help="check the scanner against its own cases")
    parser.add_argument("--ceiling", type=int, default=None,
                        help="per-function ceiling (default: the budget "
                             "file's, else {})".format(DEFAULT_CEILING))
    parser.add_argument("--alarm", type=int, default=None,
                        help="the level that counts as an emergency and "
                             "raises an issue (default: the budget file's, "
                             "else {})".format(DEFAULT_ALARM))
    parser.add_argument("--root", action="append", default=None,
                        metavar="PATH",
                        help="directory or file to scan; repeatable")
    parser.add_argument("--top", type=int, default=25,
                        help="how many of the heaviest functions to list")
    parser.add_argument("--json", metavar="PATH", default=None,
                        help="write every measurement to PATH as JSON")
    arguments = parser.parse_args(argv)

    if arguments.self_test:
        return self_test()

    budget = load_budget()
    roots = arguments.root or budget.get("roots") or list(DEFAULT_ROOTS)
    ceiling = arguments.ceiling or budget.get("ceiling") or DEFAULT_CEILING
    alarm = arguments.alarm or budget.get("alarm") or DEFAULT_ALARM
    budget["ceiling"] = ceiling
    budget["alarm"] = alarm

    files, functions, unbalanced_files = collect(roots)

    if arguments.json:
        with open(arguments.json, "w", encoding="utf-8",
                  newline="\n") as handle:
            json.dump({
                "ceiling": ceiling,
                "alarm": alarm,
                "roots": list(roots),
                "files": len(files),
                "statistics": statistics(functions, ceiling, alarm),
                "functions": functions,
            }, handle, indent=2)
            handle.write("\n")

    if arguments.update_budget:
        write_budget(build_budget(functions, ceiling, alarm, roots))
        stats = statistics(functions, ceiling, alarm)
        print("Wrote {} with {} entries over the ceiling of {}.".format(
            relative(BUDGET_PATH), stats["over_ceiling"], ceiling))
        return 0

    verdict = evaluate(functions, budget, unbalanced_files)
    verdict["roots"] = list(roots)

    if arguments.sync_issue:
        return sync_issue(verdict, functions, len(files), arguments.dry_run)

    if arguments.check:
        return check(verdict)

    print_report(files, functions, unbalanced_files, ceiling, alarm,
                 arguments.top)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
