"""Backport the SP68 SceneBoard FULLSCREEN title-hot-area condition.

The active HarmonyOS 6.0 SceneBoard stops publishing the container title hot
area after an AnCo window enters scene mode FULLSCREEN because
``updateTitleForContainer`` only checks two older full-screen flags.  SP68 also
checks ``primarySession.isInSceneMode(SCBSceneMode.FULLSCREEN)``.  This builder
adds only that original predicate to the active method, then round-trips both
the baseline and candidate with the device SDK compiler and verifies the
resulting assembly.

No host API, window action, or Linux-only lifecycle rule is introduced.
"""

from __future__ import annotations

from pathlib import Path
import argparse
import json
import re
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


RECORD = "L&@ohos/multiinput/src/main/ets/default/common/viewmodel/MultiInputViewModel&1.0.0;"
METHOD = "#~@0>#updateTitleForContainer"
ISA = ROOT / "arkcompiler-asm-standalone-v1/isa/isa.yaml"
DISASSEMBLER = ROOT / "tools/oh6-sdk-extract/toolchains-windows/toolchains/ark_disasm.exe"
PREFIXES = {"throw": 0xFE, "wide": 0xFD, "deprecated": 0xFC, "callruntime": 0xFB}


def uleb(value: int) -> bytes:
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        result.append(byte)
        if not value:
            return bytes(result)


def instruction_table() -> dict[tuple[int | None, int], dict]:
    text = ISA.read_text(encoding="utf-8")
    table = {}
    blocks = re.split(r"(?=^      - sig: )", text, flags=re.MULTILINE)[1:]
    for block in blocks:
        signature = re.search(r"^      - sig: (.+)$", block, re.MULTILINE).group(1)
        opcodes = [
            int(item.strip(), 0)
            for item in re.search(
                r"^        opcode_idx: \[(.*?)\]$", block, re.MULTILINE
            ).group(1).split(",")
        ]
        formats = [
            item.strip()
            for item in re.search(
                r"^        format: \[(.*?)\]$", block, re.MULTILINE
            ).group(1).split(",")
        ]
        prefix_match = re.search(r"^        prefix: (\w+)$", block, re.MULTILINE)
        prefix = PREFIXES[prefix_match.group(1)] if prefix_match else None
        properties_match = re.search(
            r"^        properties: \[(.*?)\]$", block, re.MULTILINE
        )
        properties = (
            []
            if properties_match is None
            else [item.strip() for item in properties_match.group(1).split(",")]
        )
        for opcode, encoding in zip(opcodes, formats):
            bits = (16 if encoding.startswith("pref_op") else 8) + sum(
                int(item) for item in re.findall(r"_(\d+)", encoding)
            )
            table[(prefix, opcode)] = {
                "name": signature.split()[0],
                "size": (bits + 7) // 8,
                "format": encoding,
                "properties": properties,
            }
    return table


def decode(code: bytes, table: dict) -> list[dict]:
    result = []
    offset = 0
    prefix_values = set(PREFIXES.values())
    while offset < len(code):
        first = code[offset]
        key = (first, code[offset + 1]) if first in prefix_values else (None, first)
        metadata = table.get(key)
        if metadata is None:
            raise ValueError(("unknown opcode", offset, code[offset : offset + 12].hex()))
        size = metadata["size"]
        if offset + size > len(code):
            raise ValueError(("truncated instruction", offset, size))
        item = dict(metadata)
        item.update(offset=offset, key=key, raw=code[offset : offset + size])
        if item["name"] in {"jmp", "jeqz", "jnez"}:
            opcode_size = 2 if key[0] is not None else 1
            width = size - opcode_size
            relative = int.from_bytes(item["raw"][-width:], "little", signed=True)
            item.update(relative=relative, target=offset + relative, jump_width=width)
        result.append(item)
        offset += size
    if offset != len(code):
        raise ValueError((offset, len(code)))
    return result


def find_target(data: bytes) -> tuple[dict, int, tuple[int, ...], int]:
    header = HEADER.unpack_from(data)
    method = None
    for index in range(header[6]):
        record_offset = struct.unpack_from("<I", data, header[7] + 4 * index)[0]
        record = parse_record_prefix(data, record_offset, len(data))
        if record["name"] != RECORD:
            continue
        _, methods = parse_record_members(data, record, len(data))
        method = next(item for item in methods if item["name"] == METHOD)
        break
    if method is None:
        raise ValueError("target method missing")
    for region_index in range(header[12]):
        region = struct.unpack_from("<10I", data, header[13] + region_index * 40)
        if region[0] <= method["offset"] < region[1]:
            return method, region_index, region, header[13] + region_index * 40
    raise ValueError("target index region missing")


def region_strings(data: bytes, region: tuple[int, ...]) -> tuple[list[int], dict[str, int]]:
    count, offset = region[4], region[5]
    ids = list(struct.unpack_from("<" + str(count) + "I", data, offset))
    wanted = {
        "mFirstFullOrSplitContainer",
        "primarySession",
        "isInSceneMode",
        "SCBSceneMode",
        "FULLSCREEN",
    }
    found = {}
    for index, entity_offset in enumerate(ids):
        try:
            value = read_string_item(data, entity_offset, len(data))
        except (IndexError, UnicodeDecodeError, ValueError):
            continue
        if value in wanted and value not in found:
            found[value] = index
    return ids, found


def global_string_offset(data: bytes, value: str) -> int:
    header = HEADER.unpack_from(data)
    matches = set()
    for region_index in range(header[12]):
        region = struct.unpack_from("<10I", data, header[13] + region_index * 40)
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
        raise ValueError((value, sorted(matches)))
    return next(iter(matches))


def emit(opcode: int, *operands: int) -> bytes:
    return bytes((opcode, *operands))


def emit_id(opcode: int, slot: int, index: int) -> bytes:
    return bytes((opcode, slot)) + struct.pack("<H", index)


def build_original_branch(ids: dict[str, int], a2_register: int, success_delta: int) -> bytes:
    """Emit only SP68's original isInSceneMode(FULLSCREEN) OR predicate."""
    code = bytearray()
    labels: dict[str, int] = {}
    fixups: list[tuple[int, str]] = []

    def mark(name: str):
        labels[name] = len(code)

    def jump(opcode: int, target: str):
        position = len(code)
        code.extend((opcode, 0))
        fixups.append((position, target))

    code.extend(emit(0x60, a2_register))
    code.extend(emit_id(0x42, 0x0E, ids["mFirstFullOrSplitContainer"]))
    code.extend(emit(0x61, 0x00))
    code.extend(emit(0x01))
    code.extend(emit(0x27, 0x10, 0x00))
    jump(0x4F, "outer_null")
    code.extend(emit(0x00))
    code.extend(emit(0x27, 0x11, 0x00))
    jump(0x51, "outer_value")
    mark("outer_null")
    code.extend(emit(0x00))
    jump(0x4D, "end_optional")
    mark("outer_value")
    code.extend(emit(0x60, 0x00))
    code.extend(emit_id(0x42, 0x12, ids["primarySession"]))
    code.extend(emit(0x61, 0x00))
    code.extend(emit(0x01))
    code.extend(emit(0x27, 0x14, 0x00))
    jump(0x4F, "session_null")
    code.extend(emit(0x00))
    code.extend(emit(0x27, 0x15, 0x00))
    jump(0x51, "session_value")
    mark("session_null")
    code.extend(emit(0x00))
    jump(0x4D, "end_optional")
    mark("session_value")
    code.extend(emit(0x60, 0x00))
    code.extend(emit_id(0x42, 0x16, ids["isInSceneMode"]))
    code.extend(emit(0x61, 0x01))
    code.extend(emit(0x7E, 0x1B))
    code.extend(emit(0x61, 0x02))
    code.extend(bytes((0xFE, 0x09)) + struct.pack("<H", ids["SCBSceneMode"]))
    code.extend(emit(0x60, 0x02))
    code.extend(emit_id(0x42, 0x18, ids["FULLSCREEN"]))
    code.extend(emit(0x61, 0x02))
    code.extend(emit(0x60, 0x01))
    code.extend(emit(0x2E, 0x1A, 0x00, 0x02))
    mark("end_optional")
    code.extend(emit(0xFB, 0x13, 0x1C))
    # The SDK canonicalizer uses the 16-bit JNEZ encoding for this forward
    # branch.  Emit it directly so the input and round-tripped code sizes stay
    # identical and the verification remains exact.
    success_jump = len(code)
    code.extend((0x9B, 0, 0))

    for position, target in fixups:
        relative = labels[target] - position
        if not -128 <= relative <= 127:
            raise ValueError((position, target, relative))
        code[position + 1] = relative & 0xFF
    relative = success_delta + len(code) - success_jump
    if not -32768 <= relative <= 32767:
        raise ValueError(("success jump", relative))
    code[success_jump + 1 : success_jump + 3] = int(relative).to_bytes(
        2, "little", signed=True
    )
    return bytes(code)


def ic_width(item: dict) -> int:
    encoding = item["format"]
    if "sixteen_bit_ic" in item["properties"] and "eight_bit_ic" not in item["properties"]:
        return 2
    if "eight_sixteen_bit_ic" in item["properties"] and "_imm_16" in encoding.split("_id")[0]:
        return 2
    return 1


def patch_code(original: bytes, method: dict, ids: dict[str, int], table: dict) -> tuple[bytes, dict]:
    code = original[method["insn_off"] : method["insn_off"] + method["code_size"]]
    decoded = decode(code, table)
    marker = next(
        item
        for item in decoded
        if item["offset"] == 0x40
        and item["name"] == "callruntime.isfalse"
        and item["raw"] == bytes.fromhex("fb140d")
    )
    following = next(item for item in decoded if item["offset"] == marker["offset"] + marker["size"])
    if not (
        following["name"] == "jnez"
        and following["jump_width"] == 2
        and following["target"] == 0xA8
    ):
        raise ValueError(("unexpected original branch", following))
    insertion = following["offset"] + following["size"]
    success = following["target"]
    branch = build_original_branch(ids, method["num_vregs"] + 2, success - insertion)

    before = bytearray(code[:insertion])
    crossing = following["offset"]
    new_relative = following["relative"] + len(branch)
    before[crossing + 1 : crossing + 3] = int(new_relative).to_bytes(2, "little", signed=True)

    after = bytearray(code[insertion:])
    shifted_slots = []
    for item in decoded:
        if item["offset"] < insertion:
            continue
        if not ({"ic_slot", "jit_ic_slot"} & set(item["properties"])):
            continue
        width = ic_width(item)
        operand_offset = item["offset"] - insertion + (2 if item["key"][0] is not None else 1)
        old = int.from_bytes(after[operand_offset : operand_offset + width], "little")
        new = old + 0x0F
        if new >= 1 << (width * 8):
            raise ValueError((item["offset"], item["name"], old, new, width))
        after[operand_offset : operand_offset + width] = new.to_bytes(width, "little")
        shifted_slots.append((item["offset"], item["name"], old, new))

    result = bytes(before + branch + after)
    decode(result, table)
    return result, {
        "old_code_size": len(code),
        "new_code_size": len(result),
        "insertion_offset": insertion,
        "inserted_bytes": len(branch),
        "old_success_offset": success,
        "new_success_offset": success + len(branch),
        "crossing_jump_old_relative": following["relative"],
        "crossing_jump_new_relative": new_relative,
        "shifted_ic_slots": shifted_slots,
    }


def method_code_pointer_offset(data: bytes, method: dict) -> int:
    cursor = method["offset"] + 8
    _access_flags, cursor = read_uleb128(data, cursor, len(data))
    if data[cursor] != 1:
        raise ValueError(("method has no CODE tag", cursor, data[cursor]))
    return cursor + 1


def compile_abc(compiler: Path, source: Path, output: Path, log: Path) -> list[str]:
    command = [
        str(compiler),
        "--enable-abc-input",
        "--merge-abc",
        "--target-api-version=18",
        "--output=" + str(output.resolve()),
        str(source.resolve()),
    ]
    with log.open("wb") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError((result.returncode, command, log))
    return command


def disassemble(source: Path, output: Path, log: Path):
    with log.open("wb") as stream:
        result = subprocess.run(
            [str(DISASSEMBLER), str(source.resolve()), str(output.resolve())],
            stdout=stream,
            stderr=subprocess.STDOUT,
        )
    if result.returncode:
        raise RuntimeError((result.returncode, source, log))


def extract_method_text(path: Path) -> str:
    prefix = ".function any &@ohos.multiinput.src.main.ets.default.common.viewmodel.MultiInputViewModel&1.0.0.#~@0>#updateTitleForContainer"
    lines = []
    found = False
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if not found:
                if line.startswith(prefix):
                    found = True
                    lines.append(line)
            else:
                lines.append(line)
                if line.rstrip() == "}":
                    break
    if not found:
        raise ValueError(("disassembled method missing", path))
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

    method, region_index, region, region_header_offset = find_target(original)
    expected_shape = (4, 5, 547, 0)
    actual_shape = (
        method["num_vregs"], method["num_args"], method["code_size"], method["tries_size"]
    )
    if actual_shape != expected_shape:
        raise ValueError((actual_shape, expected_shape))
    ids, found = region_strings(original, region)
    if "isInSceneMode" in found:
        raise ValueError(("target region unexpectedly already contains isInSceneMode", found))
    if region[4] != 65534:
        raise ValueError(("unexpected target id count", region[4]))
    is_in_scene_mode_offset = global_string_offset(original, "isInSceneMode")
    ids.append(is_in_scene_mode_offset)
    found["isInSceneMode"] = len(ids) - 1
    required = {
        "mFirstFullOrSplitContainer", "primarySession", "isInSceneMode", "SCBSceneMode", "FULLSCREEN"
    }
    if set(found) != required:
        raise ValueError(("missing target ids", found))

    new_code, code_report = patch_code(original, method, found, instruction_table())
    patched = bytearray(original)
    while len(patched) % 4:
        patched.append(0)
    new_index_offset = len(patched)
    patched.extend(struct.pack("<" + str(len(ids)) + "I", *ids))
    new_code_offset = len(patched)
    patched.extend(uleb(method["num_vregs"]))
    patched.extend(uleb(method["num_args"]))
    patched.extend(uleb(len(new_code)))
    patched.extend(uleb(0))
    patched.extend(new_code)
    struct.pack_into("<I", patched, region_header_offset + 16, len(ids))
    struct.pack_into("<I", patched, region_header_offset + 20, new_index_offset)
    struct.pack_into("<I", patched, method_code_pointer_offset(original, method), new_code_offset)
    struct.pack_into("<I", patched, 16, len(patched))
    patched_input = out / "modules.patched.input.abc"
    patched_input.write_bytes(patched)

    commands = {}
    for label, input_path in (("baseline", out / "modules.original.abc"), ("candidate", patched_input)):
        commands[label] = compile_abc(
            compiler,
            input_path,
            out / f"modules.{label}.abc",
            out / f"{label}-compile.log",
        )
        (out / f"{label}-command.json").write_text(
            json.dumps(commands[label], indent=2) + "\n", encoding="utf-8"
        )

    baseline = (out / "modules.baseline.abc").read_bytes()
    candidate_path = out / "modules.candidate.abc"
    candidate = candidate_path.read_bytes()
    baseline_method, _, _, _ = find_target(baseline)
    candidate_method, _, candidate_region, _ = find_target(candidate)
    if (
        baseline_method["num_vregs"], baseline_method["num_args"], baseline_method["code_size"], baseline_method["tries_size"]
    ) != expected_shape:
        raise ValueError(("baseline roundtrip shape", baseline_method))
    expected_candidate_shape = (4, 5, len(new_code), 0)
    if (
        candidate_method["num_vregs"], candidate_method["num_args"], candidate_method["code_size"], candidate_method["tries_size"]
    ) != expected_candidate_shape:
        raise ValueError(("candidate roundtrip shape", candidate_method, expected_candidate_shape))
    _, candidate_ids = region_strings(candidate, candidate_region)
    if "isInSceneMode" not in candidate_ids:
        raise ValueError(("candidate region lost isInSceneMode", candidate_ids))

    baseline_pa = out / "modules.baseline.pa"
    candidate_pa = out / "modules.candidate.pa"
    disassemble(out / "modules.baseline.abc", baseline_pa, out / "baseline-disasm.log")
    disassemble(candidate_path, candidate_pa, out / "candidate-disasm.log")
    baseline_text = extract_method_text(baseline_pa)
    candidate_text = extract_method_text(candidate_pa)
    (out / "updateTitleForContainer.baseline.pa").write_text(baseline_text, encoding="utf-8")
    (out / "updateTitleForContainer.candidate.pa").write_text(candidate_text, encoding="utf-8")
    required_lines = (
        'ldobjbyname 0x16, "isInSceneMode"',
        "ldexternalmodulevar 0x1b",
        'throw.undefinedifholewithname "SCBSceneMode"',
        'ldobjbyname 0x18, "FULLSCREEN"',
        "callthis1 0x1a, v0, v2",
    )
    if any(line not in candidate_text for line in required_lines):
        raise ValueError(("candidate disassembly lacks SP68 predicate", required_lines))
    if "isInSceneMode" in baseline_text:
        raise ValueError("baseline unexpectedly contains predicate")

    package = out / "SceneBoard.sp68-title-fullscreen.hap"
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
        "active_method": method,
        "target_index_region": region_index,
        "old_region_id_count": region[4],
        "new_input_region_id_count": len(ids),
        "isInSceneMode_global_string_offset": is_in_scene_mode_offset,
        "isInSceneMode_target_id": found["isInSceneMode"],
        "code_change": code_report,
        "baseline_roundtrip_method": baseline_method,
        "candidate_roundtrip_method": candidate_method,
        "candidate_roundtrip_region_id_count": candidate_region[4],
        "candidate_disassembly_has_exact_sp68_predicate": True,
        "unchanged_zip_members": len(unchanged),
        "all_other_zip_member_bytes_equal": True,
        "device_loaded": False,
        "runtime_verified": False,
        "scope": "SP68 isInSceneMode(FULLSCREEN) title-hot-area predicate only; no new interface",
    }
    (out / "verification.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--compiler",
        type=Path,
        default=ROOT
        / "audit-alignment-v306/tile-sdk/ets-windows/ets/build-tools/ets-loader/bin/ark/build-win/bin/es2abc.exe",
    )
    arguments = parser.parse_args()
    build(arguments.source, arguments.output, arguments.compiler)
