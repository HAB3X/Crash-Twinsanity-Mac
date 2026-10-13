#!/usr/bin/env python3
"""Crash Bandicoot: The Wrath of Cortex's Crash (Traveller's Tales' NU2 engine, the PS2 disc) made into a character pack for the
native port (native/CHARACTERS.md): his model (mesh, materials, textures, bone weights), his skeleton and his animations, read
from your own disc image and written to the port's settings folder. Nothing of the game's goes into the repository.

    python3 native/tools/woc_character.py "/path/to/Crash Bandicoot - The Wrath of Cortex (USA) (v1.01).bin"
        [--out DIR] [--character crash] [--preview DIR] [--list]

The image is the disc's .bin (raw 2352-byte sectors, MODE2 or MODE1) or an .iso. --out defaults to the settings folder's
characters/woc_<character>/ ($HOME/Library/Application Support/Crash Twinsanity on macOS, $XDG_CONFIG_HOME/crash-twinsanity or
~/.config/crash-twinsanity elsewhere). --preview writes PNGs of a few frames of the idle, run and jump (needs numpy and Pillow),
drawn from the pack's own data. --list prints the character's files in CHARS.DAT.

The decoding (worked out on the USA v1.01 disc; the formats' notes are native/CHARACTERS.md's):
- CHARS.DAT: NU2's .dat archive (the data's size, then a table: version, count, offset/size/unpacked size/packed flag a file, then
  a tree of names). Packed files are RNC method 2.
- chars\\a\\crash\\crash.ghg: the model as the PS2's loader takes it, a memory image with offsets for pointers: GS-ready texture
  uploads (a palette and the texels a texture, some 8 bit ones uploaded as 32 bit pixels), materials (GS register settings, a
  texture number counted from 1), the skeleton (NUJOINTDATA records, local bind matrices T, inverse bind matrices INV_WT) and
  layers of skinned meshes: per material, DMA chains of VIF packets for the skin's VU1 program, batches of up to ~30 strip
  vertices (position and normal x; u, v, normal y and z; the weights) with references back to earlier ones.
- chars\\a\\crash\\<group>\\<action>.ani: NU2 animation data (nuanimdata_s), keys on Hermite curves per joint (translation,
  rotation in radians as X then Y then Z, scale), 32-frame chunks, 30 frames a second.
"""
import argparse
import math
import os
import struct
import sys
from pathlib import Path

PACK_VERSION = 1
FLT_MAX = 3.4028234663852886e38


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
# RNC ProPack method 2 (a bit stream of literals and matches; the bits read high first a byte at a time)

def rnc_unpack(data):
    if data[:3] != b"RNC" or data[3] != 2:
        raise ValueError("not RNC method 2")
    size = struct.unpack_from(">I", data, 4)[0]
    out = bytearray()
    at = 18
    bits = 0
    count = 0

    def bit():
        nonlocal bits, count, at
        if count == 0:
            bits = data[at]
            at += 1
            count = 8
        count -= 1
        value = (bits >> 7) & 1
        bits = (bits << 1) & 0xFF
        return value

    def byte():
        nonlocal at
        value = data[at]
        at += 1
        return value

    def offset():
        value = 0
        if bit():
            value = bit()
            if bit():
                value = ((value << 1) | bit()) | 4
                if not bit():
                    value = (value << 1) | bit()
            elif value == 0:
                value = bit() + 2
        return ((value << 8) | byte()) + 1

    def copy(length, distance):
        start = len(out) - distance
        if distance >= length:
            out.extend(out[start:start + length])
        else:
            for i in range(length):
                out.append(out[start + i])

    bit()
    bit()
    while len(out) < size:
        if not bit():
            out.append(byte())
        elif bit():
            if bit():
                if bit():
                    length = byte() + 8
                    if length == 8:
                        bit()
                        continue
                else:
                    length = 3
                copy(length, offset())
            else:
                copy(2, byte() + 1)
        else:
            length = bit() + 4
            if bit():
                length = ((length - 1) << 1) + bit()
            if length != 9:
                copy(length, offset())
            else:
                literal = (bit() << 3 | bit() << 2 | bit() << 1 | bit()) * 4 + 12
                out.extend(data[at:at + literal])
                at += literal
    return bytes(out[:size])


# ----------------------------------------------------------------------------------------------------------------------------
# CHARS.DAT

class DatArchive:
    def __init__(self, data):
        self.data = data
        table_at = struct.unpack_from("<I", data, 0)[0]
        table = data[table_at:]
        _, count = struct.unpack_from("<iI", table, 0)
        self.entries = [struct.unpack_from("<IIII", table, 8 + i * 16) for i in range(count)]
        at = 8 + count * 16
        node_count = struct.unpack_from("<i", table, at)[0]
        nodes = [struct.unpack_from("<hhI", table, at + 4 + i * 8) for i in range(node_count)]
        at += 4 + node_count * 8
        names_size = struct.unpack_from("<i", table, at)[0]
        names = table[at + 4:at + 4 + names_size]
        self.files = {}

        # A folder's node has its last child's index (above it), a file's the negated file number; a node's sibling index goes
        # back to the one before it
        def walk(parent, node, prefix):
            while node > parent:
                child, sibling, name_at = nodes[node]
                name = prefix + names[name_at:names.index(b"\0", name_at)].decode("ascii")
                if child > 0:
                    walk(node, child, name + "\\")
                else:
                    self.files[name.lower()] = -child
                node = sibling

        walk(0, 1, "")

    def read(self, name):
        offset, size, unpacked, packed = self.entries[self.files[name.lower()]]
        data = self.data[offset:offset + size]
        return rnc_unpack(data) if packed else data


# ----------------------------------------------------------------------------------------------------------------------------
# The GS's local memory layouts, for the textures uploaded as 32 bit pixels and read as 8 bit ones (the GS User's Manual's page,
# block and column arrangements; src/platform/native/graphics/gs/gsmemory.cpp has the same)

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


PSMCT32, PSMT8, PSMT4 = 0x00, 0x13, 0x14


# ----------------------------------------------------------------------------------------------------------------------------
# The model (.ghg)

def read_matrix(data, at):
    values = struct.unpack_from("<16f", data, at)
    return [list(values[row * 4:row * 4 + 4]) for row in range(4)]


def multiply(a, b):
    """Row-major 4x4 product (row vectors: a then b)"""
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def float_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def bits_float(value):
    return struct.unpack("<f", struct.pack("<I", value))[0]


class Model:
    """A .ghg: its textures (RGBA rows), materials, joints and meshes (a material's vertices and triangles)"""

    def __init__(self, data):
        self.data = data
        header = struct.unpack_from("<20I", data, 0)
        texture_count, texture_table = header[2], header[3]
        material_count, material_table = header[4], header[5]
        joint_count, joint_table, bind_table, inverse_table = header[6], header[7], header[8], header[9]
        layer_count, layer_table = header[18], header[19]
        texture_offsets = struct.unpack_from("<%dI" % texture_count, data, texture_table)
        self.textures = [self.read_texture(at) for at in texture_offsets]
        self.materials = [self.read_material(at) for at in struct.unpack_from("<%dI" % material_count, data, material_table)]
        self.joints = []
        for index in range(joint_count):
            at = joint_table + index * 0x60
            name_at = struct.unpack_from("<I", data, at + 0x4C)[0]
            self.joints.append({
                "name": data[name_at:data.index(b"\0", name_at)].decode("ascii"),
                "orient": read_matrix(data, at),
                "locator": struct.unpack_from("<3f", data, at + 0x40),
                "parent": struct.unpack_from("<b", data, at + 0x50)[0],
                "flags": data[at + 0x51],
                "bind": read_matrix(data, bind_table + index * 0x40),
                "inverse": read_matrix(data, inverse_table + index * 0x40),
            })
        self.meshes = {}
        for layer in range(layer_count):
            # NULAYERDATA: name, rigid objects, skin, blend shapes' rigid objects, blend shapes' skin
            _, _, skin, _, blend_skin = struct.unpack_from("<5I", data, layer_table + layer * 20)
            for gobj in (skin, blend_skin):
                if gobj:
                    self.read_skin(struct.unpack_from("<I", data, gobj + 12)[0])

    # A texture: a header (width, a word, height, the GS formats, ...), then two uploads, each a DMA tag (RET, VIF DIRECT) of
    # A+D writes (TRXPOS, TRXREG, TRXDIR: the address set when loading) and an IMAGE: the palette (16x16 or 8x2, 32 bit), then
    # the texels
    def read_texture(self, at):
        data = self.data
        width, _, height, _, formats = struct.unpack_from("<HHIII", data, at)
        pixel_format = (formats >> 8) & 0xFF
        upload_format = (formats >> 16) & 0xFF
        uploads = []
        scan = at
        while len(uploads) < 2:
            tag, = struct.unpack_from("<I", data, scan)
            vif, = struct.unpack_from("<I", data, scan + 12)
            if (tag >> 28) & 7 == 6 and vif >> 24 == 0x50 and (vif & 0xFFFF) == (tag & 0xFFFF):
                trxreg, = struct.unpack_from("<Q", data, scan + 0x30)
                uploads.append((trxreg & 0xFFF, (trxreg >> 32) & 0xFFF, scan + 0x60))
            scan += 16
        (palette_width, palette_height, palette_at), (upload_width, upload_height, texels_at) = uploads
        colours = palette_width * palette_height
        palette = [data[palette_at + i * 4:palette_at + i * 4 + 4] for i in range(colours)]
        if colours == 256:
            # The CLUT's 32 bit 16x16 layout swaps index bits 3 and 4
            palette = [palette[(i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1] for i in range(256)]
        palette = [bytes((c[0], c[1], c[2], min(255, c[3] * 2))) for c in palette]
        if pixel_format == PSMT8 and upload_format == PSMT8:
            indexes = data[texels_at:texels_at + width * height]
        elif pixel_format == PSMT4 and upload_format == PSMT4:
            packed = data[texels_at:texels_at + width * height // 2]
            indexes = bytearray(width * height)
            indexes[0::2] = bytes(b & 15 for b in packed)
            indexes[1::2] = bytes(b >> 4 for b in packed)
        elif pixel_format == PSMT8 and upload_format == PSMCT32:
            buffer_width = max(1, upload_width // 64)
            memory = bytearray(max(address32(upload_width - 1, upload_height - 1, buffer_width) + 1, 1) * 4 + 0x2000)
            for y in range(upload_height):
                for x in range(upload_width):
                    source = texels_at + (y * upload_width + x) * 4
                    target = address32(x, y, buffer_width) * 4
                    memory[target:target + 4] = data[source:source + 4]
            indexes = bytes(memory[address8(x, y, buffer_width * 2)] for y in range(height) for x in range(width))
        else:
            raise ValueError("texture format %#x uploaded as %#x" % (pixel_format, upload_format))
        return {"width": width, "height": height, "rgba": b"".join(palette[i] for i in indexes)}

    # A material (numtl_s: the GS settings, then its attributes, colours and texture): the alpha mode (attributes bits 0-3), the
    # diffuse colour (0.5 the texture as it is) and the texture's number (from 1; 0 none)
    def read_material(self, at):
        attributes, = struct.unpack_from("<I", self.data, at + 0x16C)
        diffuse = struct.unpack_from("<3f", self.data, at + 0x180)
        texture, = struct.unpack_from("<i", self.data, at + 0x1A4)
        return {"blend": (attributes & 0xF) != 0, "diffuse": diffuse, "texture": texture - 1}

    # A skin: its geoms (next, material, a word, the DMA chain), each chain a CNT (the cycle), the REFs of the joints' matrices for
    # VU1 (the address over 0x40 is the joint) and a CALL of a packet (RET-tagged, relative to the chain); a RET ends it
    def read_skin(self, geom):
        data = self.data
        while geom:
            following, material, _, chain = struct.unpack_from("<IiiI", data, geom)
            mesh = self.meshes.setdefault(material, {"vertices": [], "triangles": []})
            palette = []
            at = chain
            while True:
                tag, address = struct.unpack_from("<II", data, at)
                kind = (tag >> 28) & 7
                if kind == 1:
                    palette = []
                    at += 16 + (tag & 0xFFFF) * 16
                elif kind == 3:
                    palette.append(address // 0x40)
                    at += 16
                elif kind == 5:
                    packet = chain + address
                    length, = struct.unpack_from("<I", data, packet)
                    self.read_packet(mesh, list(palette), packet + 8, packet + 16 + (length & 0xFFFF) * 16)
                    at += 16
                elif kind == 6:
                    break
                else:
                    raise ValueError("DMA tag %d at %#x" % (kind, at))
            geom = following

    # A skin packet's VIF codes, batch by batch (a batch starts with its count, a V4-16 to address 2: its vertex slots times four).
    # With STCYCL 4/2 a vertex's two quadwords go to slot 3 + 4n: (x, y, z, normal x), (u, v, normal y, normal z) with flags in
    # the low byte of the last five (bit 0 of the last: the strip's ADC, no triangle ends here); an S-16 pair there instead takes
    # the batch's vertex k (from 1) again, with its own ADC. With STCYCL 1/1 the weights go to slot + 2 (and + 3 past four): each a
    # float whose low 3 bits are its matrix's slot in the batch's palette. Blend shapes' offsets (to 0x38 and 0x39) are left out
    def read_packet(self, mesh, palette, start, end):
        data = self.data
        batches = []
        batch = None
        cycle = (1, 1)
        at = start
        while at < end:
            code, = struct.unpack_from("<I", data, at)
            command = (code >> 24) & 0x7F
            immediate = code & 0xFFFF
            number = (code >> 16) & 0xFF
            at += 4
            if command & 0x60 == 0x60:
                components = ((command >> 2) & 3) + 1
                size = {0: 4, 1: 2, 2: 1, 3: 2}[command & 3]
                count = number or 256
                length = size * components * count if command & 3 != 3 else 2 * count
                payload = data[at:at + length]
                at += (length + 3) // 4 * 4
                address = immediate & 0x3FF
                relative = immediate & 0x8000
                if address == 2 and command == 0x6D:
                    batch = {"slots": struct.unpack_from("<h", payload)[0] // 4, "vertices": {}, "copies": {}, "weights": {}}
                    batches.append(batch)
                elif batch is None or not relative or address == 0xD2:
                    continue
                elif cycle == (4, 2) and command == 0x6C:
                    for k in range(count // 2):
                        batch["vertices"][(address - 3) // 4 + k] = struct.unpack_from("<8I", payload, k * 32)
                elif cycle == (4, 2) and command == 0x61:
                    batch["copies"][(address - 3) // 4] = struct.unpack_from("<2h", payload)
                elif cycle == (1, 1) and command in (0x60, 0x64, 0x68, 0x6C) and (address - 5) % 4 in (0, 1):
                    slot = (address - 5) // 4
                    batch["weights"][slot] = batch["weights"].get(slot, ()) + struct.unpack("<%dI" % components, payload)
            elif command == 0x01:
                cycle = (immediate & 0xFF, immediate >> 8)
            elif command in (0x30, 0x31):
                at += 16
            elif command == 0x20:
                at += 4
            elif command in (0x50, 0x51):
                at += immediate * 16
        for batch in batches:
            strip = []
            for slot in range(batch["slots"]):
                if slot in batch["vertices"]:
                    words = batch["vertices"][slot]
                    values = [bits_float(w) for w in words]
                    weights = {}
                    for word in batch["weights"].get(slot, (float_bits(1.0),)):
                        weight = bits_float(word)
                        if abs(weight) > 1e-30:
                            joint = palette[word & 7]
                            weights[joint] = weights.get(joint, 0.0) + weight
                    vertex = (tuple(values[0:3]), (values[3], values[6], values[7]), (values[4], values[5]),
                              tuple(sorted(weights.items(), key=lambda item: -item[1])))
                    strip.append((vertex, words[7] & 1))
                else:
                    reuse, adc = batch["copies"][slot]
                    strip.append((strip[reuse - 1][0], adc & 1))
            for i in range(2, len(strip)):
                if strip[i][1]:
                    continue
                corners = [strip[i - 2][0], strip[i - 1][0], strip[i][0]]
                if corners[0][0] == corners[1][0] or corners[1][0] == corners[2][0] or corners[0][0] == corners[2][0]:
                    continue
                mesh["triangles"].append(tuple(self.add_vertex(mesh, corner) for corner in corners))

    @staticmethod
    def add_vertex(mesh, vertex):
        index = mesh.setdefault("index", {})
        if vertex not in index:
            index[vertex] = len(mesh["vertices"])
            mesh["vertices"].append(vertex)
        return index[vertex]


# ----------------------------------------------------------------------------------------------------------------------------
# Animations (.ani)

class Animation:
    def __init__(self, data):
        _, base, root = struct.unpack_from("<III", data, 0)
        self.end, _, chunk_count, chunks = struct.unpack_from("<fIiI", data, root - base)
        self.chunks = []
        for c in range(chunk_count):
            chunk, = struct.unpack_from("<I", data, chunks - base + c * 4)
            nodes = []
            if chunk:
                node_count, _, sets, _, _ = struct.unpack_from("<iiIII", data, chunk - base)
                for n in range(node_count):
                    curve_set, = struct.unpack_from("<I", data, sets - base + n * 4)
                    if curve_set == 0:
                        nodes.append(None)
                        continue
                    flags, constants, curves, curve_count = struct.unpack_from("<iIIb", data, curve_set - base)
                    values = list(struct.unpack_from("<%df" % curve_count, data, constants - base))
                    keyed = []
                    for k in range(curve_count):
                        curve = struct.unpack_from("<I", data, curves - base + k * 4)[0] if curves else 0
                        if curve == 0:
                            keyed.append(None)
                            continue
                        _, keys, key_count, _ = struct.unpack_from("<IIII", data, curve - base)
                        keyed.append([struct.unpack_from("<4f", data, keys - base + i * 16) for i in range(key_count)])
                    nodes.append((flags, values, keyed))
            self.chunks.append(nodes)

    @staticmethod
    def curve(keys, time):
        """A curve's value: keys (time, 1 / the time to the next, slope, value) on cubic Hermite segments"""
        if time <= keys[0][0]:
            return keys[0][3]
        for i in range(1, len(keys)):
            if keys[i][0] >= time:
                before, after = keys[i - 1], keys[i]
                t = (time - before[0]) * before[1]
                span = after[0] - before[0]
                return ((2 * t ** 3 - 3 * t ** 2 + 1) * before[3] + (t ** 3 - 2 * t ** 2 + t) * before[2] * span +
                        (-2 * t ** 3 + 3 * t ** 2) * after[3] + (t ** 3 - t ** 2) * after[2] * span)
        return keys[-1][3]

    def frames(self):
        """Its frames from 1 to the end (the end itself is its last frame plus one)"""
        return max(1, int(round(self.end - 1)))

    def joint(self, index, time):
        """A joint's flags and nine values (translation, rotation, scale) at a time, or None when the clip leaves it alone"""
        time = min(max(time, 1.0), self.end - 0.01)
        chunk = min(int(math.floor((time - 1.0) / 32.0)), len(self.chunks) - 1)
        nodes = self.chunks[chunk]
        if index >= len(nodes) or nodes[index] is None:
            return None
        flags, values, keyed = nodes[index]
        return flags, [self.curve(keyed[k], time) if keyed[k] else values[k] for k in range(len(values))]


ANGLE_UNITS = 65536.0 / (2.0 * math.pi)


def rotation_xyz(x, y, z):
    """NuMtxSetRotateXYZ on the angles as the game rounds them (65536 a turn): X, then Y, then Z (row vectors)"""
    x, y, z = (int(a * ANGLE_UNITS) / ANGLE_UNITS for a in (x, y, z))
    sx, cx, sy, cy, sz, cz = math.sin(x), math.cos(x), math.sin(y), math.cos(y), math.sin(z), math.cos(z)
    return [[cy * cz, cy * sz, -sy, 0.0],
            [sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy, 0.0],
            [cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy, 0.0],
            [0.0, 0.0, 0.0, 1.0]]


def joint_local(joint, sample):
    """A joint's matrix in its parent's space from a clip's values (NuAnimCurveSetApplyToJoint2): rotation (flag 1), the rest
    orientation after it (flag 0x20), scale before it (flag 8), the translation, the locator offset's pivot (the joint's flag 8),
    then Z flipped (the animation's right-handed space to the game's)"""
    if sample is None:
        return joint["bind"]
    flags, v = sample
    matrix = rotation_xyz(v[3], v[4], v[5]) if flags & 1 else [[float(i == j) for j in range(4)] for i in range(4)]
    if flags & 0x20:
        matrix = multiply(matrix, joint["orient"])
    if flags & 8:
        matrix = [[matrix[i][j] * (v[6 + i] if i < 3 else 1.0) for j in range(4)] for i in range(4)]
    matrix = [row[:] for row in matrix]
    for k in range(3):
        matrix[3][k] += v[k]
    if joint["flags"] & 8:
        locator = joint["locator"]
        pivot = [row[:] for row in matrix]
        for k in range(3):
            pivot[3][k] = sum(locator[i] * matrix[i][k] for i in range(3)) + matrix[3][k] - locator[k]
        matrix = pivot
    for i, j in ((0, 2), (1, 2), (2, 0), (2, 1), (2, 3), (3, 2)):
        matrix[i][j] = -matrix[i][j]
    return matrix


def skinning_matrices(model, animation, time):
    """Every joint's skinning matrix (its inverse bind matrix, then its place in the model) at a time (None: the bind pose)"""
    world = []
    for index, joint in enumerate(model.joints):
        local = joint_local(joint, animation.joint(index, time) if animation else None)
        world.append(multiply(local, world[joint["parent"]]) if joint["parent"] >= 0 else local)
    return [multiply(joint["inverse"], world[i]) for i, joint in enumerate(model.joints)]


# ----------------------------------------------------------------------------------------------------------------------------
# The pack (native/CHARACTERS.md, "The pack")

def oriented(vertices, triangle):
    """The triangle wound so that, seen with its vertexes' normals facing the eye, it goes counterclockwise in a right-handed view
    (its cross product along the normals)"""
    a, b, c = (vertices[i] for i in triangle)
    u = [b[0][k] - a[0][k] for k in range(3)]
    w = [c[0][k] - a[0][k] for k in range(3)]
    cross = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
    normal = [a[1][k] + b[1][k] + c[1][k] for k in range(3)]
    return triangle if sum(cross[k] * normal[k] for k in range(3)) >= 0 else (triangle[0], triangle[2], triangle[1])


def write_pack(path, model, clips):
    used = sorted({model.materials[m]["texture"] for m in model.meshes if model.materials[m]["texture"] >= 0})
    texture_index = {t: i for i, t in enumerate(used)}
    positions = [v[0] for mesh in model.meshes.values() for v in mesh["vertices"]]
    low = [min(p[k] for p in positions) for k in range(3)]
    high = [max(p[k] for p in positions) for k in range(3)]
    out = bytearray()
    out += struct.pack("<4sIIIII3f3f16x", b"TWCP", PACK_VERSION, len(model.joints), len(used), len(model.meshes), len(clips),
                       *low, *high)
    for joint in model.joints:
        out += struct.pack("<32si12f", joint["name"].encode("ascii")[:31], joint["parent"],
                           *[joint["inverse"][r][c] for r in range(4) for c in range(3)])
    for t in used:
        texture = model.textures[t]
        out += struct.pack("<HH", texture["width"], texture["height"]) + texture["rgba"]
    for material in sorted(model.meshes):
        mesh = model.meshes[material]
        settings = model.materials[material]
        colour = [min(2.0, 2.0 * c) for c in settings["diffuse"]] + [1.0]
        texture = texture_index.get(settings["texture"], -1)
        out += struct.pack("<iI4fII", texture, 1 if settings["blend"] else 0, *colour, len(mesh["vertices"]),
                           len(mesh["triangles"]))
        for position, normal, uv, weights in mesh["vertices"]:
            joints = [j for j, _ in weights[:4]] + [0] * (4 - len(weights[:4]))
            amounts = [w for _, w in weights[:4]]
            total = sum(amounts) or 1.0
            amounts = [w / total for w in amounts] + [0.0] * (4 - len(amounts))
            out += struct.pack("<3f3f2f4B4f", *position, *normal, *uv, *joints, *amounts)
        for triangle in mesh["triangles"]:
            out += struct.pack("<3H", *oriented(mesh["vertices"], triangle))
    for name, animation in clips:
        frames = animation.frames()
        out += struct.pack("<32sIf", name.encode("ascii")[:31], frames, 30.0)
        for frame in range(frames):
            for matrix in skinning_matrices(model, animation, 1.0 + frame):
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

def preview(pack_path, folder, clips_wanted):
    import numpy as np
    from PIL import Image

    pack = read_pack(pack_path)
    folder.mkdir(parents=True, exist_ok=True)
    eye = np.array([1.5, 0.8, -1.8])
    target = np.array([0.0, 0.45, 0.0])
    forward = (target - eye) / np.linalg.norm(target - eye)
    right = np.cross(forward, [0.0, 1.0, 0.0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    light = np.array([0.4, 0.8, -0.5]) / np.linalg.norm([0.4, 0.8, -0.5])
    width, height, scale = 240, 300, 300 / 2 / math.tan(0.3)
    written = []
    for name, picks in clips_wanted:
        if name not in pack["clips"]:
            continue
        frames = pack["clips"][name]
        row = []
        for frame in picks:
            matrices = frames[min(frame, len(frames) - 1)]
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
            row.append(Image.fromarray((np.clip(colour, 0, 1) * 255).astype(np.uint8)))
        sheet = Image.new("RGB", (width * len(row), height))
        for i, image in enumerate(row):
            sheet.paste(image, (i * width, 0))
        out = folder / ("%s.png" % name.replace("/", "_"))
        sheet.save(out)
        written.append(out)
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

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("disc", help="the Wrath of Cortex disc image (.bin or .iso)")
    parser.add_argument("--character", default="crash", help="the character's folder in CHARS.DAT (chars\\a\\<name>)")
    parser.add_argument("--out", type=Path, help="the pack's folder (the settings folder's characters/woc_<character>)")
    parser.add_argument("--preview", type=Path, help="write PNGs of a few frames of the idle, run and jump here")
    parser.add_argument("--list", action="store_true", help="list the character's files and stop")
    arguments = parser.parse_args()

    disc = Disc(arguments.disc)
    files = disc.files()
    if "CHARS.DAT" not in files:
        raise SystemExit("no CHARS.DAT on the disc: is it The Wrath of Cortex?")
    archive = DatArchive(disc.read(*files["CHARS.DAT"]))
    prefix = "chars\\a\\%s\\" % arguments.character.lower()
    names = sorted(name for name in archive.files if name.startswith(prefix))
    if arguments.list:
        print("\n".join(names))
        return
    model_name = prefix + arguments.character.lower() + ".ghg"
    if model_name not in archive.files:
        raise SystemExit("no %s in CHARS.DAT" % model_name)
    model = Model(archive.read(model_name))
    clips = []
    for name in names:
        if name.endswith(".ani") and name.count("\\") == 4:
            try:
                clips.append((name[len(prefix):-4].replace("\\", "/"), Animation(archive.read(name))))
            except (struct.error, ValueError, IndexError) as error:
                print("skipped %s: %s" % (name, error))
    out = arguments.out or settings_folder() / "characters" / ("woc_" + arguments.character.lower())
    out.mkdir(parents=True, exist_ok=True)
    size = write_pack(out / "pack.bin", model, clips)
    triangles = sum(len(mesh["triangles"]) for mesh in model.meshes.values())
    vertices = sum(len(mesh["vertices"]) for mesh in model.meshes.values())
    print("%s: %d joints, %d materials, %d vertices, %d triangles, %d animations (%d frames): %s, %.1f MB" % (
        model_name, len(model.joints), len(model.meshes), vertices, triangles, len(clips),
        sum(clip.frames() for _, clip in clips), out / "pack.bin", size / 1e6))
    if arguments.preview:
        wanted = [("b/idle", [0, 6, 12, 18]), ("a/run", [0, 4, 8, 12, 16, 20]), ("b/jump", [0, 5, 10, 15, 20]),
                  ("a/spin", [0, 2, 4, 6]), ("a/slidattk", [0, 4, 8, 12])]
        for path in preview(out / "pack.bin", arguments.preview, wanted):
            print("preview:", path)


if __name__ == "__main__":
    main()
