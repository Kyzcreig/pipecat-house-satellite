#!/usr/bin/env python3
"""Every /xvf/tune knob must have an EFFECT consumer, not just bookkeeping.

Incident (t_029fcce2, 2026-10-11): a brightness knob was argued about for 16 h on
the strength of write == readback. A readback proves the STORE, never the effect.
This lint makes the firmware prove, statically, that each runtime tunable reaches
hardware. It walks the kTuneEntries table and, for every knob:

  * finds the branch of pipecat_xvf_tune() that handles the knob's TuneTarget;
  * DIRECT effect: that branch (transitively) calls a hardware write sink
    (SINKS below; i2c_master_transmit is the XMOS/codec write; the read path uses
    i2c_master_transmit_receive and does not count);
  * STATE effect: otherwise the branch stores into firmware state
    (``var.store(...)`` / ``var = ...``). Then at least one function that READS
    that state must also reach a sink. Reads that do not count, by construction:
      - pipecat_xvf_tune / pipecat_xvf_read_param (the write and the readback);
      - reads inside a log/print call (ESP_LOG*, printf, snprintf);
      - functions that never reach a sink (NVS load/save, status JSON);
      - one-line accessors (``return var.load();``) are followed to their callers,
        which are judged by the same rules.

A knob with zero effect consumers FAILS with the knob name. Unparseable source
fails closed (exit 2), never green.

Limit (stated, not hidden): this proves ">=1 effect path exists", not that the
value lands in the sink's argument. A read in a function that also writes
unrelated registers counts. The mutation runner pins the shapes it catches.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SINKS = ("i2c_master_transmit", "i2s_channel_write", "gpio_set_level", "ledc_set_duty")
TUNE_FN = "pipecat_xvf_tune"
BOOKKEEPING_FNS = {TUNE_FN, "pipecat_xvf_read_param"}
LOG_CALL = re.compile(r"\b(ESP_LOG[A-Z]?|ESP_EARLY_LOG[A-Z]?|printf|snprintf|fprintf)\s*\(")
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "catch", "defined"}
IDENT_CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def strip_comments_and_strings(src: str) -> str:
    """Blank comments and string/char literal contents, keep offsets and newlines."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(c + " " * (min(j, n) - i - 1) + (c if j < n else ""))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def balance_conditionals(text: str) -> str:
    """Blank preprocessor lines. Keep every branch of an #if group whose branches are
    each brace-neutral (maximally inclusive for the consumer search). When any
    branch opens or closes a brace across the directive (`#if X if (a) { #else
    if (b) { #endif`), keep only the LAST branch so braces stay balanced."""
    lines = text.split("\n")
    keep = [True] * len(lines)
    stack: list[list[list[int]]] = []  # per open #if: [[start_line, brace_delta], ...]
    for i, line in enumerate(lines):
        s = line.strip()
        if re.match(r"#\s*if", s):
            stack.append([[i, 0]])
        elif re.match(r"#\s*(else|elif)", s) and stack:
            stack[-1].append([i, 0])
        elif re.match(r"#\s*endif", s) and stack:
            branches = stack.pop()
            unbalanced = any(d != 0 for _, d in branches) and len(branches) > 1
            if unbalanced:
                for (start, _), nxt in zip(branches, branches[1:] + [[i, 0]]):
                    if nxt[0] != i:
                        for k in range(start, nxt[0]):
                            keep[k] = False
            # a dropped branch's delta must not count toward the enclosing #if
            delta = branches[-1][1] if unbalanced else sum(d for _, d in branches)
            if stack:
                stack[-1][-1][1] += delta
        else:
            if stack:
                stack[-1][-1][1] += line.count("{") - line.count("}")
            continue
        keep[i] = False  # the directive line itself
    return "\n".join(l if k else "" for l, k in zip(lines, keep))


def match_brace(text: str, open_pos: int) -> int:
    depth = 0
    for pos in range(open_pos, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return pos
    raise ValueError("unbalanced braces")


SIG = re.compile(r"([A-Za-z_]\w*)\s*\((?:[^()]|\([^()]*\))*\)\s*(?:const\s*)?(?:noexcept\s*)?$")


def functions(text: str) -> dict[str, list[str]]:
    """Top-level function name -> list of bodies (static names may repeat across files)."""
    found: dict[str, list[str]] = {}
    pos, seg_start = 0, 0
    while pos < len(text):
        ch = text[pos]
        if ch in ";}":
            seg_start = pos + 1
            pos += 1
            continue
        if ch == "{":
            end = match_brace(text, pos)
            head = "\n".join(l for l in text[seg_start:pos].splitlines() if not l.lstrip().startswith("#"))
            m = SIG.search(head.rstrip())
            if m and m.group(1) not in KEYWORDS:
                found.setdefault(m.group(1), []).append(text[pos + 1 : end])
            pos = end + 1
            seg_start = pos
            continue
        pos += 1
    return found


def drop_log_calls(body: str) -> str:
    out, i = [], 0
    for m in LOG_CALL.finditer(body):
        if m.start() < i:
            continue
        out.append(body[i : m.start()])
        depth, j = 0, m.end() - 1
        while j < len(body):
            if body[j] == "(":
                depth += 1
            elif body[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        i = j + 1
    out.append(body[i:])
    return "".join(out)


def calls(body: str) -> set[str]:
    return {m.group(1) for m in IDENT_CALL.finditer(body)} - KEYWORDS


class Graph:
    def __init__(self, fns: dict[str, list[str]], exclude: frozenset[str] = frozenset()):
        self.fns = fns
        self.exclude = exclude  # never traverse these (the tune write path itself)
        self._sink: dict[str, list[str] | None] = {}

    def sink_path(self, name: str, stack: tuple[str, ...] = ()) -> list[str] | None:
        """Call chain from ``name`` to a sink, or None."""
        if name in SINKS:
            return [name]
        if name in self._sink:
            return self._sink[name]
        if name in stack or name not in self.fns or name in self.exclude:
            return None
        result = None
        for body in self.fns[name]:
            for callee in sorted(calls(body)):
                sub = self.sink_path(callee, stack + (name,))
                if sub:
                    result = [name] + sub
                    break
            if result:
                break
        self._sink[name] = result
        return result

    def segment_sink(self, segment: str) -> list[str] | None:
        for callee in sorted(calls(segment)):
            path = self.sink_path(callee)
            if path:
                return path
        return None


def target_segments(tune_body: str) -> tuple[dict[str, str], str]:
    """Per-target dedicated branches + the shared remainder (the generic path)."""
    seg: dict[str, str] = {}
    remainder = tune_body
    for m in list(re.finditer(r"if\s*\(\s*entry->target\s*==\s*TuneTarget::(\w+)\s*\)\s*\{", tune_body)):
        open_pos = m.end() - 1
        close = match_brace(tune_body, open_pos)
        seg[m.group(1)] = tune_body[open_pos + 1 : close]
        remainder = remainder.replace(tune_body[m.start() : close + 1], " ")
    return seg, remainder


STORE = re.compile(r"(?<![\w.>])([A-Za-z_]\w*)\s*(?:\.\s*store\s*\(|(?<![=!<>+\-*/|&^%])=(?!=))")


def file_scope_text(text: str) -> str:
    """The source with every top-level {...} body removed (declarations remain)."""
    out, pos = [], 0
    while True:
        i = text.find("{", pos)
        if i < 0:
            out.append(text[pos:])
            return "".join(out)
        out.append(text[pos:i])
        pos = match_brace(text, i) + 1


def is_global(scope: str, var: str) -> bool:
    return re.search(r"\b" + re.escape(var) + r"\s*(?:\{|=|;|\[)", scope) is not None


def accessor_of(fns: dict[str, list[str]], var: str) -> set[str]:
    pat = re.compile(r"^\s*return\s+" + re.escape(var) + r"(?:\s*\.\s*load\s*\(\s*\))?\s*;\s*$")
    return {n for n, bodies in fns.items() if any(pat.match(b) for b in bodies)}


def reads_var(body: str, var: str) -> bool:
    body = drop_log_calls(body)
    for m in re.finditer(r"\b" + re.escape(var) + r"\b", body):
        tail = body[m.end() : m.end() + 12]
        if re.match(r"\s*\.\s*store\s*\(", tail) or re.match(r"\s*=(?!=)", tail):
            continue
        return True
    return False


def effect_consumers(graph: Graph, var: str) -> tuple[list[str], list[str]]:
    """(effect consumers with sink chain, bookkeeping readers) for a state variable."""
    fns = graph.fns
    accessors = accessor_of(fns, var)
    effect, bookkeeping = [], []
    for name, bodies in sorted(fns.items()):
        if name in accessors:
            bookkeeping.append(f"{name} (accessor)")
            continue
        if not any(reads_var(b, var) or any(reads_var(b, a) for a in accessors) for b in bodies):
            continue
        if name in BOOKKEEPING_FNS:
            bookkeeping.append(f"{name} (write/readback)")
            continue
        path = graph.sink_path(name)
        if path:
            effect.append(" -> ".join(path))
        else:
            bookkeeping.append(f"{name} (no sink)")
    return effect, bookkeeping


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Every /xvf/tune knob must have an effect consumer.")
    ap.add_argument("--repo", default=".", help="repository root")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the evidence for every knob")
    args = ap.parse_args(argv)
    src_dir = Path(args.repo) / "xiao-esp32-s3" / "src"
    files = sorted(list(src_dir.glob("*.cpp")) + list(src_dir.glob("*.c")))
    if not files:
        print(f"tune-knob-effect-lint: no firmware sources under {src_dir}", file=sys.stderr)
        return 2
    try:
        texts = {f: balance_conditionals(strip_comments_and_strings(f.read_text(encoding="utf-8"))) for f in files}
        fns: dict[str, list[str]] = {}
        for text in texts.values():
            for name, bodies in functions(text).items():
                fns.setdefault(name, []).extend(bodies)
        # Knob names are string literals, blanked above; read the table from raw source.
        raw = next(f.read_text(encoding="utf-8") for f in files if "kTuneEntries[]" in f.read_text(encoding="utf-8"))
        knobs = re.findall(r'\{\s*"(\w+)"\s*,[^{}]*?TuneTarget::(\w+)',
                           raw[raw.index("kTuneEntries[]") : raw.index("};", raw.index("kTuneEntries[]"))])
        if TUNE_FN not in fns or not knobs:
            raise ValueError(f"{TUNE_FN}() or the kTuneEntries table not found")
        seg, remainder = target_segments(fns[TUNE_FN][0])
        scope = "\n".join(file_scope_text(t) for t in texts.values())
    except (StopIteration, ValueError) as exc:
        print(f"tune-knob-effect-lint: cannot parse firmware ({exc}); failing closed", file=sys.stderr)
        return 2
    # Effect consumers are searched with the tune write path cut out of the graph:
    # a function that only re-applies the knob through pipecat_xvf_tune (NVS replay,
    # the HTTP handler) proves nothing about where the stored value goes.
    graph = Graph(fns, exclude=frozenset(BOOKKEEPING_FNS))
    failures = 0
    for knob, target in knobs:
        segment = seg.get(target, remainder)
        path = graph.segment_sink(drop_log_calls(segment))
        if path:
            if args.verbose:
                print(f"ok   {knob:<18} {target:<20} direct: {' -> '.join(path)}")
            continue
        stored = sorted(v for v in {m.group(1) for m in STORE.finditer(drop_log_calls(segment))}
                        if is_global(scope, v))
        reasons = []
        for var in stored:
            effect, books = effect_consumers(graph, var)
            if effect:
                if args.verbose:
                    print(f"ok   {knob:<18} {target:<20} state {var}: {'; '.join(effect)}")
                break
            reasons.append(f"{var} read only by: {', '.join(books) or 'nothing'}")
        else:
            failures += 1
            why = "; ".join(reasons) if reasons else "tune branch neither writes hardware nor stores state"
            print(f"FAIL tune knob '{knob}' (TuneTarget::{target}): no effect consumer. {why}")
    print(f"tune-knob-effect-lint: {len(knobs)} knob(s), {failures} without an effect consumer")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
