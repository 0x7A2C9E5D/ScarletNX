#!/usr/bin/env python3
"""Candidates for the code quality rules of RFC #142, for a human to judge. It never blocks anything.

    check.py [PATH...]     (files or folders; default src/; .c and .h)

It prints one line per candidate, file:line: rule: what it saw. Three rules can be seen in the text:
  flag    a presence flag beside the pointer it describes (has_X / X_present / X_valid beside a pointer X,
          or is_present / present / valid in a struct with one pointer): NULL already says it
  allocs  one function filling two or more members of one object from separate allocations: one block
          carved by offsets frees once and keeps them together
  atomics two or more atomics in one struct, or defined at file scope in one file, a later one without an
          alignas of its own: they may share a cache line, and two threads writing them slow each other down
Exit status 0 whatever it finds; 2 when a path cannot be read.
"""
import os, re, sys

ALLOC = r"(?:calloc|malloc|realloc|aligned_alloc|snx_alloc|snx_realloc|SDL_calloc|SDL_malloc)"
ATOMIC = re.compile(r"\b(_Atomic\b|atomic_[a-z_]+\b|SDL_Atomic(?:Int|U32)\b)")
GENERIC_FLAGS = {"is_present", "present", "valid", "is_valid", "has_value"}


def strip(text):
    """Comments and string or char literals blanked, newlines kept, so line numbers still match."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', blank, text, flags=re.S)


def closing(text, start):
    """The index of the brace closing the one at start."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if not depth:
                return i
    return len(text) - 1


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def members(body, offset):
    """(name, declaration, index) of each member at the top level of a struct body."""
    out, depth, start = [], 0, 0
    for i, c in enumerate(body):
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif c == ";" and depth == 0:
            decl = body[start:i].strip()
            names = re.findall(r"(\w+)\s*(?:\[[^\]]*\]\s*)*(?::\s*\d+\s*)?(?=,|$)", decl)
            lead = len(body[start:i]) - len(body[start:i].lstrip())
            for name in names:
                out.append((name, decl, offset + start + lead))
            start = i + 1
    return out


def check_structs(path, text, report):
    for m in re.finditer(r"\b(?:struct|union)\s*\w*\s*\{", text):
        open_at = m.end() - 1
        body = text[open_at + 1:closing(text, open_at)]
        decls = members(body, open_at + 1)
        pointers = {name for name, decl, _ in decls if re.search(r"\*\s*(?:const\s+)?" + re.escape(name) + r"\b", decl)}
        for name, decl, at in decls:
            if not re.match(r"(?:bool|_Bool|int|unsigned|u8|uint8_t|char)\b", decl):
                continue
            target = None
            for pattern in (r"has_(\w+)", r"(\w+)_present", r"(\w+)_valid", r"is_(\w+)_set"):
                hit = re.fullmatch(pattern, name)
                if hit and hit.group(1) in pointers:
                    target = hit.group(1)
            if not target and name in GENERIC_FLAGS and len(pointers) == 1:
                target = next(iter(pointers))
            if target:
                report(path, line_of(text, at), "flag", f"'{name}' beside pointer '{target}'")
        atomics = [(name, decl, at) for name, decl, at in decls if ATOMIC.search(decl[:decl.rfind(name)])]
        if len(atomics) > 1:
            bare = [name for name, decl, _ in atomics[1:] if not re.search(r"\b(?:alignas|_Alignas)\s*\(", decl)]
            if bare:
                report(path, line_of(text, atomics[0][2]), "atomics",
                       f"{len(atomics)} atomics in one struct ({', '.join(n for n, _, _ in atomics)}), "
                       f"no alignas on {', '.join(bare)}")


def bodies(text):
    """The index of each function body's opening brace: a brace at file scope right after a ')'."""
    depth, last = 0, ""
    for i, c in enumerate(text):
        if c == "{":
            if depth == 0 and last == ")":
                yield i
            depth += 1
        elif c == "}":
            depth = max(0, depth - 1)
        if not c.isspace():
            last = c


def check_functions(path, text, report):
    for open_at in bodies(text):
        body = text[open_at:closing(text, open_at)]
        owners = {}
        for a in re.finditer(r"(\b\w+(?:\s*(?:->|\.)\s*\w+)*?)\s*(?:->|\.)\s*\w+\s*(?:\[[^\]]*\])?\s*=\s*"
                             r"(?:\([^()]*\)\s*)?" + ALLOC + r"\s*\(", body):
            owner = re.sub(r"\s+", "", a.group(1))
            owners.setdefault(owner, []).append(open_at + a.start())
        for owner, hits in owners.items():
            if len(hits) > 1:
                report(path, line_of(text, hits[0]), "allocs",
                       f"{len(hits)} separate allocations into members of '{owner}'")


def check_globals(path, text, report):
    """Atomics defined at file scope: the compiler lays a file's definitions out together."""
    depth, start, found = 0, 0, []
    for i, c in enumerate(text):
        if c == "{":
            depth += 1
        elif c == "}":
            depth = max(0, depth - 1)
            if depth == 0:
                start = i + 1
        elif c == ";" and depth == 0:
            stmt = text[start:i]
            lead = len(stmt) - len(stmt.lstrip())
            decl = stmt.strip()
            # an extern or a typedef does not match: only definitions take space here
            if ATOMIC.match(re.sub(r"^(?:static|_Thread_local|thread_local|volatile)\s+", "", decl)) \
                    and "(" not in decl.split("=")[0]:
                name = re.findall(r"(\w+)\s*(?:\[[^\]]*\]\s*)*(?:=|$)", decl.split("=")[0] + "=")[0]
                found.append((name, decl, start + lead))
            start = i + 1
    if len(found) > 1:
        bare = [name for name, decl, _ in found[1:] if not re.search(r"\b(?:alignas|_Alignas)\s*\(", decl)]
        if bare:
            report(path, line_of(text, found[0][2]), "atomics",
                   f"{len(found)} atomics at file scope ({', '.join(n for n, _, _ in found)}), "
                   f"no alignas on {', '.join(bare)}")


def files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in sorted(names):
                    if n.endswith((".c", ".h")):
                        yield os.path.join(root, n)
        else:
            yield p


def main(argv):
    paths = argv[1:] or ["src"]
    found, failed = [], False

    def report(path, line, rule, what):
        found.append(f"{path}:{line}: {rule}: {what}")

    for path in files(paths):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = strip(f.read())
        except OSError as e:
            print(f"{path}: {e.strerror}", file=sys.stderr)
            failed = True
            continue
        check_structs(path, text, report)
        check_functions(path, text, report)
        check_globals(path, text, report)
    for line in found:
        print(line)
    print(f"{len(found)} candidate(s)", file=sys.stderr)
    return 2 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
