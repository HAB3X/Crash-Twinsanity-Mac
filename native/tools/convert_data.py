#!/usr/bin/env python3
"""The retail data the native build needs, made C++: build/native/data.cpp.

The PS2 build links the game's data (.data, .rodata, .sdata, .bss, .sbss) from the split (asm/data/*.s): words of the retail
executable, its pointers as symbols. The native build's pointers are 64 bit, so each object is laid out again by its C++ type
(build/native/data_types.json, native/tools/data_types.py): clang's record layouts for the PS2 (mips64el n32, the decomp's own
layout checks hold there) and for the host give every field's place on both, numbers and text are copied to their native place,
and every pointer becomes a fixup applied at start-up (NativeApplyDataFixups) before the game runs. An object the C++ doesn't
name but other data points at gets its type from the pointer's type; one nothing reaches is left out. Nothing is guessed: an
object whose layout can't be worked out exactly is reported, and the build stops on it unless it's listed in KNOWN_RAW below.

    native/tools/convert_data.py [--report]
"""
import argparse, collections, json, os, re, struct, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]

# Objects whose raw byte members hold addresses the game never reads: those bytes are left zero natively, with the reason
KNOWN_RAW = {
    "D_002EA0E0": "newlib's struct _reent (_impure_data): RetailLibc::Reent's unused04 bytes are stdio's FILE pointers "
                  "(_stdin, _stdout, _stderr) and unused5C is the rest of the block; the game only reads errno, rand's state "
                  "and atexit's list",
}
BUILD = HERE / "build" / "native"

# ---------------------------------------------------------------------------------------------------------------- the split
def read_split():
    """{label: {"section", "data": bytearray, "relocs": {offset: (symbol, addend)}, "align"}} from asm/data"""
    objects, order = {}, []
    for path in sorted((HERE / "asm" / "data").glob("*.s")):
        section = path.stem.split(".")[0]
        current = None
        previous = None
        pad = False
        for raw in path.read_text().splitlines():
            line = raw.strip()
            if "Automatically generated and unreferenced pad" in line:
                pad = True
                continue
            m = re.match(r"(?:dlabel|glabel|jlabel)\s+(\S+)", line)
            if m:
                if pad and previous is not None:
                    previous["pads"] = previous.get("pads", 0) + 1
                    # A label splat made where nothing points: the bytes are the previous object's (its array goes on: the
                    # static constructors' count, then its functions), and stay right after it as on the PS2
                    current = previous
                else:
                    current = {"section": section, "data": bytearray(), "relocs": {}, "address": None,
                               "label": m.group(1)}
                    objects[m.group(1)] = current
                    order.append(m.group(1))
                previous, pad = current, False
                continue
            if line.startswith(("enddlabel", "endlabel")):
                current = None
                continue
            if current is None:
                continue
            if current["address"] is None:
                # The comment in front of the first data: "/* rom vram word */" (data) or "/* vram */" (bss)
                found = re.match(r"/\*\s*([0-9A-Fa-f]+)(?:\s+([0-9A-Fa-f]{8}))?", line)
                if found:
                    current["address"] = int(found.group(2) or found.group(1), 16) - len(current["data"])
            m = re.match(r'\.incbin\s+"([^"]+)"(?:\s*,\s*(\w+)\s*,\s*(\w+))?', line)
            if m:
                # A binary the split keeps as a file (the VU microcode): its bytes, from the start and length given
                blob = (HERE / m.group(1)).read_bytes()
                start = int(m.group(2), 0) if m.group(2) else 0
                length = int(m.group(3), 0) if m.group(3) else len(blob) - start
                current["data"] += blob[start:start + length]
                continue
            m = re.search(r"\.(word|short|half|byte|space|skip|zero|float|asciz|ascii|balign|align)\s*(.*)$", line)
            if not m:
                continue
            kind, arg = m.group(1), m.group(2).split("/*")[0].strip()
            data = current["data"]
            if kind == "word":
                for value in arg.split(","):
                    value = value.strip()
                    if re.match(r"[A-Za-z_.$]", value):
                        sym = re.match(r"([A-Za-z_.$][\w.$]*)\s*(?:([+-])\s*(0x[0-9A-Fa-f]+|\d+))?$", value)
                        addend = int(sym.group(3), 0) * (-1 if sym.group(2) == "-" else 1) if sym.group(2) else 0
                        current["relocs"][len(data)] = (sym.group(1), addend)
                        data += b"\0\0\0\0"
                    else:
                        data += struct.pack("<I", int(value, 0) & 0xFFFFFFFF)
            elif kind in ("short", "half"):
                for value in arg.split(","):
                    data += struct.pack("<H", int(value.strip(), 0) & 0xFFFF)
            elif kind == "byte":
                for value in arg.split(","):
                    data += struct.pack("<B", int(value.strip(), 0) & 0xFF)
            elif kind in ("space", "skip", "zero"):
                data += b"\0" * int(arg.split(",")[0], 0)
            elif kind == "float":
                for value in arg.split(","):
                    data += struct.pack("<f", float(value.strip()))
            elif kind in ("asciz", "ascii"):
                text = bytes(arg.strip()[1:-1], "utf-8").decode("unicode_escape").encode("latin-1")
                data += text + (b"\0" if kind == "asciz" else b"")
            elif kind in ("balign", "align"):
                pass
    return objects, order

# ---------------------------------------------------------------------------------------------------------------- layouts
def toolchain_includes():
    gxx = os.path.expanduser("~/ps2dev/ee/bin/mips64r5900el-ps2-elf-g++")
    r = subprocess.run([gxx, "-E", "-v", "-x", "c++", os.devnull], capture_output=True, text=True)
    lines = r.stderr.splitlines()
    s, e = lines.index("#include <...> search starts here:") + 1, lines.index("End of search list.")
    dirs = [os.path.normpath(l.strip()) for l in lines[s:e]]
    return [a for d in dirs if not d.endswith(("15.2.0/include", "include-fixed")) for a in ("-isystem", d)]

def probe_source():
    """Every header, and the .cpp files' own types too: the probe includes each .cpp's types by compiling the .cpp files"""
    headers = sorted(p.relative_to(HERE / "include").as_posix() for p in (HERE / "include").rglob("*.h"))
    return "".join(f'#include "{h}"\n' for h in headers)

def dump_layouts(target):
    """{record name: {"size", "align", "fields": [(offset, bit, depth, type, name)]}} for "native" or "ps2" """
    BUILD.mkdir(parents=True, exist_ok=True)
    cache = BUILD / f"layouts_{target}.json"
    probe = BUILD / "probe.cpp"
    reused = [HERE / l.strip() for l in (HERE / "native" / "reused_ps2.txt").read_text().splitlines()
              if l.strip() and not l.startswith("#")]
    sources = [probe] + sorted(p for p in (HERE / "src").rglob("*.cpp")
                               if (p.relative_to(HERE / "src").parts[0] != "platform"
                                   or p.relative_to(HERE / "src").parts[1] == "native") and p.name != "abi.cpp") + reused
    newest = max(p.stat().st_mtime for p in list((HERE / "include").rglob("*.h")) + sources[1:])
    if cache.exists() and cache.stat().st_mtime > newest:
        return json.loads(cache.read_text())
    probe.write_text(probe_source())
    if target == "native":
        base = ["clang++", "-std=gnu++2c", "-fsyntax-only", "-DTWIN_NATIVE", "-Inative/include", "-Iinclude",
                "-Isrc/platform/native", "-I/opt/homebrew/include"]
    else:
        base = (["clang++", "--target=mips64el-unknown-elf", "-mabi=n32", "-std=gnu++2c", "-fsyntax-only", "-nostdinc++"]
                + toolchain_includes() + ["-D_EE", "-Iinclude", "-I" + os.path.expanduser("~/ps2dev/ps2sdk/ee/include"),
                                          "-I" + os.path.expanduser("~/ps2dev/ps2sdk/common/include")])
    base += ["-fno-exceptions", "-fno-rtti", "-Wno-everything", "-Xclang", "-fdump-record-layouts-complete"]
    layouts = {}
    from concurrent.futures import ThreadPoolExecutor
    def run(source):
        args = base
        if target == "ps2" and "platform/native" in str(source):
            # The native platform's own types laid out as the PS2 had them: built for the PS2's target (its C++ library's
            # headers) with the native build's headers in place of PS2SDK's (uiptr is the target's pointer size, 32 bits)
            args = (["clang++", "--target=mips64el-unknown-elf", "-mabi=n32", "-std=gnu++2c", "-fsyntax-only", "-nostdinc++"]
                    + toolchain_includes() + ["-DTWIN_NATIVE", "-Inative/include", "-Iinclude", "-Isrc/platform/native",
                                              "-I/opt/homebrew/include", "-fno-exceptions", "-fno-rtti", "-Wno-everything",
                                              "-Xclang", "-fdump-record-layouts-complete"])
        return subprocess.run(args + [str(source)], capture_output=True, text=True, cwd=HERE).stdout
    with ThreadPoolExecutor(10) as pool:
        for text in pool.map(run, sources):
            parse_layouts(text, layouts)
    cache.write_text(json.dumps(layouts))
    return layouts

FIELD = re.compile(r"^\s*(\d+)(?::(\d+)-(\d+))?\s\|(\s+)(.*)$")

def parse_layouts(text, layouts):
    for block in text.split("*** Dumping AST Record Layout")[1:]:
        lines = [l for l in block.splitlines() if l.strip()]
        head = FIELD.match(lines[0])
        if not head:
            continue
        name = head.group(5).strip()
        fields = []
        size = align = None
        for line in lines[1:]:
            m = re.search(r"\[sizeof=(\d+),.*align=(\d+)", line)
            if m:
                size, align = int(m.group(1)), int(m.group(2))
                continue
            m = FIELD.match(line)
            if not m:
                continue
            depth = (len(m.group(4)) - 1) // 2
            body = m.group(5).rstrip()
            bits = (int(m.group(2)), int(m.group(3))) if m.group(2) else None
            fields.append((int(m.group(1)), bits, depth, body))
        if size is not None and name not in layouts:
            layouts[name] = {"size": size, "align": align, "fields": fields}

# ---------------------------------------------------------------------------------------------------------------- types
SCALARS = {  # name: (ps2 size, native size, kind)
    **{n: (1, 1, "int") for n in ("u8", "s8", "char", "signed char", "unsigned char", "bool", "uint8_t", "int8_t", "vu8")},
    **{n: (2, 2, "int") for n in ("u16", "s16", "short", "unsigned short", "uint16_t", "int16_t", "char16_t", "vu16")},
    **{n: (4, 4, "int") for n in ("u32", "s32", "int", "unsigned int", "uint32_t", "int32_t", "vu32", "vs32", "char32_t",
                                  "wchar_t")},
    **{n: (4, 4, "float") for n in ("f32", "float")},
    **{n: (8, 8, "int") for n in ("u64", "s64", "long long", "unsigned long long", "uint64_t", "int64_t")},
    **{n: (8, 8, "float") for n in ("double",)},
    **{n: (16, 16, "int") for n in ("u128", "s128", "__int128", "unsigned __int128")},
    # Pointer-sized integers: 32 bit on the PS2, 64 natively (an address in them is a fixup)
    **{n: (4, 8, "uptr") for n in ("uiptr", "uintptr_t", "std::uintptr_t", "size_t", "std::size_t", "unsigned long")},
    **{n: (4, 8, "sptr") for n in ("siptr", "intptr_t", "std::intptr_t", "ptrdiff_t", "long")},
}

def enum_bases():
    """Enums with an underlying type of their own (enum X : u8): their size"""
    sizes = {}
    for path in list((HERE / "include").rglob("*.h")) + list((HERE / "src").rglob("*.cpp")):
        for m in re.finditer(r"\benum\s+(?:class\s+|struct\s+)?(\w+)\s*:\s*([\w:]+)", path.read_text(errors="ignore")):
            base = SCALARS.get(m.group(2))
            if base:
                sizes[m.group(1)] = base[0]
    return sizes

def aliases():
    found = {}
    for path in list((HERE / "include").rglob("*.h")) + list((HERE / "src").rglob("*.cpp")):
        text = path.read_text(errors="ignore")
        for m in re.finditer(r"\busing\s+(\w+)\s*=\s*([^;{]+);", text):
            found[m.group(1)] = m.group(2).strip()
        for m in re.finditer(r"\btypedef\s+([^;{]*?)\(\s*\*\s*(\w+)\s*\)\s*\(([^;]*)\)\s*;", text):
            found[m.group(2)] = f"{m.group(1).strip()} (*)({m.group(3)})"
        for m in re.finditer(r"\btypedef\s+([\w\s:*<>]+?)\s+(\w+)\s*;", text):
            found.setdefault(m.group(2), m.group(1).strip())
    return found

class Types:
    def __init__(self, ps2, native):
        self.ps2, self.native, self.enums, self.aliases = ps2, native, enum_bases(), aliases()

    @staticmethod
    def clean(t):
        t = re.sub(r"\b(const|volatile|struct|class|union|enum|typename)\b", "", t)
        return re.sub(r"\s+", " ", t).strip()

    def record(self, t):
        """The layouts of a record type by the name the dumps give it, on both targets"""
        # A record's own dump calls an anonymous member's type "unnamed", its parent's line "anonymous"
        t = t.replace("(anonymous at", "(unnamed at")
        for key in (t, "struct " + t, "class " + t, "union " + t):
            if key in self.ps2 and key in self.native:
                return self.ps2[key], self.native[key]
        # Namespaced names print without their namespace in field types (or the other way)
        tail = t.split("::")[-1]
        hits = [k for k in self.ps2 if k.split(" ", 1)[-1].split("::")[-1] == tail and k in self.native]
        if len(hits) == 1:
            return self.ps2[hits[0]], self.native[hits[0]]
        return None

    def parse(self, t):
        """("array", elem, count) | ("pointer",) | ("scalar", ps2size, nativesize, kind) | ("record", ps2, native) | None"""
        t = t.strip()
        m = re.match(r"^(.*)\(\s*\*\s*(?:const\s*)?\[(\d*)\]\s*\)\s*(\(.*\))$", t)
        if m:
            return ("array", f"{m.group(1).strip()} (*){m.group(3)}", int(m.group(2)) if m.group(2) else None)
        m = re.match(r"^(.*?)\s*\[(\d*)\]$", t)
        if m:
            return ("array", m.group(1), int(m.group(2)) if m.group(2) else None)
        if "(*)" in t or "(&)" in t or re.search(r"\*\s*(const|volatile)?\s*$", t) or t.endswith("&"):
            return ("pointer",)
        c = self.clean(t)
        if c in SCALARS:
            return ("scalar",) + SCALARS[c]
        if c in self.aliases and self.aliases[c] != c and not self.record(c):
            return self.parse(self.aliases[c])
        if c in self.enums:
            return ("scalar", self.enums[c], self.enums[c], "int")
        r = self.record(c)
        if r:
            return ("record",) + r
        # A plain enum (int sized) the dumps don't list
        if re.search(r"\benum\b", t):
            return ("scalar", 4, 4, "int")
        return None

    def native_offset(self, t, ps2_offset):
        """Where a PS2 offset inside an object of type t is natively (a member's start), None when it's inside a member"""
        if ps2_offset == 0:
            return 0
        p = self.parse(t)
        if p is None or p[0] in ("pointer", "scalar"):
            return None
        if p[0] == "array":
            es, en, _, _ = self.sizes(p[1])
            index, rest = divmod(ps2_offset, es)
            inner = self.native_offset(p[1], rest)
            return None if inner is None else index * en + inner
        ps2, native = p[1], p[2]
        if not self.has_pointers(t):
            return ps2_offset
        pf, nf = ps2["fields"], native["fields"]
        best = None
        for (po, pbits, pd, pbody), (no, nbits, nd, nbody) in zip(pf, nf):
            if pd == 1 and not pbits and po <= ps2_offset:
                best = (po, no, split_field(pbody)[0])
        if best is None:
            return None
        inner = self.native_offset(best[2], ps2_offset - best[0])
        return None if inner is None else best[1] + inner

    def register_nested(self, ftype, pf, nf, pstart, nstart):
        """A member's record type whose layout only shows nested in its parent's dump (a template's instance): taken from
        the lines below the member, on both targets"""
        def sub(fields, start):
            offset, depth = start
            i = next(k for k, f in enumerate(fields) if f[0] == offset and f[2] == depth and split_field(f[3])[0] == ftype)
            lines = []
            for f in fields[i + 1:]:
                if f[2] <= depth:
                    break
                lines.append((f[0] - offset, f[1], f[2] - depth, f[3]))
            end = max((f[0] + 8 for f in lines), default=0)
            return {"size": end, "align": 4, "fields": lines, "union": False}
        try:
            ps2, native = sub(pf, pstart), sub(nf, nstart)
        except StopIteration:
            return
        if ps2["fields"]:
            name = self.clean(ftype)
            self.ps2[name], self.native[name] = ps2, native

    def sizes(self, t):
        p = self.parse(t)
        if p is None:
            raise KeyError(t)
        if p[0] == "pointer":
            return 4, 8, 4, 8
        if p[0] == "scalar":
            return p[1], p[2], min(p[1], 8) if p[1] < 16 else 16, min(p[2], 16)
        if p[0] == "record":
            return p[1]["size"], p[2]["size"], p[1]["align"], p[2]["align"]
        es, en, ea, na = self.sizes(p[1])
        return es * (p[2] or 0), en * (p[2] or 0), ea, na

    def has_pointers(self, t, seen=()):
        p = self.parse(t)
        if p is None:
            raise KeyError(t)
        if p[0] == "pointer":
            return True
        if p[0] == "scalar":
            return p[3] in ("uptr", "sptr")
        if p[0] == "array":
            return self.has_pointers(p[1], seen)
        return p[1]["size"] != p[2]["size"] or any(
            self.has_pointers(split_field(f[3])[0], seen) for f in direct_fields(p[1]) if split_field(f[3])[0] not in seen)

def split_field(body):
    """("type", "name") of a dump's member line; an anonymous struct/union member is all type"""
    body = body.strip()
    if body.endswith(")") and "(anonymous" in body or "(unnamed" in body and body.endswith(")"):
        return body, None
    if " " not in body:
        return body, None
    return body.rsplit(" ", 1)

def direct_fields(layout):
    """The record's own members (depth 1), as (offset, bits, depth, "type name"); bases are flattened in by the dump"""
    return [f for f in layout["fields"] if f[2] == 1]

# ---------------------------------------------------------------------------------------------------------------- converting
class Converter:
    def __init__(self, types, objects):
        self.types, self.objects = types, objects
        self.problems = []
        self.last_type = None
        # What each symbol data points at is, by the pointer's type ("const char" for text): {symbol: {pointee types}}
        self.pointees = collections.defaultdict(set)

    def convert(self, label, t):
        """(native bytes, align, fixups [(offset, symbol, addend)]) of the object as type t"""
        obj = self.objects[label]
        data, relocs = obj["data"], obj["relocs"]
        p = self.types.parse(t)
        if self.types.parse(t) is None and not relocs and not any(data):
            # Zero storage (.bss) of a type without a layout dump (a template only a .cpp instantiates, a platform type):
            # room for it whatever its pointers make of it, at most twice the PS2's size
            size = len(data) * 2 + 16
            return bytearray(size), 16, []
        if p and p[0] == "array" and p[2] is None:
            es, en, ea, na = self.types.sizes(p[1])
            count = len(data) // es if es else 0
            element = self.types.clean(p[1])
            if element == "char":
                count = data.index(0) + 1 if 0 in data else len(data)
            elif self.types.has_pointers(p[1]):
                count = self.whole_elements(p[1], es, data, relocs, count)
            t = f"{p[1]} [{count}]"
        self.last_type = t
        ps2_size, native_size, _, native_align = self.types.sizes(t)
        out = bytearray(native_size)
        fixups = []
        self.used_relocs = set()
        self.place(label, t, data, relocs, 0, out, 0, fixups)
        for off in relocs:
            if off not in self.used_relocs and off < ps2_size:
                self.problems.append(f"{label}: the pointer at +{off:#x} ({relocs[off][0]}) isn't a pointer field of {t}")
        return out, max(native_align, 16 if obj["section"] in ("vutext", "vudata") else 4), fixups

    def whole_elements(self, element, size, data, relocs, count):
        """How many elements of an unsized array are its own: up to the first holding a number where a pointer goes (another,
        unlabelled object follows, which the C++ can't name)"""
        for i in range(count):
            probe = Converter(self.types, self.objects)
            probe.used_relocs = set()
            probe.place("probe", element, data, relocs, i * size, bytearray(self.types.sizes(element)[1]), 0, [])
            if any("without a symbol" in problem for problem in probe.problems):
                return i
        return count

    def place(self, label, t, data, relocs, src, out, dst, fixups):
        p = self.types.parse(t)
        if p is None:
            self.problems.append(f"{label}: no layout for type {t!r}")
            return
        kind = p[0]
        if kind == "pointer":
            if src in relocs:
                self.used_relocs.add(src)
                fixups.append((dst, *relocs[src]))
                pointee = re.sub(r"\*\s*(const|volatile)?\s*$", "", t).strip()
                if "(" not in t:
                    self.pointees[relocs[src][0]].add(pointee)
            else:
                value = struct.unpack_from("<I", data, src)[0] if src + 4 <= len(data) else 0
                if value != 0:
                    self.problems.append(f"{label}+{src:#x}: a pointer of {value:#x} without a symbol")
            return
        if kind == "scalar":
            ps2, native, k = p[1], p[2], p[3]
            if k in ("uptr", "sptr"):
                if src in relocs:
                    self.used_relocs.add(src)
                    fixups.append((dst, *relocs[src]))
                    return
                value = struct.unpack_from("<I" if k == "uptr" else "<i", data, src)[0] if src + 4 <= len(data) else 0
                out[dst:dst + 8] = struct.pack("<Q" if k == "uptr" else "<q", value)
                return
            if src in relocs:
                # An address kept in an integer the C++ doesn't widen: it must be widened (uiptr)
                self.used_relocs.add(src)
                self.problems.append(f"{label}+{src:#x}: the address of {relocs[src][0]} in a {t}")
            out[dst:dst + native] = data[src:src + ps2].ljust(native, b"\0")
            return
        if kind == "array":
            es, en, _, _ = self.types.sizes(p[1])
            if not self.types.has_pointers(p[1]):
                n = es * p[2]
                for off in range(src, src + n, 4):
                    if off in relocs:
                        self.used_relocs.add(off)
                        if label not in KNOWN_RAW:
                            self.problems.append(f"{label}+{off:#x}: the address of {relocs[off][0]} in {t}")
                out[dst:dst + n] = data[src:src + n].ljust(n, b"\0")
                return
            for i in range(p[2]):
                self.place(label, p[1], data, relocs, src + i * es, out, dst + i * en, fixups)
            return
        ps2, native = p[1], p[2]
        if not self.types.has_pointers(t):
            n = ps2["size"]
            for off in range(src, src + n, 4):
                if off in relocs:
                    self.used_relocs.add(off)
                    self.problems.append(f"{label}+{off:#x}: the address of {relocs[off][0]} in {t}")
            out[dst:dst + n] = data[src:src + n].ljust(n, b"\0")
            return
        # Field by field: the two dumps list the same members in the same order
        pf, nf = ps2["fields"], native["fields"]
        if len(pf) != len(nf):
            self.problems.append(f"{label}: {t} has {len(pf)} fields on the PS2 and {len(nf)} natively")
            return
        covered = set()
        for (po, pbits, pd, pbody), (no, nbits, nd, nbody) in zip(pf, nf):
            if pd != 1:
                continue
            ftype = split_field(pbody)[0]
            if self.types.parse(ftype) is None:
                self.types.register_nested(ftype, pf, nf, (po, pd), (no, nd))
            if pbits:
                # Bitfields: their storage unit copied whole (pointer-free by construction)
                unit = po
                if unit not in covered:
                    covered.add(unit)
                    out[dst + no:dst + no + 4] = data[src + unit:src + unit + 4]
                continue
            # Unions: the first member placed decides (the dump lists them all at one offset)
            if po in covered and native.get("union"):
                continue
            covered.add(po)
            self.place(label, ftype, data, relocs, src + po, out, dst + no, fixups)

def pointee_type(conv, symbol, objects, problems):
    """The type of an object only reached through pointers: what they point to (as an array when there's room for more than
    one), or its bytes when it holds no pointers and nothing says more (void*, a function's)"""
    kinds = {re.sub(r"\b(const|volatile)\b", "", k).strip() for k in conv.pointees.get(symbol, set())} - {"void", ""}
    size = len(objects[symbol]["data"])
    if len(kinds) == 1:
        kind = kinds.pop()
        try:
            es = conv.types.sizes(kind)[0]
        except KeyError:
            problems.append(f"{symbol}: pointed to as {kind!r}, which has no layout")
            return None
        return kind if es >= size or es == 0 else f"{kind} [{size // es}]"
    if objects[symbol]["relocs"]:
        problems.append(f"{symbol}: holds pointers, and what points at it says nothing of its type ({sorted(kinds) or 'void'})")
        return None
    return f"u8 [{size}]"

# ---------------------------------------------------------------------------------------------------------------- output
def cxx_escape(label):
    return re.sub(r"\W", "_", label)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", action="store_true", help="print every object's type and the problems, write nothing")
    args = parser.parse_args()

    objects, order = read_split()
    types_info = json.loads((BUILD / "data_types.json").read_text())
    ps2, native = dump_layouts("ps2"), dump_layouts("native")
    for layouts in (ps2, native):
        for name, layout in layouts.items():
            layout["union"] = name.startswith("union ")
    types = Types(ps2, native)
    conv = Converter(types, objects)

    # What the native build needs: every retail variable the C++ declares and doesn't define, then whatever their pointers reach
    wanted = collections.OrderedDict()
    for label, info in types_info.items():
        if info["kind"] == "var" and not info.get("defined") and label in objects:
            wanted[label] = info["canonical"]
    missing_from_split = sorted(l for l, i in types_info.items()
                                if i["kind"] == "var" and not i.get("defined") and l not in objects)

    results, problems, warnings = {}, [], []
    aliases = {}  # label -> (container label, PS2 offset)
    by_section = collections.defaultdict(list)
    for label in order:
        if objects[label]["address"] is not None:
            by_section[objects[label]["section"]].append(label)
    position = {label: (objects[label]["section"], i) for section, labels in by_section.items() for i, label in enumerate(labels)}
    for label in sorted(wanted, key=lambda l: objects[l]["address"] if objects[l]["address"] is not None else -1):
        obj = objects[label]
        if label in aliases or obj["address"] is None:
            continue
        try:
            extent = types.sizes(wanted[label])[0] if types.parse(wanted[label]) and types.parse(wanted[label])[0] != "array" \
                or (types.parse(wanted[label]) and types.parse(wanted[label])[2] is not None) else 0
        except KeyError:
            continue
        if extent <= len(obj["data"]):
            continue
        section, index = position[label]
        labels = by_section[section]
        for following in labels[index + 1:]:
            other = objects[following]
            offset = other["address"] - obj["address"]
            if offset >= extent:
                break
            # The bytes between, then the following object's own, its pointers moved along
            obj["data"] += bytes(offset - len(obj["data"])) if offset > len(obj["data"]) else b""
            for at, target in other["relocs"].items():
                obj["relocs"][offset + at] = target
            obj["data"][offset:offset + len(other["data"])] = other["data"]
            aliases[following] = (label, offset)
    queue = [label for label in wanted if label not in aliases]
    inferred = {}
    while queue:
        label = queue.pop(0)
        if label in results:
            continue
        t = wanted.get(label) or inferred.get(label)
        try:
            out, align, fixups = conv.convert(label, t)
        except KeyError as error:
            problems.append(f"{label}: no layout for {error}")
            continue
        results[label] = (t, out, align, fixups)
        obj = objects[label]
        ps2_covered = conv.types.sizes(conv.last_type)[0] if conv.last_type else len(obj["data"])
        if obj.get("pads") and ps2_covered < len(obj["data"]) and (obj["relocs"] or any(obj["data"][ps2_covered:])):
            warnings.append(f"{label}: {len(obj['data']) - ps2_covered} bytes past its type {conv.last_type} (a splat pad) "
                            f"aren't brought over")
        for _, symbol, _ in fixups:
            if symbol in objects and symbol not in results and symbol not in wanted and symbol not in inferred \
                    and symbol not in aliases:
                # Data only other data points at: its type is the pointer's
                inferred[symbol] = pointee_type(conv, symbol, objects, problems)
                if inferred[symbol] is not None:
                    queue.append(symbol)
    alias_offsets = {}
    # Only what's named needs a symbol: by the C++, or by a pointer in the data converted
    named = set(types_info) | {symbol for (_, _, _, fx) in results.values() for _, symbol, _ in fx}
    for label, (container, offset) in aliases.items():
        if container not in results or label not in named:
            continue
        native = types.native_offset(results[container][0], offset)
        if native is None:
            problems.append(f"{label}: inside {container} at +{offset:#x}, which isn't a member's start natively")
        else:
            alias_offsets[label] = (container, native)
    problems += conv.problems

    if args.report:
        for label, (t, out, align, fixups) in results.items():
            print(f"{label:40} {t!s:40} {len(objects[label]['data']):6} -> {len(out):6} fixups {len(fixups)}")
        print(f"\n{len(results)} objects, {sum(len(r[3]) for r in results.values())} fixups, "
              f"{len(missing_from_split)} declared but not in the split, {len(problems)} problems")
        for p in problems:
            print("PROBLEM", p)
        return

    for w in warnings:
        print("WARNING", w, file=sys.stderr)
    write_cpp(results, objects, alias_offsets)
    if problems:
        for p in problems:
            print("PROBLEM", p, file=sys.stderr)
        sys.exit(f"{len(problems)} problems (see above)")

def write_cpp(results, objects, aliases):
    lines = ["// Generated by native/tools/convert_data.py from the split's data: don't edit",
             "#include <stdint.h>", "#include <string.h>", "",
             "// The retail names as symbols (common.h's RETAIL): Mach-O's start with an underscore",
             "#ifdef __APPLE__", '#define SYMBOL(name) "_" name', "#else", "#define SYMBOL(name) name", "#endif", ""]
    symbols = sorted({s for (_, _, _, fx) in results.values() for _, s, _ in fx})
    lines.append('extern "C"\n{')
    for s in symbols:
        if s not in results and s not in aliases:
            lines.append(f'extern char x_{cxx_escape(s)} asm(SYMBOL("{s}"));')
    for label, (t, out, align, fixups) in results.items():
        body = ",".join(str(b) for b in out) if any(out) else ""
        lines.append(f'// {t}\nalignas({align}) unsigned char x_{cxx_escape(label)}[{max(len(out), 1)}] asm(SYMBOL("{label}")) = {{{body}}};')
    lines.append("}\n")
    # Labels inside another object (the PS2's neighbouring globals a C++ struct covers): symbols at their native offsets in it
    for label, (container, offset) in sorted(aliases.items()):
        lines.append(f'asm(".globl " SYMBOL("{label}") "\\n.set " SYMBOL("{label}") ", " SYMBOL("{container}") " + {offset}\\n");')
    for label in aliases:
        lines.append(f'extern "C" char x_{cxx_escape(label)} asm(SYMBOL("{label}"));')
    lines.append("\nnamespace\n{\nstruct Fixup\n{\n    unsigned char* at;\n    const void* to;\n};\n")
    lines.append("const Fixup g_Fixups[] = {")
    for label, (t, out, align, fixups) in results.items():
        for off, symbol, addend in fixups:
            target = f"x_{cxx_escape(symbol)}"
            lines.append(f"    {{x_{cxx_escape(label)} + {off}, reinterpret_cast<const char*>(&{target}) + ({addend})}},")
    lines.append("};\n}\n")
    lines.append('extern "C" void NativeApplyDataFixups()\n{\n    for (const Fixup& fixup : g_Fixups)\n    {\n'
                 '        memcpy(fixup.at, &fixup.to, sizeof(fixup.to));\n    }\n}')
    (BUILD / "data.cpp").write_text("\n".join(lines) + "\n")
    print(f"build/native/data.cpp: {len(results)} objects, {len(aliases)} inside others, "
          f"{sum(len(r[3]) for r in results.values())} fixups")

if __name__ == "__main__":
    main()
