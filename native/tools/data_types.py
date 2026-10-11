#!/usr/bin/env python3
"""The C++ type of every retail symbol the C++ declares (RETAIL(name): `extern T x asm("name")`), from clang's AST dump of each
file built natively. Writes build/native/data_types.json: {label: {"name": C++ name, "type": type, "canonical": canonical type,
"kind": "var" | "function"}}. The data converter (native/tools/convert_data.py) lays the split's data out by these types."""
import json, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
FLAGS = ["-std=gnu++2c", "-fsyntax-only", "-DTWIN_NATIVE", "-Inative/include", "-Iinclude", "-fno-exceptions", "-fno-rtti",
         "-Wno-everything", "-Xclang", "-ast-dump"]
DECL = re.compile(r"^[\s|`-]*(VarDecl|FunctionDecl|CXXMethodDecl)\s+0x[0-9a-f]+\s.*?\s(\w+)\s'([^']*)'(?::'([^']*)')?")
LABEL = re.compile(r"^[\s|`-]*AsmLabelAttr\s+0x[0-9a-f]+\s.*\s\"([^\"]+)\"\s*$")

def scan(path):
    out = subprocess.run(["clang++"] + FLAGS + [path], capture_output=True, text=True, cwd=HERE).stdout.splitlines()
    found, pending = {}, None

    def commit_unlabelled():
        # A variable declared without RETAIL in extern "C" is the retail symbol of its own name (G_GameController); a C++
        # one's mangled name matches no retail label, so taking every variable's name is harmless
        if pending and pending[0] == "VarDecl" and pending[1] not in found:
            kind, name, typ, canonical, defined = pending
            found[name] = {"name": name, "type": typ, "canonical": canonical, "kind": "var", "defined": defined,
                           "unlabelled": True}

    for line in out:
        m = DECL.match(line)
        if m:
            commit_unlabelled()
            # A variable declared without extern is defined by the C++ (the converter leaves it out)
            rest = line[m.end():]
            defined = m.group(1) == "VarDecl" and not re.search(r"\bextern\b", rest)
            pending = (m.group(1), m.group(2), m.group(3), m.group(4) or m.group(3), defined)
            continue
        m = LABEL.match(line)
        if m and pending:
            # common.h's RETAIL puts Mach-O's underscore in front of the retail name
            label = m.group(1)[1:] if sys.platform == "darwin" and m.group(1).startswith("_") else m.group(1)
            kind, name, typ, canonical, defined = pending
            previous = found.get(label, {})
            if previous.get("unlabelled"):
                previous = {}
            found[label] = {"name": name, "type": typ, "canonical": canonical,
                                 "kind": "var" if kind == "VarDecl" else "function",
                                 "defined": defined or previous.get("defined", False)}
            pending = None
    commit_unlabelled()
    return found

# The native build's C++: the game's, the native platform's and the PS2 platform files it reuses (native/reused_ps2.txt)
reused = [l.strip() for l in (HERE / "native" / "reused_ps2.txt").read_text().splitlines() if l.strip() and not l.startswith("#")]
files = sorted(str(p.relative_to(HERE)) for p in (HERE / "src").rglob("*.cpp")
               if (p.relative_to(HERE / "src").parts[0] != "platform" or p.relative_to(HERE / "src").parts[1] == "native")
               and p.name != "abi.cpp") + reused
types = {}
with ThreadPoolExecutor(10) as pool:
    for found in pool.map(scan, files):
        for label, info in found.items():
            if label in types and types[label].get("defined"):
                info["defined"] = True
            if label in types and types[label]["canonical"] != info["canonical"]:
                # Declared differently in two files (an array with and without its size): keep the sized one
                if "[]" in info["canonical"]:
                    continue
            types[label] = info
out = HERE / "build" / "native" / "data_types.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(types, indent=1, sort_keys=True))
print(f"{len(types)} retail symbols ({sum(t['kind'] == 'var' for t in types.values())} variables) -> {out.relative_to(HERE)}")
