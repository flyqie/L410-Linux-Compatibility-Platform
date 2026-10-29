"""Backport the stock SP68 AnCo maximize/recover route into HarmonyOS 6.0.

The active SceneBoard already contains the original compatible-window event,
title-action and zoom pipeline, but EVENT_MAXIMIZE/EVENT_RECOVER bypass it.
SP68 routes AnCo applications through that pipeline and preserves the original
window type, position and size.  This builder ports only those stock semantics
using the existing HarmonyOS 6.0 objects and methods.

No Linux-only host interface, synthetic input, window action, or lifecycle
policy is introduced.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import argparse
import importlib.util
import json
import struct
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from abc_record_index import (  # noqa: E402
    HEADER,
    parse_record_members,
    parse_record_prefix,
    read_string_item,
    read_uleb128,
)


BASE_BUILDER = ROOT / "linux-compat/scripts/build-anco-sp68-title-fullscreen.py"
_spec = importlib.util.spec_from_file_location("title_builder", BASE_BUILDER)
if _spec is None or _spec.loader is None:
    raise RuntimeError(BASE_BUILDER)
_base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_base)

uleb = _base.uleb
instruction_table = _base.instruction_table
decode = _base.decode
method_code_pointer_offset = _base.method_code_pointer_offset
compile_abc = _base.compile_abc
disassemble = _base.disassemble


SESSION_RECORD = "L&@ohos/windowscene/src/main/ets/scene/session/SCBSceneSession&1.0.0;"
SCENE_RECORD = "L&@ohos/pcbase/src/main/ets/SceneBoard/scenemanager/SCBScene&1.0.0;"
SESSION_METHOD = "#~@c>#onSessionEvent"
SCENE_METHOD = "#~@1>#onCompatibleModeMaximizeOrRecover"
MAX_CLOSURE = "#~@1>@2*#"
RECOVER_CLOSURE = "#~@1>@2*#^1"

NEW_STRINGS = (
    "hmos_fusion.container.pc.freemode.captionbar",
    "lastWindowShowType",
    "lastPosForCompatible",
    "lastSizeForCompatible",
)


def find_methods(data: bytes) -> dict[str, dict]:
    header = HEADER.unpack_from(data)
    wanted = {
        SESSION_RECORD: {SESSION_METHOD},
        SCENE_RECORD: {SCENE_METHOD, MAX_CLOSURE, RECOVER_CLOSURE},
    }
    result: dict[str, dict] = {}
    for index in range(header[6]):
        record_offset = struct.unpack_from("<I", data, header[7] + 4 * index)[0]
        record = parse_record_prefix(data, record_offset, len(data))
        if record["name"] not in wanted:
            continue
        _, methods = parse_record_members(data, record, len(data))
        for method in methods:
            if method["name"] in wanted[record["name"]]:
                result[method["name"]] = method
    expected = {SESSION_METHOD, SCENE_METHOD, MAX_CLOSURE, RECOVER_CLOSURE}
    if set(result) != expected:
        raise ValueError(("target methods", set(result), expected))
    return result


def method_region(data: bytes, method: dict) -> tuple[int, tuple[int, ...], int]:
    header = HEADER.unpack_from(data)
    for index in range(header[12]):
        header_offset = header[13] + index * 40
        region = struct.unpack_from("<10I", data, header_offset)
        if region[0] <= method["offset"] < region[1]:
            return index, region, header_offset
    raise ValueError(("method region missing", method["name"]))


def region_ids_and_strings(data: bytes, region: tuple[int, ...]) -> tuple[list[int], dict[str, int]]:
    count, offset = region[4], region[5]
    ids = list(struct.unpack_from("<" + str(count) + "I", data, offset))
    strings: dict[str, int] = {}
    for index, entity_offset in enumerate(ids):
        try:
            value = read_string_item(data, entity_offset, len(data))
        except (IndexError, UnicodeDecodeError, ValueError):
            continue
        strings.setdefault(value, index)
    return ids, strings


def global_string_offset(data: bytes, value: str) -> int:
    header = HEADER.unpack_from(data)
    matches: set[int] = set()
    for index in range(header[12]):
        region = struct.unpack_from("<10I", data, header[13] + index * 40)
        count, offset = region[4], region[5]
        if count in (0, 0xFFFFFFFF) or offset in (0, 0xFFFFFFFF):
            continue
        if offset + count * 4 > len(data):
            continue
        for entity_offset in struct.unpack_from("<" + str(count) + "I", data, offset):
            try:
                if read_string_item(data, entity_offset, len(data)) == value:
                    matches.add(entity_offset)
            except (IndexError, UnicodeDecodeError, ValueError):
                pass
    if len(matches) != 1:
        raise ValueError(("global string", value, sorted(matches)))
    return next(iter(matches))


def panda_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(value.encode("utf-16-le")) // 2 != len(value):
        raise ValueError(("only ASCII additions expected", value))
    return uleb((len(value) << 1) | 1) + encoded + b"\x00"


def method_annotation(data: bytes, method: dict, tag_wanted: int = 6) -> int:
    cursor = method["offset"] + 8
    _flags, cursor = read_uleb128(data, cursor, len(data))
    while data[cursor] != 0:
        tag = data[cursor]
        cursor += 1
        if tag == 2:
            cursor += 1
            continue
        if not 1 <= tag <= 9:
            raise ValueError(("method tag", method["name"], tag, cursor - 1))
        value = struct.unpack_from("<I", data, cursor)[0]
        if tag == tag_wanted:
            return value
        cursor += 4
    raise ValueError(("annotation missing", method["name"], tag_wanted))


def slot_number(data: bytes, method: dict) -> tuple[int, int]:
    annotation = method_annotation(data, method)
    _class_idx, count = struct.unpack_from("<HH", data, annotation)
    if count != 1:
        raise ValueError(("annotation element count", method["name"], count))
    value_offset = annotation + 8
    return struct.unpack_from("<I", data, value_offset)[0], value_offset


def ic_width(item: dict) -> int:
    encoding = item["format"]
    if "sixteen_bit_ic" in item["properties"] and "eight_bit_ic" not in item["properties"]:
        return 2
    if "eight_sixteen_bit_ic" in item["properties"] and "_imm_16" in encoding.split("_id")[0]:
        return 2
    return 1


def used_small_slots(decoded: list[dict], kind: str) -> set[int]:
    used: set[int] = set()
    for item in decoded:
        if kind not in item["properties"]:
            continue
        width = ic_width(item)
        if width != 1:
            continue
        operand_offset = 2 if item["key"][0] is not None else 1
        start = item["raw"][operand_offset]
        span = 2 if "two_slot" in item["properties"] else 1
        used.update(range(start, start + span))
    return used


class SlotAllocator:
    def __init__(self, old_slot_number: int, decoded: list[dict]):
        self.old = old_slot_number
        self.high = old_slot_number
        self.used_ic = used_small_slots(decoded, "ic_slot")
        self.used_jit = used_small_slots(decoded, "jit_ic_slot")

    def alloc(self, span: int, wide_ok: bool = True, kind: str = "ic_slot") -> int:
        if self.old < 256:
            start = self.high
            if start + span <= 256:
                self.high += span
                # Compiler-generated low SlotNumber methods allocate from one
                # monotonically increasing range even though Ark maintains
                # separate IC and JIT caches.  Preserve that layout.
                self.used_ic.update(range(start, start + span))
                self.used_jit.update(range(start, start + span))
                return start
        if wide_ok:
            start = self.high
            self.high += span
            return start
        used_small = self.used_jit if kind == "jit_ic_slot" else self.used_ic
        for start in range(0, 256 - span + 1):
            if all(slot not in used_small for slot in range(start, start + span)):
                used_small.update(range(start, start + span))
                return start
        raise ValueError("no free 8-bit IC slots")

    @property
    def new_slot_number(self) -> int:
        return max(self.old, self.high)


@dataclass
class Fixup:
    position: int
    label: str | None = None
    old_target: int | None = None


class Snippet:
    def __init__(self, ids: dict[str, int], slots: SlotAllocator):
        self.ids = ids
        self.slots = slots
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[Fixup] = []

    def raw(self, *values: int):
        self.code.extend(values)

    def mark(self, label: str):
        if label in self.labels:
            raise ValueError(("duplicate label", label))
        self.labels[label] = len(self.code)

    def jump(self, opcode: int, label: str | None = None, old_target: int | None = None):
        position = len(self.code)
        self.code.extend((opcode, 0, 0))
        self.fixups.append(Fixup(position, label, old_target))

    def lda(self, reg: int): self.raw(0x60, reg)
    def sta(self, reg: int): self.raw(0x61, reg)
    def ldtrue(self): self.raw(0x02)
    def ldfalse(self): self.raw(0x03)
    def ldnull(self): self.raw(0x01)
    def ldundefined(self): self.raw(0x00)
    def ldai(self, value: int): self.code.extend((0x62,)); self.code.extend(struct.pack("<i", value))
    def ldext(self, index: int): self.raw(0x7E, index)
    def ldlex(self, depth: int, slot: int):
        if depth < 16 and slot < 16:
            self.raw(0x3C, (slot << 4) | depth)
        else:
            self.raw(0x8A, depth, slot)
    def lda_str(self, name: str): self.code.extend((0x3E,)); self.code.extend(struct.pack("<H", self.ids[name]))
    def throw_name(self, name: str): self.code.extend((0xFE, 0x09)); self.code.extend(struct.pack("<H", self.ids[name]))

    def prop(self, opcode8: int, opcode16: int, name: str, obj_reg: int | None = None):
        slot = self.slots.alloc(2, True, "ic_slot")
        if slot < 256:
            self.raw(opcode8, slot)
        else:
            self.code.extend((opcode16,)); self.code.extend(struct.pack("<H", slot))
        self.code.extend(struct.pack("<H", self.ids[name]))
        if obj_reg is not None:
            self.raw(obj_reg)

    def ldobj(self, name: str): self.prop(0x42, 0x90, name)
    def stobj(self, name: str, obj_reg: int): self.prop(0x43, 0x91, name, obj_reg)

    def call(self, argc: int, this_reg: int, *arg_regs: int):
        if argc != len(arg_regs) or not 0 <= argc <= 3:
            raise ValueError((argc, arg_regs))
        slot = self.slots.alloc(2, False, "jit_ic_slot")
        self.raw(0x2D + argc, slot, this_reg, *arg_regs)

    def truth(self, is_true: bool):
        slot = self.slots.alloc(1, False, "ic_slot")
        self.raw(0xFB, 0x13 if is_true else 0x14, slot)

    def strict(self, equal: bool, reg: int):
        slot = self.slots.alloc(1, False, "jit_ic_slot")
        self.raw(0x28 if equal else 0x27, slot, reg)

    def empty_array(self):
        slot = self.slots.alloc(1, True, "ic_slot")
        if slot < 256:
            self.raw(0x05, slot)
        else:
            self.code.extend((0x80,)); self.code.extend(struct.pack("<H", slot))

    def stown_index(self, obj_reg: int, index: int):
        slot = self.slots.alloc(2, True, "ic_slot")
        if slot < 256:
            self.raw(0x79, slot, obj_reg)
        else:
            self.code.extend((0xCB,)); self.code.extend(struct.pack("<H", slot)); self.raw(obj_reg)
        self.code.extend(struct.pack("<H", index))

    def ldobj_value(self, obj_reg: int):
        slot = self.slots.alloc(2, True, "ic_slot")
        if slot < 256:
            self.raw(0x37, slot, obj_reg)
        else:
            self.code.extend((0x85,)); self.code.extend(struct.pack("<H", slot)); self.raw(obj_reg)

    def assemble(self, base: int, target_map) -> bytes:
        result = bytearray(self.code)
        for fixup in self.fixups:
            if fixup.label is not None:
                target = base + self.labels[fixup.label]
            elif fixup.old_target is not None:
                target = target_map(fixup.old_target)
            else:
                raise ValueError(fixup)
            source = base + fixup.position
            relative = target - source
            if not -32768 <= relative <= 32767:
                raise ValueError(("snippet jump overflow", source, target, relative))
            result[fixup.position + 1 : fixup.position + 3] = struct.pack("<h", relative)
        return bytes(result)


def rewrite(code: bytes, table: dict, snippets: dict[int, Snippet]) -> bytes:
    decoded = decode(code, table)
    starts = {item["offset"] for item in decoded}
    if not set(snippets) <= starts:
        raise ValueError(("insertion not on instruction", set(snippets) - starts))
    lengths = {position: len(snippet.code) for position, snippet in snippets.items()}
    jump_variants = {
        "jmp": {1: 0x4D, 2: 0x4E, 4: 0x98},
        "jeqz": {1: 0x4F, 2: 0x50, 4: 0x9A},
        "jnez": {1: 0x51, 2: 0x9B, 4: 0x9C},
    }
    jump_widths = {
        item["offset"]: item["jump_width"] for item in decoded if "target" in item
    }

    def promotion_before(old: int) -> int:
        return sum(
            jump_widths[item["offset"]] - item["jump_width"]
            for item in decoded
            if "target" in item and item["offset"] < old
        )

    def instruction_map(old: int) -> int:
        return (
            old
            + sum(length for position, length in lengths.items() if position <= old)
            + promotion_before(old)
        )

    def target_map(old: int) -> int:
        return (
            old
            + sum(length for position, length in lengths.items() if position < old)
            + promotion_before(old)
        )

    # Inserted stock blocks can make an original 8-bit branch cross its
    # encoding range.  Promote only those existing branches, exactly as the
    # compiler would, and iterate because one promotion shifts later targets.
    changed = True
    while changed:
        changed = False
        for item in decoded:
            if "target" not in item:
                continue
            relative = target_map(item["target"]) - instruction_map(item["offset"])
            current = jump_widths[item["offset"]]
            if -(1 << (current * 8 - 1)) <= relative < (1 << (current * 8 - 1)):
                continue
            next_width = next(
                (width for width in (2, 4) if width > current and -(1 << (width * 8 - 1)) <= relative < (1 << (width * 8 - 1))),
                None,
            )
            if next_width is None:
                raise ValueError(("original jump cannot be promoted", item["offset"], item["target"], relative))
            jump_widths[item["offset"]] = next_width
            changed = True

    out = bytearray()
    for item in decoded:
        position = item["offset"]
        if position in snippets:
            if len(out) != target_map(position):
                raise ValueError(("snippet position", position, len(out), target_map(position)))
            out.extend(snippets[position].assemble(len(out), target_map))
        if len(out) != instruction_map(position):
            raise ValueError(("instruction position", position, len(out), instruction_map(position)))
        raw = bytearray(item["raw"])
        if "target" in item:
            new_source = instruction_map(position)
            new_target = target_map(item["target"])
            relative = new_target - new_source
            width = jump_widths[position]
            if not -(1 << (width * 8 - 1)) <= relative < (1 << (width * 8 - 1)):
                raise ValueError(("original jump overflow", position, item["target"], relative, width))
            if width != item["jump_width"]:
                raw = bytearray((jump_variants[item["name"]][width],))
                raw.extend(int(relative).to_bytes(width, "little", signed=True))
            else:
                raw[-width:] = int(relative).to_bytes(width, "little", signed=True)
        out.extend(raw)
    decode(bytes(out), table)
    return bytes(out)


def session_gate(ids: dict[str, int], slots: SlotAllocator, maximize: bool) -> Snippet:
    s = Snippet(ids, slots)
    a2 = 14
    s.ldext(0x27); s.sta(0); s.throw_name("SCBSceneSessionManager")
    s.lda(0); s.ldobj("getInstance"); s.call(0, 0); s.sta(0)
    s.lda(0); s.ldobj("isPc"); s.call(0, 0); s.truth(False); s.jump(0x9B, "generic")
    s.lda(0); s.ldobj("isAncoApplication"); s.sta(1)
    s.lda(a2); s.ldobj("sceneInfo"); s.ldobj("bundleName"); s.sta(2)
    s.lda(1); s.call(1, 0, 2); s.truth(False); s.jump(0x9B, "generic")
    s.ldext(0x45); s.sta(1); s.throw_name("f5349")
    s.lda(1); s.ldobj("getSync"); s.sta(2)
    s.lda_str("hmos_fusion.container.pc.freemode.captionbar"); s.sta(3)
    s.lda_str("0"); s.sta(4)
    s.lda(2); s.call(2, 1, 3, 4); s.sta(1)
    s.lda_str("1"); s.strict(False, 1); s.jump(0x9B, "generic")
    if maximize:
        s.lda(a2); s.ldobj("isOnlySupportFullScreen"); s.call(0, a2)
        s.truth(True); s.jump(0x9B, "generic")
    s.lda(0); s.ldobj("processCompatibleMaximizeOrRecover"); s.sta(1)
    s.ldtrue() if maximize else s.ldfalse(); s.sta(2)
    s.lda(a2); s.ldobj("sceneInfo"); s.ldobj("screenId"); s.sta(3)
    s.lda(a2); s.ldobj("session"); s.ldobj("persistentId"); s.sta(4)
    s.lda(1); s.call(3, 0, 2, 3, 4)
    s.jump(0x4E, old_target=0x668)
    s.mark("generic")
    return s


def scene_type_conversion(ids: dict[str, int], slots: SlotAllocator) -> Snippet:
    s = Snippet(ids, slots)
    a2, a3 = 6, 7
    s.ldext(0x0F); s.sta(1); s.throw_name("PCWindowShowType")
    s.lda(1); s.ldobj("DEFAULTWINDOW"); s.strict(False, 0); s.jump(0x9B, "done")
    s.ldext(0x26); s.sta(1); s.throw_name("SCBSceneSessionManager")
    s.lda(1); s.ldobj("getInstance"); s.call(0, 1); s.sta(1)
    s.lda(1); s.ldobj("isAncoApplication"); s.sta(2)
    s.lda(a2); s.ldobj("sceneSession"); s.ldobj("sceneInfo"); s.ldobj("bundleName"); s.sta(3)
    s.lda(2); s.call(1, 1, 3); s.truth(False); s.jump(0x9B, "done")
    s.lda(a3); s.truth(False); s.jump(0x9B, "done")
    s.lda(a2); s.ldobj("sceneSession"); s.sta(1)
    s.lda(0); s.stobj("lastWindowShowType", 1)
    s.ldext(0x0F); s.sta(2); s.throw_name("PCWindowShowType")
    s.lda(2); s.ldobj("FULLSCREENSCALE_LANDSCAPEWINDOW"); s.sta(0)
    s.lda(0); s.stobj("pcWindowShowType", 1)
    s.mark("done")
    return s


def save_position(ids: dict[str, int], slots: SlotAllocator) -> Snippet:
    s = Snippet(ids, slots)
    s.ldlex(0, 4); s.ldobj("sceneSession"); s.sta(1)
    s.empty_array(); s.sta(2)
    s.lda(1); s.ldobj("currRect"); s.ldobj("left"); s.sta(3)
    s.lda(3); s.ldobj("getPx"); s.call(0, 3); s.stown_index(2, 0)
    s.lda(1); s.ldobj("currRect"); s.ldobj("top"); s.sta(3)
    s.lda(3); s.ldobj("getPx"); s.call(0, 3); s.stown_index(2, 1)
    s.lda(2); s.stobj("lastPosForCompatible", 1)
    return s


def maximize_anco_geometry(ids: dict[str, int], slots: SlotAllocator) -> Snippet:
    s = Snippet(ids, slots)
    s.ldext(0x26); s.sta(1); s.throw_name("SCBSceneSessionManager")
    s.lda(1); s.ldobj("getInstance"); s.call(0, 1); s.sta(1)
    s.lda(1); s.ldobj("isAncoApplication"); s.sta(2)
    s.ldlex(0, 4); s.ldobj("sceneSession"); s.ldobj("sceneInfo"); s.ldobj("bundleName"); s.sta(3)
    s.lda(2); s.call(1, 1, 3); s.truth(False); s.jump(0x9B, "done")
    s.ldlex(0, 4); s.ldobj("presenter"); s.sta(1)
    s.lda(1); s.truth(False); s.jump(0x9B, "done")
    s.lda(1); s.ldobj("getRealScreenInfo"); s.sta(2)
    s.ldlex(0, 4); s.ldobj("screenProperty"); s.sta(3)
    s.lda(2); s.call(1, 1, 3); s.sta(1)
    s.lda(1); s.truth(False); s.jump(0x9B, "done")
    s.ldlex(0, 4); s.ldobj("sceneSession"); s.sta(2)
    s.empty_array(); s.sta(3)
    s.lda(2); s.ldobj("currRect"); s.ldobj("width"); s.sta(4)
    s.lda(4); s.ldobj("getPx"); s.call(0, 4); s.stown_index(3, 0)
    s.lda(2); s.ldobj("currRect"); s.ldobj("height"); s.sta(4)
    s.lda(4); s.ldobj("getPx"); s.call(0, 4); s.stown_index(3, 1)
    s.lda(3); s.stobj("lastSizeForCompatible", 2)
    s.lda(2); s.ldobj("currRect"); s.sta(3); s.ldobj("setSizeNum"); s.sta(4)
    s.lda(1); s.ldobj("screenWidth"); s.sta(5)
    s.lda(1); s.ldobj("screenHeight"); s.sta(6)
    s.lda(4); s.call(2, 3, 5, 6)
    s.lda(2); s.ldobj("currRect"); s.sta(3); s.ldobj("setPosNum"); s.sta(4)
    s.ldai(0); s.sta(5)
    s.lda(1); s.ldobj("fullScreenTop"); s.sta(6)
    s.lda(4); s.call(2, 3, 5, 6)
    s.mark("done")
    return s


def skip_anco_calibration(ids: dict[str, int], slots: SlotAllocator, old_end: int) -> Snippet:
    s = Snippet(ids, slots)
    s.ldext(0x26); s.sta(0); s.throw_name("SCBSceneSessionManager")
    s.lda(0); s.ldobj("getInstance"); s.call(0, 0); s.sta(0)
    s.lda(0); s.ldobj("isAncoApplication"); s.sta(1)
    s.ldlex(0, 4); s.ldobj("sceneSession"); s.ldobj("sceneInfo"); s.ldobj("bundleName"); s.sta(2)
    s.lda(1); s.call(1, 0, 2); s.truth(False); s.jump(0x9B, "normal")
    s.jump(0x4E, old_target=old_end)
    s.mark("normal")
    return s


def recover_geometry(ids: dict[str, int], slots: SlotAllocator) -> Snippet:
    s = Snippet(ids, slots)
    # Restore the position saved for every compatible maximize.
    s.ldlex(0, 4); s.ldobj("sceneSession"); s.sta(0)
    s.ldobj("currRect"); s.sta(1); s.ldobj("setPosNum"); s.sta(2)
    s.lda(0); s.ldobj("lastPosForCompatible"); s.sta(3)
    s.ldai(0); s.ldobj_value(3); s.sta(3)
    s.lda(0); s.ldobj("lastPosForCompatible"); s.sta(4)
    s.ldai(1); s.ldobj_value(4); s.sta(4)
    s.lda(2); s.call(2, 1, 3, 4)
    # SP68 restores size/type only for AnCo applications.
    s.ldext(0x26); s.sta(0); s.throw_name("SCBSceneSessionManager")
    s.lda(0); s.ldobj("getInstance"); s.call(0, 0); s.sta(0)
    s.lda(0); s.ldobj("isAncoApplication"); s.sta(1)
    s.ldlex(0, 4); s.ldobj("sceneSession"); s.ldobj("sceneInfo"); s.ldobj("bundleName"); s.sta(2)
    s.lda(1); s.call(1, 0, 2); s.truth(False); s.jump(0x9B, "done")
    s.ldlex(0, 4); s.ldobj("sceneSession"); s.sta(0)
    s.ldobj("currRect"); s.sta(1); s.ldobj("setSizeNum"); s.sta(2)
    s.lda(0); s.ldobj("lastSizeForCompatible"); s.sta(3)
    s.ldai(0); s.ldobj_value(3); s.sta(3)
    s.lda(0); s.ldobj("lastSizeForCompatible"); s.sta(4)
    s.ldai(1); s.ldobj_value(4); s.sta(4)
    s.lda(2); s.call(2, 1, 3, 4)
    s.lda(0); s.ldobj("pcWindowShowType"); s.sta(1)
    s.ldext(0x0F); s.sta(2); s.throw_name("PCWindowShowType")
    s.lda(2); s.ldobj("FULLSCREENSCALE_LANDSCAPEWINDOW"); s.strict(True, 1); s.jump(0x50, "done")
    s.lda(0); s.ldobj("lastWindowShowType"); s.stobj("pcWindowShowType", 0)
    s.mark("done")
    return s


def extract_method_text(path: Path, full_name_fragment: str) -> str:
    lines: list[str] = []
    found = False
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if not found:
                if line.startswith(".function ") and full_name_fragment in line:
                    found = True
                    lines.append(line)
            else:
                lines.append(line)
                if line.rstrip() == "}":
                    break
    if not found:
        raise ValueError(("disassembled method missing", full_name_fragment))
    return "".join(lines)


def build(source: Path, out: Path, compiler: Path):
    if out.exists():
        raise FileExistsError(out)
    out.mkdir(parents=True)
    with zipfile.ZipFile(source) as archive:
        names = [name for name in archive.namelist() if name.endswith("/modules.abc")]
        if names != ["ets/modules.abc"]:
            raise ValueError(names)
        original = archive.read(names[0])
    (out / "modules.original.abc").write_bytes(original)

    methods = find_methods(original)
    expected_shapes = {
        SESSION_METHOD: (12, 5, 1669, 0),
        SCENE_METHOD: (4, 4, 222, 0),
        MAX_CLOSURE: (12, 3, 298, 0),
        RECOVER_CLOSURE: (5, 3, 190, 0),
    }
    for name, method in methods.items():
        actual = (method["num_vregs"], method["num_args"], method["code_size"], method["tries_size"])
        if actual != expected_shapes[name]:
            raise ValueError(("method shape", name, actual, expected_shapes[name]))

    regions: dict[int, dict] = {}
    method_region_index: dict[str, int] = {}
    for name, method in methods.items():
        index, region, header_offset = method_region(original, method)
        method_region_index[name] = index
        regions.setdefault(index, {"region": region, "header_offset": header_offset})
    if method_region_index[SESSION_METHOD] == method_region_index[SCENE_METHOD]:
        raise ValueError("expected separate index regions")

    patched = bytearray(original)
    string_offsets: dict[str, int] = {}
    for value in NEW_STRINGS:
        try:
            global_string_offset(original, value)
        except ValueError:
            string_offsets[value] = len(patched)
            patched.extend(panda_string(value))
        else:
            raise ValueError(("new string unexpectedly already exists", value))

    # Populate the two target region string tables.  Existing entries retain
    # their exact IDs; only stock SP68 names absent from HarmonyOS 6.0 append.
    for index, info in regions.items():
        ids, strings = region_ids_and_strings(original, info["region"])
        additions: list[str]
        if index == method_region_index[SESSION_METHOD]:
            additions = ["hmos_fusion.container.pc.freemode.captionbar"]
        else:
            additions = ["isAncoApplication", "lastWindowShowType", "lastPosForCompatible", "lastSizeForCompatible"]
        for value in additions:
            if value in strings:
                continue
            offset = string_offsets.get(value)
            if offset is None:
                offset = global_string_offset(original, value)
            ids.append(offset)
            strings[value] = len(ids) - 1
        if len(ids) > 65535:
            raise ValueError(("region ID overflow", index, len(ids)))
        info.update(ids=ids, strings=strings, old_count=info["region"][4])

    table = instruction_table()
    rewritten: dict[str, bytes] = {}
    method_reports: dict[str, dict] = {}
    for name, method in methods.items():
        code = original[method["insn_off"] : method["insn_off"] + method["code_size"]]
        decoded = decode(code, table)
        old_slots, annotation_value_offset = slot_number(original, method)
        slots = SlotAllocator(old_slots, decoded)
        ids = regions[method_region_index[name]]["strings"]
        if name == SESSION_METHOD:
            snippets = {
                0x0F9: session_gate(ids, slots, True),
                0x408: session_gate(ids, slots, False),
            }
        elif name == SCENE_METHOD:
            snippets = {0x03C: scene_type_conversion(ids, slots)}
        elif name == MAX_CLOSURE:
            snippets = {
                0x032: save_position(ids, slots),
                0x0CA: maximize_anco_geometry(ids, slots),
                0x0FD: skip_anco_calibration(ids, slots, 0x129),
            }
        else:
            snippets = {0x091: recover_geometry(ids, slots)}
        new_code = rewrite(code, table, snippets)
        rewritten[name] = new_code
        method_reports[name] = {
            "old_code_size": len(code),
            "new_code_size": len(new_code),
            "insertions": {hex(position): len(snippet.code) for position, snippet in snippets.items()},
            "old_slot_number": old_slots,
            "new_slot_number": slots.new_slot_number,
            "annotation_value_offset": annotation_value_offset,
        }

    while len(patched) % 4:
        patched.append(0)
    for index, info in regions.items():
        info["new_index_offset"] = len(patched)
        patched.extend(struct.pack("<" + str(len(info["ids"])) + "I", *info["ids"]))
        struct.pack_into("<I", patched, info["header_offset"] + 16, len(info["ids"]))
        struct.pack_into("<I", patched, info["header_offset"] + 20, info["new_index_offset"])

    for name, method in methods.items():
        new_code = rewritten[name]
        code_offset = len(patched)
        patched.extend(uleb(method["num_vregs"]))
        patched.extend(uleb(method["num_args"]))
        patched.extend(uleb(len(new_code)))
        patched.extend(uleb(0))
        patched.extend(new_code)
        struct.pack_into("<I", patched, method_code_pointer_offset(original, method), code_offset)
        struct.pack_into("<I", patched, method_reports[name]["annotation_value_offset"], method_reports[name]["new_slot_number"])
        method_reports[name]["new_code_offset"] = code_offset
    struct.pack_into("<I", patched, 16, len(patched))
    patched_input = out / "modules.patched.input.abc"
    patched_input.write_bytes(patched)

    commands = {}
    for label, input_path in (("baseline", out / "modules.original.abc"), ("candidate", patched_input)):
        commands[label] = compile_abc(compiler, input_path, out / f"modules.{label}.abc", out / f"{label}-compile.log")
        (out / f"{label}-command.json").write_text(json.dumps(commands[label], indent=2) + "\n", encoding="utf-8")

    baseline = (out / "modules.baseline.abc").read_bytes()
    candidate_path = out / "modules.candidate.abc"
    candidate = candidate_path.read_bytes()
    baseline_methods = find_methods(baseline)
    candidate_methods = find_methods(candidate)
    for name, method in candidate_methods.items():
        expected = (expected_shapes[name][0], expected_shapes[name][1], 0)
        actual = (method["num_vregs"], method["num_args"], method["tries_size"])
        if actual != expected or method["code_size"] <= expected_shapes[name][2]:
            raise ValueError(("candidate roundtrip shape", name, actual, expected, method["code_size"]))

    baseline_pa = out / "modules.baseline.pa"
    candidate_pa = out / "modules.candidate.pa"
    disassemble(out / "modules.baseline.abc", baseline_pa, out / "baseline-disasm.log")
    disassemble(candidate_path, candidate_pa, out / "candidate-disasm.log")
    fragments = {
        SESSION_METHOD: "SCBSceneSession&1.0.0.#~@c>#onSessionEvent",
        SCENE_METHOD: "SCBScene&1.0.0.#~@1>#onCompatibleModeMaximizeOrRecover",
        MAX_CLOSURE: "SCBScene&1.0.0.#~@1>@2*#(any",
        RECOVER_CLOSURE: "SCBScene&1.0.0.#~@1>@2*#^1(any",
    }
    texts = {}
    for name, fragment in fragments.items():
        text = extract_method_text(candidate_pa, fragment)
        texts[name] = text
        safe = {SESSION_METHOD: "onSessionEvent", SCENE_METHOD: "onCompatibleModeMaximizeOrRecover", MAX_CLOSURE: "maximizeClosure", RECOVER_CLOSURE: "recoverClosure"}[name]
        (out / f"{safe}.candidate.pa").write_text(text, encoding="utf-8")

    required_by_method = {
        SESSION_METHOD: ("processCompatibleMaximizeOrRecover", "hmos_fusion.container.pc.freemode.captionbar", "isOnlySupportFullScreen"),
        SCENE_METHOD: ("DEFAULTWINDOW", "lastWindowShowType", "FULLSCREENSCALE_LANDSCAPEWINDOW"),
        MAX_CLOSURE: ("lastPosForCompatible", "lastSizeForCompatible", "getRealScreenInfo", "setSizeNum", "setPosNum"),
        RECOVER_CLOSURE: ("lastPosForCompatible", "lastSizeForCompatible", "lastWindowShowType", "setSizeNum", "setPosNum"),
    }
    for name, required in required_by_method.items():
        missing = [item for item in required if item not in texts[name]]
        if missing:
            raise ValueError(("candidate disassembly missing stock route", name, missing))

    package = out / "SceneBoard.sp68-anco-maximize-recover.hap"
    with zipfile.ZipFile(source) as before, zipfile.ZipFile(package, "w") as after:
        for entry in before.infolist():
            after.writestr(entry, candidate if entry.filename == names[0] else before.read(entry.filename))
    with zipfile.ZipFile(source) as before, zipfile.ZipFile(package) as after:
        unchanged = [name for name in before.namelist() if name != names[0]]
        if before.namelist() != after.namelist():
            raise ValueError("ZIP member order changed")
        if not all(before.read(name) == after.read(name) for name in unchanged):
            raise ValueError("unrelated ZIP member changed")
        if after.read(names[0]) != candidate:
            raise ValueError("candidate ABC packaging failed")

    report = {
        "source": str(source.resolve()),
        "source_bytes": source.stat().st_size,
        "candidate": str(package.resolve()),
        "candidate_bytes": package.stat().st_size,
        "methods": method_reports,
        "regions": {
            str(index): {
                "old_count": info["old_count"],
                "new_count": len(info["ids"]),
                "new_index_offset": info["new_index_offset"],
            }
            for index, info in regions.items()
        },
        "new_strings": string_offsets,
        "baseline_roundtrip_shapes": {
            name: [method["num_vregs"], method["num_args"], method["code_size"], method["tries_size"]]
            for name, method in baseline_methods.items()
        },
        "candidate_roundtrip_shapes": {
            name: [method["num_vregs"], method["num_args"], method["code_size"], method["tries_size"]]
            for name, method in candidate_methods.items()
        },
        "all_required_stock_route_tokens_in_disassembly": True,
        "unchanged_zip_members": len(unchanged),
        "all_other_zip_member_bytes_equal": True,
        "device_loaded": False,
        "runtime_verified": False,
        "scope": "SP68 AnCo compatible maximize/recover entry, window type, position and size semantics only; no new interface",
    }
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--compiler",
        type=Path,
        default=ROOT / "audit-alignment-v306/tile-sdk/ets-windows/ets/build-tools/ets-loader/bin/ark/build-win/bin/es2abc.exe",
    )
    args = parser.parse_args()
    build(args.source, args.output, args.compiler)
