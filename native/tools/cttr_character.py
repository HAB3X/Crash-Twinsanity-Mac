#!/usr/bin/env python3
"""Crash Tag Team Racing's on-foot Crash (Radical Entertainment's Pure3D engine, the PS2 disc) made into a character pack for the
native port, in the same format as native/tools/woc_character.py's (native/CHARACTERS.md): his model (mesh, textures, bone
weights), his skeleton and his on-foot animations, read from your own disc image and written to the port's settings folder.
Nothing of the game's goes into the repository.

    python3 native/tools/cttr_character.py "/path/to/Crash Tag Team Racing.iso"
        [--out DIR] [--costume NAME] [--preview DIR] [--max-texture N] [--list]

The image is the disc's .iso (or a raw 2352-byte-sector .bin). --out defaults to the settings folder's characters/cttr_crash/
(characters/cttr_crash_<costume>/ for another costume; $HOME/Library/Application Support/Crash Twinsanity on macOS,
$XDG_CONFIG_HOME/crash-twinsanity or ~/.config/crash-twinsanity elsewhere). --costume picks the outfit: classic (the default,
art\\characters\\crash_onfoot_model.p3d), or blacktie, buckethead, caveman, hiphop, madscientist, naked, nega, raiders, skeleton,
startrek, japan, japan_blacktie and the other Japanese ones (--list prints them). --preview writes PNGs of a few frames and
animated GIFs of the idle, run, walk and jump (needs numpy and Pillow), drawn from the pack's own data. --max-texture N halves
textures until no side is over N texels (the classic costume's is 256x256; by default they keep their full size). --list prints
the costumes, the animation clips and the archives they were found in.

The decoding (worked out on the USA disc, SLUS_211.91; native/CHARACTERS_CTTR.md has the notes):
- RCF ("ATG CORE CEMENT LIBRARY", version 2.1): a 32-byte signature, version bytes, then the directory's offset and size, the
  name table's offset and size, a word, the file count. The directory is (hash, offset, size) a file, sorted by hash; the name
  table has, a file, a time stamp, the alignment, a word, the name's length (with its 0), the name and three bytes of padding. A
  name's hash is h = h * 31 + toupper(c) + 32 over its characters (32 bits); that ties the names to the directory.
  ADEFAULT/DEFAULT.RCF holds every costume and the animations; the per-world RCFs hold copies.
- Pure3D: chunks of (id, header size, total size), the header's data first, then the children. The ones read here: texture
  0x19000 > image 0x19001 > image data 0x19002 (a "P3DI" block: format 14, size, depth, palette size, texel size, the palette
  (32 bit, the 8 bit CLUT's bits 3 and 4 swapped, alpha 0x80 opaque), then the 8 bit texels as the GS stores them when uploaded as
  32 bit pixels at half the width and height); shader 0x11000 (its texture 0x11002); skeleton 0x23000 > joints 0x23001 (name,
  parent, rest pose: a 4x4 matrix in its parent's space, row vectors); skin 0x10001 > primitive groups 0x10020 (shader, triangle
  strip, vertex and matrix counts) > the PS2 memory image 0x10012 (VIF packets: batches of up to 48 strip vertices, a batch's
  first two repeating the last batch's last two, each vertex a quadword a slot: 3 u v (V2-32; u's lowest bit the strip's ADC,
  set on a new strip's first two vertices), 4 the normal (V3-16, 4096 is 1; or a baked vertex colour, V4-5, left out and the
  normals made from the faces), 5 the position (V3-32), 6 four matrix slots (V4-8, 4 a slot: the matrix palette's row), 7 three
  weights (V4-16, 4096 is 1; the fourth slot takes 1 minus the three; none in a rigid group)) and the matrix palette 0x1000D
  (the palette's joints); animation 0x121000 (name, "PTRN", frames, rate) > group list 0x121002 > groups 0x121001 (a joint's
  name) > channels: rotation as compressed quaternions 0x121112 (key frames, then x y z a key as 16 bit fractions, w the
  positive rest) and translation 0x121102/3/4 (1, 2 or 3 animated coordinates around constants). Skins named for the shadow are
  left out (the Japanese models carry one).
- The animations name joints and the rig's IK controls (Right_Foot, Left_Arm, Right_Knee...); the controls are left out, every
  joint has its own rotation key frames. Motion_Root's translation (the run's distance covered) is dropped, so loops play in
  place; a loop's last frame, a copy of its first, is dropped; the mouth flap (a partial layer) is filled from the idle; the
  spin animates the separate spin model and is left out.
- The pack is in Wrath of Cortex's pack space (woc_character.py's): Y up, facing -Z, his left at +X, about 1 tall, the feet at 0.
  Pure3D's Crash has the same axes at 2.58 units tall (to the hair's tip), so the conversion only scales, by SCALE. The clips doing what the port's mapping shows by Wrath
  of Cortex clip names (b/idle, a/run, b/jump...) are copied under those names too (WOC_NAMES).
"""
import argparse
import math
import os
import struct
import sys
from pathlib import Path

PACK_VERSION = 1
SCALE = 0.386
MOTION_ROOT = "Motion_Root"
COSTUME_PREFIX = "art\\characters\\"
ANIMATIONS = "art\\animation\\crash_onfoot_animations.p3d"
# The Japanese release's Crash (the japan costumes) has his own face rig and animations
ANIMATIONS_JAPAN = "art\\animation\\crash_japan_onfoot_animations.p3d"


# ----------------------------------------------------------------------------------------------------------------------------
# The disc: ISO 9660 on 2048-byte sectors, or on raw 2352-byte ones (the user data after the sync, header and, for MODE2, the
# subheader)

class Disc:
    def __init__(self, path):
        self.file = open(path, "rb")
        head = self.file.read(16)
        if head[:12] == b"\x00" + b"\xff" * 10 + b"\x00":
            self.sector = 2352
            self.data = 24 if head[15] == 2 else 16
        else:
            self.sector = 2048
            self.data = 0

    def read(self, lba, size):
        if self.sector == 2048:
            self.file.seek(lba * 2048)
            return self.file.read(size)
        out = bytearray()
        sector = lba
        while len(out) < size:
            self.file.seek(sector * self.sector + self.data)
            out += self.file.read(2048)
            sector += 1
        return bytes(out[:size])

    def files(self):
        """Every file's path ('DIR/NAME', the version dropped) and its extent"""
        descriptor = self.read(16, 2048)
        if descriptor[1:6] != b"CD001":
            raise SystemExit("not an ISO 9660 disc image")
        found = {}

        def walk(lba, size, prefix):
            data = self.read(lba, size)
            at = 0
            while at < len(data):
                length = data[at]
                if length == 0:
                    at = (at // 2048 + 1) * 2048
                    continue
                record = data[at:at + length]
                at += length
                name = record[33:33 + record[32]]
                if name in (b"\x00", b"\x01"):
                    continue
                extent, extent_size = struct.unpack_from("<I", record, 2)[0], struct.unpack_from("<I", record, 10)[0]
                name = name.decode("ascii").split(";")[0]
                if record[25] & 2:
                    walk(extent, extent_size, prefix + name + "/")
                else:
                    found[prefix + name] = (extent, extent_size)

        root = descriptor[156:156 + 34]
        walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")
        return found


# ----------------------------------------------------------------------------------------------------------------------------
# RCF archives

def rcf_hash(name):
    """h = h * 31 + toupper(c) + 32 over the name, a leading backslash left out (COMMON.RCF stores two names with one)"""
    value = 0
    for c in name.lstrip("\\/").upper():
        value = (value * 31 + ord(c) + 32) & 0xFFFFFFFF
    return value


class Rcf:
    def __init__(self, disc, lba, label):
        self.disc, self.lba, self.label = disc, lba, label
        head = disc.read(lba, 0x40)
        if not head.startswith(b"ATG CORE CEMENT LIBRARY") or head[0x20:0x22] != b"\x02\x01":
            raise ValueError("%s: not an RCF 2.1 archive" % label)
        directory_at, _, names_at, names_size, _, count = struct.unpack_from("<6I", head, 0x24)
        table = disc.read(lba, names_at + names_size)
        directory = {}
        for i in range(count):
            key, offset, size = struct.unpack_from("<3I", table, directory_at + i * 12)
            directory[key] = (offset, size)
        self.files = {}
        at = names_at + 8
        for _ in range(count):
            length, = struct.unpack_from("<I", table, at + 12)
            name = table[at + 16:at + 15 + length].decode("latin-1")
            at += 16 + length + 3
            if rcf_hash(name) in directory:
                self.files[name.lower()] = (name, *directory[rcf_hash(name)])

    def read(self, name):
        _, offset, size = self.files[name.lower()]
        return self.disc.read(self.lba + offset // 2048, offset % 2048 + size)[offset % 2048:]


# ----------------------------------------------------------------------------------------------------------------------------
# Pure3D chunks

def chunks(data, start, end):
    """(id, where its data starts, where its children start, where it ends) for each chunk in [start, end)"""
    at = start
    while at + 12 <= end:
        ident, header, total = struct.unpack_from("<III", data, at)
        if total < 12 or at + total > end:
            raise ValueError("bad chunk %#x at %#x" % (ident, at))
        yield ident, at + 12, at + header, at + total
        at += total


def string(data, at):
    """A Pure3D string (a length byte, then the text padded with zeros) and where the next field starts"""
    length = data[at]
    return data[at + 1:at + 1 + length].split(b"\0")[0].decode("latin-1"), at + 1 + length


def matrix_product(a, b):
    """Row-major 4x4 product (row vectors: a then b)"""
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inverse_affine(m):
    """The inverse of a row-vector affine matrix (a 3x3 block and a translation row)"""
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3] -
             a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)] for i in range(3)]
    t = m[3][:3]
    out = [inv[i] + [0.0] for i in range(3)]
    out.append([-sum(t[k] * inv[k][j] for k in range(3)) for j in range(3)] + [1.0])
    return out


# The GS's local memory layouts, for 8 bit textures stored as they sit after an upload as 32 bit pixels (the GS User's Manual's
# page, block and column arrangements; src/platform/native/graphics/gs/gsmemory.cpp has the same)

COLUMN8 = [
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255],
]


def block32(bx, by):
    return (bx & 1) | (by & 1) << 1 | (bx & 2) << 1 | (by & 2) << 2 | (bx & 4) << 2


def address32(x, y, width):
    """A 32 bit pixel's word in GS memory (a buffer at 0, width in 64 pixels)"""
    page = (y >> 5) * width + (x >> 6)
    column = (x & 1) | (y & 1) << 1 | (x & 6) << 1 | (y & 6) << 3
    return (page * 32 + block32((x >> 3) & 7, (y >> 3) & 3)) << 6 | column


def address8(x, y, width):
    """An 8 bit pixel's byte in GS memory (a buffer at 0, width in 64 pixels)"""
    page = (y >> 6) * (width >> 1) + (x >> 7)
    return (page * 32 + block32((x >> 4) & 7, (y >> 4) & 3)) << 8 | COLUMN8[y & 15][x & 15]


# ----------------------------------------------------------------------------------------------------------------------------
# The model (a costume's _onfoot_model.p3d)

class Model:
    """Its textures (RGBA rows, by name), shaders (their textures), skeleton (joints with rest poses and bind matrices) and
    meshes (a primitive group's vertices and triangles)"""

    def __init__(self, data):
        self.data = data
        self.textures = {}
        self.shaders = {}
        self.joints = []
        self.meshes = []
        skeletons = {}
        skins = []
        for ident, at, children, end in chunks(data, 12, len(data)):
            if ident == 0x19000:
                self.read_texture(at, children, end)
            elif ident == 0x11000:
                self.read_shader(at, children, end)
            elif ident == 0x23000:
                name, _ = string(data, at)
                skeletons[name] = self.read_skeleton(children, end)
            elif ident == 0x10001:
                skins.append((at, children, end))
        # The skins drawn: the ones not named for the shadow (the Japanese models carry a shadow skin of their own as well as
        # the 0x10019 shadow)
        drawn = [skin for skin in skins if "shadow" not in string(data, skin[0])[0].lower()] or skins[:1]
        if not drawn:
            raise ValueError("no skin in the model")
        for at, children, end in drawn:
            _, p = string(data, at)
            skeleton_name, _ = string(data, p + 4)
            if skeleton_name not in skeletons:
                raise ValueError("skin's skeleton %s missing" % skeleton_name)
            if self.joints and self.joints is not skeletons[skeleton_name]:
                raise ValueError("skins on two skeletons")
            self.joints = skeletons[skeleton_name]
            for ident, sub, sub_children, sub_end in chunks(data, children, end):
                if ident == 0x10020:
                    self.read_primitive_group(sub, sub_children, sub_end)

    def read_texture(self, at, children, end):
        data = self.data
        name, _ = string(data, at)
        for ident, image_at, image_children, image_end in chunks(data, children, end):
            if ident != 0x19001:
                continue
            _, p = string(data, image_at)
            _, width, height, _, _, has_alpha, _ = struct.unpack_from("<7I", data, p)
            for ident2, raw, _, _ in chunks(data, image_children, image_end):
                if ident2 != 0x19002:
                    continue
                magic, storage, w, h, bits, palette_size, texel_size = struct.unpack_from("<4sIIIIII", data, raw + 4)
                if magic != b"P3DI" or storage != 14 or bits != 8 or palette_size != 1024 or (w, h) != (width, height):
                    raise ValueError("texture %s: image format %r %d %dx%d %d bit not handled" % (
                        name, magic, storage, w, h, bits))
                palette_at = raw + 4 + 28
                texels_at = palette_at + palette_size
                palette = [data[palette_at + i * 4:palette_at + i * 4 + 4] for i in range(256)]
                # The CLUT's 32 bit 16x16 layout swaps index bits 3 and 4; alpha 0x80 is opaque
                palette = [palette[(i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1] for i in range(256)]
                palette = [bytes((c[0], c[1], c[2], min(255, c[3] * 2))) for c in palette]
                upload_width, upload_height = max(1, width // 2), max(1, height // 2)
                buffer_width = max(1, upload_width // 64)
                memory = bytearray((address32(upload_width - 1, upload_height - 1, buffer_width) + 1) * 4 + 0x2000)
                for y in range(upload_height):
                    for x in range(upload_width):
                        source = texels_at + (y * upload_width + x) * 4
                        target = address32(x, y, buffer_width) * 4
                        memory[target:target + 4] = data[source:source + 4]
                indexes = bytes(memory[address8(x, y, buffer_width * 2)] for y in range(height) for x in range(width))
                self.textures[name] = {"width": width, "height": height, "alpha": bool(has_alpha),
                                       "rgba": b"".join(palette[i] for i in indexes)}

    def read_shader(self, at, children, end):
        data = self.data
        name, p = string(data, at)
        _, p = struct.unpack_from("<I", data, p)[0], p + 4
        _, p = string(data, p)
        translucent, = struct.unpack_from("<I", data, p)
        texture = None
        integers = {}
        colour = (1.0, 1.0, 1.0)
        for ident, sub, _, _ in chunks(data, children, end):
            parameter = data[sub:sub + 4].rstrip(b"\0").decode("latin-1")
            if ident == 0x11002 and parameter == "TEX":
                texture, _ = string(data, sub + 4)
                texture = texture or None
            elif ident == 0x11003:
                integers[parameter], = struct.unpack_from("<I", data, sub + 4)
            elif ident == 0x11005 and parameter == "DIFF":
                # A colour word, 0xAARRGGBB
                argb, = struct.unpack_from("<I", data, sub + 4)
                colour = ((argb >> 16 & 0xFF) / 255.0, (argb >> 8 & 0xFF) / 255.0, (argb & 0xFF) / 255.0)
        self.shaders[name] = {"texture": texture, "colour": colour,
                              "blend": bool(translucent) or integers.get("BLMO", 0) != 0}

    def read_skeleton(self, children, end):
        data = self.data
        joints = []
        for ident, at, _, _ in chunks(data, children, end):
            if ident != 0x23001:
                continue
            name, p = string(data, at)
            parent, = struct.unpack_from("<I", data, p)
            values = struct.unpack_from("<16f", data, p + 4)
            rest = [list(values[r * 4:r * 4 + 4]) for r in range(4)]
            index = len(joints)
            joint = {"name": name, "parent": parent if parent != index else -1, "rest": rest}
            joint["bind"] = (matrix_product(rest, joints[parent]["bind"]) if joint["parent"] >= 0 else rest)
            joint["inverse"] = inverse_affine(joint["bind"])
            joints.append(joint)
        return joints

    def read_primitive_group(self, at, children, end):
        data = self.data
        _, p = struct.unpack_from("<I", data, at)[0], at + 4
        shader, p = string(data, p)
        primitive, _, vertex_count = struct.unpack_from("<3I", data, p)
        if primitive != 1:
            raise ValueError("primitive type %d (a strip expected)" % primitive)
        stream = palette = None
        for ident, sub, _, _ in chunks(data, children, end):
            if ident == 0x10012:
                _, _, size = struct.unpack_from("<3I", data, sub)
                stream = (sub + 12, sub + 12 + size)
            elif ident == 0x1000D:
                count, = struct.unpack_from("<I", data, sub)
                palette = list(struct.unpack_from("<%dI" % count, data, sub + 4))
        if stream is None or palette is None:
            raise ValueError("primitive group without its PS2 memory image or matrix palette")
        mesh = {"shader": shader, "vertices": [], "triangles": [], "index": {}}
        batches = self.read_batches(*stream)
        for batch in batches:
            strip = []
            for k in range(len(batch["position"])):
                slots = [s // 4 for s in batch["matrices"][k]]
                if "weights" in batch:
                    amounts = [w / 4096.0 for w in batch["weights"][k][:3]]
                    amounts.append(1.0 - sum(amounts))
                else:
                    amounts = [1.0]
                weights = {}
                for slot, amount in zip(slots, amounts):
                    if abs(amount) > 1e-6:
                        joint = palette[slot]
                        weights[joint] = weights.get(joint, 0.0) + amount
                weights = tuple(sorted(((j, w) for j, w in weights.items() if w > 1e-4), key=lambda item: -item[1]))
                normal = tuple(n / 4096.0 for n in batch["normal"][k]) if "normal" in batch else (0.0, 0.0, 0.0)
                strip.append((tuple(batch["position"][k]), normal, tuple(batch["uv"][k]), weights))
            for i in range(2, len(strip)):
                if batch["adc"][i]:
                    continue
                corners = strip[i - 2:i + 1]
                if corners[0][0] == corners[1][0] or corners[1][0] == corners[2][0] or corners[0][0] == corners[2][0]:
                    continue
                mesh["triangles"].append(tuple(self.add_vertex(mesh, corner) for corner in corners))
        del mesh["index"]
        if any("normal" not in batch for batch in batches):
            self.smooth_normals(mesh)
        strip_vertices = sum(len(b["position"]) for b in batches) - 2 * (len(batches) - 1)
        if strip_vertices != vertex_count:
            raise ValueError("%d strip vertices decoded, %d expected" % (strip_vertices, vertex_count))
        self.meshes.append(mesh)

    @staticmethod
    def add_vertex(mesh, vertex):
        index = mesh["index"]
        if vertex not in index:
            index[vertex] = len(mesh["vertices"])
            mesh["vertices"].append(vertex)
        return index[vertex]

    @staticmethod
    def smooth_normals(mesh):
        """Normals for a group that has baked vertex colours in their place: its triangles' (by area), summed where vertices
        share a place. The strip's winding alternates, so the faces are first made to agree across their shared edges, then
        turned to face away from the group's middle"""
        vertices = mesh["vertices"]
        faces = []
        edges = {}
        for t, triangle in enumerate(mesh["triangles"]):
            a, b, c = (vertices[i][0] for i in triangle)
            u = [b[k] - a[k] for k in range(3)]
            w = [c[k] - a[k] for k in range(3)]
            faces.append((u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]))
            for i in range(3):
                edges.setdefault(frozenset((vertices[triangle[i]][0], vertices[triangle[(i + 1) % 3]][0])), []).append(t)
        sign = [0] * len(faces)
        for start in range(len(faces)):
            if sign[start]:
                continue
            sign[start] = 1
            queue = [start]
            while queue:
                t = queue.pop()
                triangle = mesh["triangles"][t]
                for i in range(3):
                    for other in edges[frozenset((vertices[triangle[i]][0], vertices[triangle[(i + 1) % 3]][0]))]:
                        if not sign[other]:
                            dot = sum(faces[t][k] * faces[other][k] for k in range(3))
                            sign[other] = sign[t] if dot >= 0 else -sign[t]
                            queue.append(other)
        middle = [sum(v[0][k] for v in vertices) / len(vertices) for k in range(3)]
        outward = sum(sign[t] * sum(faces[t][k] * (vertices[triangle[0]][0][k] - middle[k]) for k in range(3))
                      for t, triangle in enumerate(mesh["triangles"]))
        flip = -1 if outward < 0 else 1
        sums = {}
        for t, triangle in enumerate(mesh["triangles"]):
            for i in triangle:
                total = sums.setdefault(vertices[i][0], [0.0, 0.0, 0.0])
                for k in range(3):
                    total[k] += faces[t][k] * sign[t] * flip
        for i, (position, _, uv, weights) in enumerate(vertices):
            n = sums.get(position, [0.0, 1.0, 0.0])
            length = math.sqrt(sum(v * v for v in n)) or 1.0
            vertices[i] = (position, tuple(v / length for v in n), uv, weights)

    # The memory image's VIF codes: a batch starts with a V4-32 unpack of three quadwords to address 0 (the vertex count, ...,
    # the GIF tag); then, with STCYCL 5/1 (4/1 for a rigid group), one unpack a vertex attribute, a slot of the vertex each: 3 u v
    # (V2-32, u's lowest bit the strip's ADC: set on a new strip's first two vertices), 4 the normal (V3-16) or a baked vertex
    # colour (V4-5, left out), 5 the position (V3-32), 6 the matrix slots (V4-8, added to STROW's matrix base by STMOD 1) and 7
    # the weights (V4-16; none in a rigid group)
    def read_batches(self, start, end):
        data = self.data
        batches = []
        batch = None
        at = start
        kinds = {(3, 2, 0): "uv", (4, 3, 1): "normal", (4, 4, 3): "colour", (5, 3, 0): "position", (6, 4, 2): "matrices",
                 (7, 4, 1): "weights"}
        while at < end:
            code, = struct.unpack_from("<I", data, at)
            command = (code >> 24) & 0x7F
            immediate = code & 0xFFFF
            number = (code >> 16) & 0xFF
            at += 4
            if command & 0x60 == 0x60:
                components = ((command >> 2) & 3) + 1
                width = command & 3
                count = number or 256
                payload = at
                if width == 3:
                    length = 2 * count
                    rows = list(struct.unpack_from("<%dH" % count, data, at))
                else:
                    size = {0: 4, 1: 2, 2: 1}[width]
                    length = size * components * count
                    unsigned = bool(immediate & 0x4000)
                    kind = {0: "f", 1: "H" if unsigned else "h", 2: "B" if unsigned else "b"}[width]
                    values = struct.unpack_from("<%d%s" % (components * count, kind), data, at)
                    rows = [values[i * components:(i + 1) * components] for i in range(count)]
                at += (length + 3) // 4 * 4
                address = immediate & 0x3FF
                if address == 0:
                    batch = {}
                    batches.append(batch)
                elif batch is not None:
                    name = kinds.get((address, components, width))
                    if name is None:
                        raise ValueError("unknown vertex attribute V%d-%d at %d" % (
                            components, 32 >> width if width < 3 else 5, address))
                    batch[name] = rows
                    if name == "uv":
                        # The strip's ADC (no triangle ends at this vertex): u's lowest bit
                        batch["adc"] = [struct.unpack_from("<I", data, payload + i * 8)[0] & 1 for i in range(count)]
            elif command in (0x30, 0x31):
                at += 16
            elif command == 0x20:
                at += 4
            elif command in (0x50, 0x51):
                at += immediate * 16
            elif command == 0x4A:
                at += (number or 256) * 8
        for batch in batches:
            missing = {"uv", "position", "matrices"} - set(batch)
            if missing:
                raise ValueError("a batch without %s" % ", ".join(sorted(missing)))
        return batches


# ----------------------------------------------------------------------------------------------------------------------------
# Animations (crash_onfoot_animations.p3d)

class Animation:
    def __init__(self, data, at, children, end):
        self.name, p = string(data, at + 4)
        _, self.end, self.rate, _ = struct.unpack_from("<4sffI", data, p)
        self.groups = {}
        self.base = None
        for ident, _, list_children, list_end in chunks(data, children, end):
            if ident != 0x121002:
                continue
            for ident2, group_at, group_children, group_end in chunks(data, list_children, list_end):
                if ident2 != 0x121001:
                    continue
                name, _ = string(data, group_at + 4)
                channels = {}
                for ident3, channel, _, _ in chunks(data, group_children, group_end):
                    parameter = data[channel + 4:channel + 8].rstrip(b"\0").decode("latin-1")
                    if ident3 == 0x121112 and parameter == "ROT":
                        count, = struct.unpack_from("<I", data, channel + 8)
                        frames = struct.unpack_from("<%dH" % count, data, channel + 12)
                        raw = struct.unpack_from("<%dh" % (count * 3), data, channel + 12 + count * 2)
                        keys = []
                        for i in range(count):
                            x, y, z = (v / 32767.0 for v in raw[i * 3:i * 3 + 3])
                            keys.append((math.sqrt(max(0.0, 1.0 - x * x - y * y - z * z)), x, y, z))
                        channels["rotation"] = (frames, keys)
                    elif ident3 in (0x121102, 0x121103, 0x121104) and parameter == "TRAN":
                        channels["translation"] = self.read_translation(data, ident3, channel + 8)
                self.groups[name] = channels

    @staticmethod
    def read_translation(data, ident, at):
        if ident == 0x121104:
            count, = struct.unpack_from("<I", data, at)
            frames = struct.unpack_from("<%dH" % count, data, at + 4)
            values = struct.unpack_from("<%df" % (count * 3), data, at + 4 + count * 2)
            return frames, [values[i * 3:i * 3 + 3] for i in range(count)]
        index, = struct.unpack_from("<H", data, at)
        constants = struct.unpack_from("<3f", data, at + 2)
        count, = struct.unpack_from("<I", data, at + 14)
        frames = struct.unpack_from("<%dH" % count, data, at + 18)
        animated = 1 if ident == 0x121102 else 2
        values = struct.unpack_from("<%df" % (count * animated), data, at + 18 + count * 2)
        keys = []
        for i in range(count):
            value = list(constants)
            if animated == 1:
                value[index] = values[i]
            else:
                free = [k for k in range(3) if k != index]
                value[free[0]], value[free[1]] = values[i * 2], values[i * 2 + 1]
            keys.append(tuple(value))
        return frames, keys

    def frames(self):
        return max(1, int(round(self.end)))

    @staticmethod
    def sample(frames, keys, time, blend):
        if time <= frames[0]:
            return keys[0]
        for i in range(1, len(frames)):
            if frames[i] >= time:
                t = (time - frames[i - 1]) / max(frames[i] - frames[i - 1], 1e-9)
                return blend(keys[i - 1], keys[i], t)
        return keys[-1]

    def joint(self, name, time):
        """A joint's rotation (w x y z) and translation at a time, either None when the clip leaves it alone (a partial clip's
        base, the idle, has the joints it leaves out)"""
        if name not in self.groups and self.base is not None:
            return self.base.joint(name, time % max(self.base.frames(), 1))
        channels = self.groups.get(name, {})
        rotation = translation = None
        if "rotation" in channels:
            rotation = self.sample(*channels["rotation"], time, slerp)
        if "translation" in channels and name != MOTION_ROOT:
            translation = self.sample(*channels["translation"], time,
                                      lambda a, b, t: tuple(a[k] + (b[k] - a[k]) * t for k in range(3)))
        return rotation, translation


class Sequence:
    """Clips played one after another, as one (the jump's takeoff then its rise)"""

    def __init__(self, parts):
        self.parts = parts
        self.rate = parts[0].rate
        self.base = None

    def frames(self):
        return sum(part.frames() for part in self.parts)

    def joint(self, name, time):
        for part in self.parts[:-1]:
            if time < part.frames():
                return part.joint(name, time)
            time -= part.frames()
        return self.parts[-1].joint(name, time)


class Slice:
    """Some of a clip's frames, as a clip of their own"""

    def __init__(self, clip, start, count):
        self.clip, self.start, self.count = clip, start, count
        self.rate = clip.rate
        self.base = None

    def frames(self):
        return self.count

    def joint(self, name, time):
        return self.clip.joint(name, self.start + time)


# The port's mapping (src/platform/native/graphics/characters_mapping.inc) shows Twinsanity's animations as Wrath of Cortex
# clips, by their names: the Tag Team Racing clips doing the same are copied under those names too. A part is a clip's name, or
# (name, first frame, frame count). The spin: Tag Team Racing swaps in a separate tornado model for it, so the spin kick's
# arms-out start stands in (the port turns the model round for a/spin, as for WoC's spin pose). Twinsanity's crouch, crawl and
# slide have nothing like them in Tag Team Racing
WOC_NAMES = [
    ("b/idle", ["onfoot_idle"]),
    ("a/walk", ["onfoot_walk"]),
    ("a/run", ["onfoot_run"]),
    ("a/sprint", ["onfoot_run"]),
    ("a/spin", [("onfoot_spinkick_recover", 0, 5)]),
    ("b/jump", ["onfoot_jump_takeoff", "onfoot_jump_rise"]),
    ("a/runjump", ["onfoot_jump_takeoff", "onfoot_jump_rise"]),
    ("a/starjump", ["onfoot_doublejump_start"]),
    ("f/fall", ["onfoot_jump_fall"]),
    ("g/longfall", ["onfoot_jump_fall"]),
    ("a/land", ["onfoot_jump_landing"]),
    ("a/bodyslam", ["onfoot_bellyflop_antic", "onfoot_bellyflop_fall"]),
    ("b/die", ["onfoot_death"]),
    ("b/hngabout", ["onfoot_edgehang_idle"]),
    ("b/hngwalk", ["onfoot_edgehang_left_loop"]),
    ("b/hngpulup", ["onfoot_edgehang_springup", "onfoot_edgehang_springup_recover"]),
    ("b/itch", ["onfoot_itchy"]),
]


def slerp(a, b, t):
    dot = sum(a[k] * b[k] for k in range(4))
    if dot < 0:
        b, dot = tuple(-v for v in b), -dot
    if dot > 0.9995:
        out = [a[k] + (b[k] - a[k]) * t for k in range(4)]
    else:
        angle = math.acos(dot)
        sa, sb = math.sin((1 - t) * angle), math.sin(t * angle)
        out = [(a[k] * sa + b[k] * sb) / math.sin(angle) for k in range(4)]
    length = math.sqrt(sum(v * v for v in out))
    return tuple(v / length for v in out)


def quaternion_matrix(q):
    """A rotation (w x y z) as a row-vector 3x3 matrix"""
    w, x, y, z = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)],
            [2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)],
            [2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)]]


def joint_local(joint, sample):
    """A joint's matrix in its parent's space: the clip's rotation and translation, or the rest pose's where it has none"""
    rest = joint["rest"]
    rotation, translation = sample if sample else (None, None)
    block = quaternion_matrix(rotation) if rotation else [row[:3] for row in rest[:3]]
    offset = list(translation) if translation else rest[3][:3]
    return [block[0] + [0.0], block[1] + [0.0], block[2] + [0.0], offset + [1.0]]


def skinning_matrices(model, animation, time):
    """Every joint's skinning matrix (its inverse bind matrix, then its place in the model) at a time (None: the bind pose)"""
    world = []
    for joint in model.joints:
        local = joint_local(joint, animation.joint(joint["name"], time) if animation else None)
        world.append(matrix_product(local, world[joint["parent"]]) if joint["parent"] >= 0 else local)
    return [matrix_product(joint["inverse"], world[i]) for i, joint in enumerate(model.joints)]


# ----------------------------------------------------------------------------------------------------------------------------
# Into the pack's space: Pure3D's axes are the pack's already (Y up, facing -Z, his left at +X); only the size changes

def to_pack_point(p):
    return (p[0] * SCALE, p[1] * SCALE, p[2] * SCALE)


def to_pack_normal(n):
    length = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) or 1.0
    return (n[0] / length, n[1] / length, n[2] / length)


def to_pack_matrix(m):
    """A skinning or inverse bind matrix (4x3, row vectors) for scaled points: the translation scaled"""
    return [m[i][:3] for i in range(3)] + [[m[3][j] * SCALE for j in range(3)]]


# ----------------------------------------------------------------------------------------------------------------------------
# The pack (native/CHARACTERS.md, "The pack"; the same as woc_character.py writes)

def oriented(vertices, triangle):
    """The triangle wound so that, seen with its vertexes' normals facing the eye, it goes counterclockwise in a right-handed view
    (its cross product along the normals)"""
    a, b, c = (vertices[i] for i in triangle)
    u = [b[0][k] - a[0][k] for k in range(3)]
    w = [c[0][k] - a[0][k] for k in range(3)]
    cross = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
    normal = [a[1][k] + b[1][k] + c[1][k] for k in range(3)]
    return triangle if sum(cross[k] * normal[k] for k in range(3)) >= 0 else (triangle[0], triangle[2], triangle[1])


def fit_texture(texture, largest=None):
    """A texture's size and RGBA rows, at full size, or halved (each texel the mean of four) until neither side is over largest
    (--max-texture)"""
    width, height, rgba = texture["width"], texture["height"], texture["rgba"]
    while largest and max(width, height) > largest and min(width, height) > 1:
        half = bytearray((width // 2) * (height // 2) * 4)
        for y in range(height // 2):
            for x in range(width // 2):
                for c in range(4):
                    total = sum(rgba[((y * 2 + dy) * width + x * 2 + dx) * 4 + c] for dy in (0, 1) for dx in (0, 1))
                    half[(y * (width // 2) + x) * 4 + c] = (total + 2) // 4
        width, height, rgba = width // 2, height // 2, bytes(half)
    return width, height, rgba


def write_pack(path, model, clips, largest_texture=None):
    used = []
    for mesh in model.meshes:
        texture = model.shaders.get(mesh["shader"], {}).get("texture")
        if texture in model.textures and texture not in used:
            used.append(texture)
    texture_index = {t: i for i, t in enumerate(used)}
    meshes = []
    for mesh in model.meshes:
        vertices = [(to_pack_point(p), to_pack_normal(n), uv, w) for p, n, uv, w in mesh["vertices"]]
        meshes.append((mesh, vertices))
    positions = [v[0] for _, vertices in meshes for v in vertices]
    low = [min(p[k] for p in positions) for k in range(3)]
    high = [max(p[k] for p in positions) for k in range(3)]
    out = bytearray()
    out += struct.pack("<4sIIIII3f3f16x", b"TWCP", PACK_VERSION, len(model.joints), len(used), len(meshes), len(clips),
                       *low, *high)
    for joint in model.joints:
        inverse = to_pack_matrix(joint["inverse"])
        out += struct.pack("<32si12f", joint["name"].encode("ascii")[:31], joint["parent"],
                           *[inverse[r][c] for r in range(4) for c in range(3)])
    for name in used:
        width, height, rgba = fit_texture(model.textures[name], largest_texture)
        out += struct.pack("<HH", width, height) + rgba
    for mesh, vertices in meshes:
        shader = model.shaders.get(mesh["shader"], {})
        texture = texture_index.get(shader.get("texture"), -1)
        blend = shader.get("blend", False) or (texture >= 0 and model.textures[used[texture]]["alpha"])
        colour = shader.get("colour", (1.0, 1.0, 1.0))
        out += struct.pack("<iI4fII", texture, 1 if blend else 0, *colour, 1.0, len(vertices), len(mesh["triangles"]))
        for position, normal, uv, weights in vertices:
            joints = [j for j, _ in weights[:4]] + [0] * (4 - len(weights[:4]))
            amounts = [w for _, w in weights[:4]]
            total = sum(amounts) or 1.0
            amounts = [w / total for w in amounts] + [0.0] * (4 - len(amounts))
            out += struct.pack("<3f3f2f4B4f", *position, *normal, *uv, *joints, *amounts)
        for triangle in mesh["triangles"]:
            out += struct.pack("<3H", *oriented(vertices, triangle))
    for name, animation in clips:
        frames = [[to_pack_matrix(m) for m in skinning_matrices(model, animation, float(frame))]
                  for frame in range(animation.frames())]
        # A loop's last key repeats its first (the idle's frame 41 is frame 0): kept, the loop would stop on it a frame
        if len(frames) > 2 and max(abs(a[r][c] - b[r][c]) for a, b in zip(frames[0], frames[-1])
                                   for r in range(4) for c in range(3)) < 2e-3:
            frames.pop()
        out += struct.pack("<32sIf", name.encode("ascii")[:31], len(frames), animation.rate)
        for matrices in frames:
            for matrix in matrices:
                out += struct.pack("<12f", *[matrix[r][c] for r in range(4) for c in range(3)])
    path.write_bytes(out)
    return len(out)


def settings_folder():
    home = os.environ.get("HOME", ".")
    if sys.platform == "darwin":
        return Path(home) / "Library" / "Application Support" / "Crash Twinsanity"
    config = os.environ.get("XDG_CONFIG_HOME")
    return Path(config) / "crash-twinsanity" if config else Path(home) / ".config" / "crash-twinsanity"


# ----------------------------------------------------------------------------------------------------------------------------
# The preview: the pack's own data skinned and drawn (a small z-buffered rasteriser)

def render_frame(pack, matrices, eye, target, width=240, height=300):
    import numpy as np
    eye, target = np.asarray(eye, float), np.asarray(target, float)
    forward = (target - eye) / np.linalg.norm(target - eye)
    right = np.cross(forward, [0.0, 1.0, 0.0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    light = np.array([0.4, 0.8, -0.5]) / np.linalg.norm([0.4, 0.8, -0.5])
    scale = height / 2 / math.tan(0.3)
    colour = np.zeros((height, width, 3)) + (0.25, 0.3, 0.35)
    depth = np.full((height, width), np.inf)
    for material in sorted(pack["materials"], key=lambda m: m["blend"]):
        v = material["vertices"]
        homogeneous = np.hstack([v["position"], np.ones((len(v["position"]), 1))])
        skinned = np.einsum("vi,vjik,vj->vk", homogeneous, matrices[v["joints"]], v["weights"])[:, :3]
        normals = np.einsum("vi,vjik,vj->vk", v["normal"], matrices[v["joints"]][:, :, :3, :3], v["weights"])
        normals /= np.linalg.norm(normals, axis=1, keepdims=True) + 1e-9
        relative = skinned - eye
        cz = relative @ forward
        sx = width / 2 + (relative @ right) / cz * scale
        sy = height / 2 - (relative @ up) / cz * scale
        shade = 0.45 + 0.55 * np.clip(normals @ light, 0, 1)
        texture = pack["textures"][material["texture"]] if material["texture"] >= 0 else None
        for a, b, c in material["triangles"]:
            x0, y0, x1, y1, x2, y2 = sx[a], sy[a], sx[b], sy[b], sx[c], sy[c]
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-9 or min(cz[a], cz[b], cz[c]) < 0.05:
                continue
            left, top = max(int(min(x0, x1, x2)), 0), max(int(min(y0, y1, y2)), 0)
            right_edge, bottom = min(int(max(x0, x1, x2)) + 1, width - 1), min(int(max(y0, y1, y2)) + 1, height - 1)
            if left > right_edge or top > bottom:
                continue
            ys, xs = np.mgrid[top:bottom + 1, left:right_edge + 1] + 0.5
            w0 = ((x1 - xs) * (y2 - ys) - (x2 - xs) * (y1 - ys)) / area
            w1 = ((x2 - xs) * (y0 - ys) - (x0 - xs) * (y2 - ys)) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            z = 1 / (w0 / cz[a] + w1 / cz[b] + w2 / cz[c])
            region = depth[top:bottom + 1, left:right_edge + 1]
            inside &= z < region
            if not inside.any():
                continue
            p0, p1, p2 = w0 / cz[a] * z, w1 / cz[b] * z, w2 / cz[c] * z
            light_level = p0 * shade[a] + p1 * shade[b] + p2 * shade[c]
            rgb = np.ones(inside.shape + (3,)) * material["colour"][:3]
            alpha = np.ones(inside.shape)
            if texture is not None:
                uv = v["uv"]
                tu = p0 * uv[a, 0] + p1 * uv[b, 0] + p2 * uv[c, 0]
                tv = p0 * uv[a, 1] + p1 * uv[b, 1] + p2 * uv[c, 1]
                th, tw = texture.shape[:2]
                texel = texture[np.floor(tv * th).astype(int) % th, np.floor(tu * tw).astype(int) % tw] / 255.0
                rgb = rgb * texel[..., :3]
                alpha = texel[..., 3]
            inside &= alpha > (0.02 if material["blend"] else 0.25)
            target_pixels = colour[top:bottom + 1, left:right_edge + 1]
            source = rgb * light_level[..., None]
            if material["blend"]:
                source = source * alpha[..., None] + target_pixels * (1 - alpha[..., None])
            else:
                region[inside] = z[inside]
            target_pixels[inside] = source[inside]
    return colour


def preview(pack_path, folder, clips_wanted):
    import numpy as np
    from PIL import Image

    pack = read_pack(pack_path)
    folder.mkdir(parents=True, exist_ok=True)
    width, height = 240, 300
    written = []
    for name, picks in clips_wanted:
        if name not in pack["clips"]:
            continue
        frames = pack["clips"][name]
        every = []
        for matrices in frames:
            colour = render_frame(pack, matrices, (1.5, 0.8, -1.8), (0.0, 0.45, 0.0), width, height)
            every.append(Image.fromarray((np.clip(colour, 0, 1) * 255).astype(np.uint8)))
        row = [every[min(frame, len(every) - 1)] for frame in picks]
        sheet = Image.new("RGB", (width * len(row), height))
        for i, image in enumerate(row):
            sheet.paste(image, (i * width, 0))
        out = folder / ("%s.png" % name.replace("/", "_"))
        sheet.save(out)
        written.append(out)
        # Every frame as an animated GIF at the clip's 30 frames a second
        animated = folder / ("%s.gif" % name.replace("/", "_"))
        every[0].save(animated, save_all=True, append_images=every[1:], duration=33, loop=0)
        written.append(animated)
    return written


def read_pack(path):
    """The pack read back (numpy arrays): what the port reads, for the preview and checks"""
    import numpy as np
    data = path.read_bytes()
    magic, version, joint_count, texture_count, material_count, clip_count = struct.unpack_from("<4sIIIII", data, 0)
    if magic != b"TWCP" or version != PACK_VERSION:
        raise ValueError("not a version %d pack" % PACK_VERSION)
    at = 64 + joint_count * 84
    textures = []
    for _ in range(texture_count):
        width, height = struct.unpack_from("<HH", data, at)
        at += 4
        textures.append(np.frombuffer(data, np.uint8, width * height * 4, at).reshape(height, width, 4))
        at += width * height * 4
    vertex = np.dtype([("position", "<f4", 3), ("normal", "<f4", 3), ("uv", "<f4", 2), ("joints", "u1", 4),
                       ("weights", "<f4", 4)])
    materials = []
    for _ in range(material_count):
        texture, flags, r, g, b, a, vertex_count, triangle_count = struct.unpack_from("<iI4fII", data, at)
        at += 32
        vertices = np.frombuffer(data, vertex, vertex_count, at)
        at += vertex_count * vertex.itemsize
        triangles = np.frombuffer(data, "<u2", triangle_count * 3, at).reshape(-1, 3)
        at += triangle_count * 6
        materials.append({"texture": texture, "blend": bool(flags & 1), "colour": (r, g, b, a),
                          "vertices": {k: vertices[k].astype(np.float64) if k != "joints" else vertices[k].astype(int)
                                       for k in vertex.names}, "triangles": triangles})
    clips = {}
    for _ in range(clip_count):
        name, frames, _ = struct.unpack_from("<32sIf", data, at)
        at += 40
        matrices = np.frombuffer(data, "<f4", frames * joint_count * 12, at).reshape(frames, joint_count, 4, 3)
        at += frames * joint_count * 48
        full = np.zeros((frames, joint_count, 4, 4))
        full[..., :3] = matrices
        full[..., 3, 3] = 1.0
        clips[name.rstrip(b"\0").decode("ascii")] = full
    return {"textures": textures, "materials": materials, "clips": clips}


# ----------------------------------------------------------------------------------------------------------------------------

def find_archives(disc):
    """The disc's RCF archives, ADEFAULT/DEFAULT.RCF first"""
    files = disc.files()
    names = sorted((name for name in files if name.endswith(".RCF") and name != "MOVIES.RCF"),
                   key=lambda name: (name != "ADEFAULT/DEFAULT.RCF", name))
    if "ADEFAULT/DEFAULT.RCF" not in names:
        raise SystemExit("no ADEFAULT/DEFAULT.RCF on the disc: is it Crash Tag Team Racing?")
    return [(name, files[name][0]) for name in names]


def costume_file(costume):
    """The model's name in the archives for a costume ('classic', 'caveman', 'japan', 'japan_caveman'...)"""
    costume = costume.lower()
    if costume in ("classic", "crash", ""):
        return COSTUME_PREFIX + "crash_onfoot_model.p3d"
    if costume == "japan":
        return COSTUME_PREFIX + "crash_japan_onfoot_model.p3d"
    if costume.startswith("japan_"):
        return COSTUME_PREFIX + "japan_crash_%s_onfoot_model.p3d" % costume[6:]
    return COSTUME_PREFIX + "crash_%s_onfoot_model.p3d" % costume


def costumes(names):
    found = []
    for name in sorted(names):
        if not name.startswith(COSTUME_PREFIX) or not name.endswith("_onfoot_model.p3d"):
            continue
        stem = name[len(COSTUME_PREFIX):-len("_onfoot_model.p3d")]
        if stem == "crash":
            found.append("classic")
        elif stem == "crash_japan":
            found.append("japan")
        elif stem.startswith("crash_"):
            found.append(stem[6:])
        elif stem.startswith("japan_crash_"):
            found.append("japan_" + stem[12:])
    return sorted(found, key=lambda c: (c != "classic", c.startswith("japan"), c))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("disc", help="the Crash Tag Team Racing disc image (.iso or .bin)")
    parser.add_argument("--costume", default="classic", help="the outfit (classic, caveman, japan, ...; --list prints them)")
    parser.add_argument("--out", type=Path, help="the pack's folder (the settings folder's characters/cttr_crash)")
    parser.add_argument("--preview", type=Path, help="write PNGs and GIFs of the idle, run, walk and jump here")
    parser.add_argument("--max-texture", type=int, metavar="N",
                        help="halve textures until no side is over N texels (default: full size)")
    parser.add_argument("--list", action="store_true", help="list the costumes and animations and stop")
    arguments = parser.parse_args()

    disc = Disc(arguments.disc)
    archives = []
    for label, lba in find_archives(disc):
        try:
            archives.append(Rcf(disc, lba, label))
        except ValueError as error:
            print("skipped %s" % error)

    def find(name):
        for archive in archives:
            if name.lower() in archive.files:
                return archive
        return None

    if arguments.list:
        names = set()
        for archive in archives:
            names.update(entry[0] for entry in archive.files.values())
        print("costumes:", " ".join(costumes(names)))
        archive = find(ANIMATIONS)
        if archive:
            data = archive.read(ANIMATIONS)
            print("animations (%s in %s):" % (ANIMATIONS, archive.label))
            for ident, at, children, end in chunks(data, 12, len(data)):
                if ident == 0x121000:
                    clip = Animation(data, at, children, end)
                    print("    %-36s %4d frames" % (clip.name, clip.frames()))
        return

    model_name = costume_file(arguments.costume)
    archive = find(model_name)
    if archive is None:
        raise SystemExit("no %s on the disc (--list prints the costumes)" % model_name)
    model = Model(archive.read(model_name))
    clips = []
    animations = ANIMATIONS_JAPAN if arguments.costume.lower().startswith("japan") and find(ANIMATIONS_JAPAN) else ANIMATIONS
    data = find(animations).read(animations)
    joints = {joint["name"] for joint in model.joints}
    for ident, at, children, end in chunks(data, 12, len(data)):
        if ident == 0x121000:
            clip = Animation(data, at, children, end)
            if not joints & set(clip.groups):
                print("skipped %s: it animates another skeleton (%s)" % (clip.name, ", ".join(sorted(clip.groups))))
                continue
            clips.append((clip.name, clip))
    # A clip moving under half the idle's joints is a layer the game plays over the idle (the mouth flap): the idle fills it
    by_name = dict(clips)
    idle = by_name.get("onfoot_idle")
    if idle:
        idle_joints = joints & set(idle.groups)
        for _, clip in clips:
            if len(idle_joints & set(clip.groups)) * 2 < len(idle_joints):
                clip.base = idle
    for woc_name, parts in WOC_NAMES:
        parts = [part if isinstance(part, tuple) else (part, 0, None) for part in parts]
        if all(name in by_name for name, _, _ in parts):
            pieces = [by_name[name] if count is None else Slice(by_name[name], start, count) for name, start, count in parts]
            clips.append((woc_name, pieces[0] if len(pieces) == 1 else Sequence(pieces)))
    costume = arguments.costume.lower()
    folder = "cttr_crash" if costume in ("classic", "crash", "") else "cttr_crash_" + costume
    out = arguments.out or settings_folder() / "characters" / folder
    out.mkdir(parents=True, exist_ok=True)
    size = write_pack(out / "pack.bin", model, clips, arguments.max_texture)
    triangles = sum(len(mesh["triangles"]) for mesh in model.meshes)
    vertices = sum(len(mesh["vertices"]) for mesh in model.meshes)
    copies = sum(1 for name, _ in clips if "/" in name)
    print("%s (%s): %d joints, %d materials, %d vertices, %d triangles, %d animations and %d copies under WoC's names "
          "(%d frames): %s, %.1f MB" % (model_name, archive.label, len(model.joints), len(model.meshes), vertices, triangles,
                                        len(clips) - copies, copies, sum(clip.frames() for _, clip in clips), out / "pack.bin",
                                        size / 1e6))
    if arguments.preview:
        wanted = [("onfoot_idle", [0, 10, 20, 30]), ("onfoot_run", [0, 3, 6, 9, 12, 15]),
                  ("onfoot_jump_rise", [0, 5, 10, 14]), ("onfoot_jump_fall", [0, 6, 12]),
                  ("onfoot_doublejump_start", [0, 8, 16, 24, 32]), ("onfoot_walk", [0, 6, 12, 18])]
        for path in preview(out / "pack.bin", arguments.preview, wanted):
            print("preview:", path)


if __name__ == "__main__":
    main()
