"""Keep Linux SNI tray menus authoritative in the HarmonyOS status bar.

SceneBoard's StatusBarServiceEventMgr appends its fixed More and Exit entries
after every provider-supplied statusBarGroupMenu.  For the hidden Linux tray
hosts this creates a second Exit action which terminates only the native proxy
process, rather than invoking the Linux application's SNI Quit action.

This builder changes only getMenu(): when the registered bundle starts with
the stable Linux tray-host prefix ``com.l410.linux.tray_``, it returns the
provider-supplied menu unchanged.  Every other native status-bar provider keeps
the original SceneBoard More/Exit behavior.  No IPC, public API, Linux
lifecycle rule, or application-specific menu item is added.
"""

from __future__ import annotations

from pathlib import Path
import argparse
import importlib.util
import json
import struct
import sys
import zipfile
import zlib


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from abc_record_index import (  # noqa: E402
    HEADER,
    parse_record_members,
    parse_record_prefix,
    read_string_item,
    read_uleb128,
)


HELPERS_PATH = ROOT / "linux-compat/scripts/build-anco-sp68-maximize-recover.py"
_spec = importlib.util.spec_from_file_location("r148_helpers", HELPERS_PATH)
if _spec is None or _spec.loader is None:
    raise RuntimeError(HELPERS_PATH)
helpers = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = helpers
_spec.loader.exec_module(helpers)


RECORD = "L&@ohos/smartdock/src/main/ets/default/manager/StatusBarServiceEventMgr&1.0.0;"
METHOD = "#~@0>#getMenu"
EXPECTED_SHAPE = (3, 3, 74, 0)
PREFIX = "com.l410.linux.tray_"
INSERTION_OFFSET = 0x1B


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


def find_target(data: bytes) -> tuple[dict, int, tuple[int, ...], int]:
    header = HEADER.unpack_from(data)
    method = None
    for index in range(header[6]):
        record_offset = struct.unpack_from("<I", data, header[7] + 4 * index)[0]
        record = parse_record_prefix(data, record_offset, len(data))
        if record["name"] != RECORD:
            continue
        _, methods = parse_record_members(data, record, len(data))
        matches = [item for item in methods if item["name"] == METHOD]
        if len(matches) != 1:
            raise ValueError(("target count", len(matches)))
        method = matches[0]
        break
    if method is None:
        raise ValueError("StatusBarServiceEventMgr.getMenu missing")
    for region_index in range(header[12]):
        region_header_offset = header[13] + region_index * 40
        region = struct.unpack_from("<10I", data, region_header_offset)
        if region[0] <= method["offset"] < region[1]:
            return method, region_index, region, region_header_offset
    raise ValueError("target index region missing")


def shape(method: dict) -> tuple[int, int, int, int]:
    return (
        method["num_vregs"], method["num_args"],
        method["code_size"], method["tries_size"],
    )


def region_strings(data: bytes, region: tuple[int, ...]) -> tuple[list[int], dict[str, int]]:
    ids = list(struct.unpack_from("<" + str(region[4]) + "I", data, region[5]))
    wanted = {"bundleName", "startsWith", "getExtendedMenu", "getMoreMenu", "getExitMenu"}
    found: dict[str, int] = {}
    for index, entity_offset in enumerate(ids):
        try:
            value = read_string_item(data, entity_offset, len(data))
        except (IndexError, UnicodeDecodeError, ValueError):
            continue
        if value in wanted and value not in found:
            found[value] = index
    if set(found) != wanted:
        raise ValueError(("missing target strings", found))
    return ids, found


def method_code_pointer_offset(data: bytes, method: dict) -> int:
    cursor = method["offset"] + 8
    _access_flags, cursor = read_uleb128(data, cursor, len(data))
    if data[cursor] != 1:
        raise ValueError(("method has no CODE tag", cursor, data[cursor]))
    return cursor + 1


def prefix_string_item() -> bytes:
    raw = PREFIX.encode("utf-8")
    if len(raw) != len(PREFIX):
        raise ValueError("tray prefix must remain ASCII")
    return uleb(len(PREFIX) * 2 + 1) + raw + b"\x00"


def build_guard(ids: dict[str, int], prefix_id: int) -> bytes:
    if max(ids["bundleName"], ids["startsWith"], prefix_id) > 0xFFFF:
        raise ValueError(("string id overflow", ids, prefix_id))
    # a2 is register 5 because getMenu has 3 vregs and 3 arguments.  v0 keeps
    # the already-built provider menu; v1/v2 are scratch registers here.
    code = bytearray()
    code.extend((0x60, 0x05))                         # lda a2
    code.extend((0x42, 0x15))                         # ldobjbyname slot 0x15
    code.extend(struct.pack("<H", ids["bundleName"]))
    code.extend((0x61, 0x01))                         # sta v1
    code.extend((0x3E,))                              # lda.str prefix
    code.extend(struct.pack("<H", prefix_id))
    code.extend((0x61, 0x02))                         # sta v2
    code.extend((0x60, 0x01))                         # lda v1
    code.extend((0x42, 0x17))                         # ldobjbyname slot 0x17
    code.extend(struct.pack("<H", ids["startsWith"]))
    code.extend((0x2E, 0x19, 0x01, 0x02))             # callthis1 v1, v2
    code.extend((0xFB, 0x14, 0x1B))                   # callruntime.isfalse
    code.extend((0x51, 0x05))                         # jnez original append
    code.extend((0x60, 0x00))                         # lda v0
    code.extend((0x64,))                              # return
    if len(code) != 31:
        raise ValueError(("guard size", len(code)))
    return bytes(code)


def method_inventory(data: bytes) -> list[tuple[str, str, int, tuple]]:
    header = HEADER.unpack_from(data)
    table = helpers.instruction_table()
    regions = [
        struct.unpack_from("<10I", data, header[13] + index * 40)
        for index in range(header[12])
    ]
    region_string_ids: dict[int, tuple[int, ...]] = {}
    result = []
    for record_index in range(header[6]):
        record_offset = struct.unpack_from("<I", data, header[7] + 4 * record_index)[0]
        record = parse_record_prefix(data, record_offset, len(data))
        _, methods = parse_record_members(data, record, len(data))
        occurrences: dict[str, int] = {}
        for method in methods:
            occurrence = occurrences.get(method["name"], 0)
            occurrences[method["name"]] = occurrence + 1
            code = data[method["insn_off"]:method["insn_off"] + method["code_size"]]
            region_index = next(
                index for index, region in enumerate(regions)
                if region[0] <= method["offset"] < region[1]
            )
            if region_index not in region_string_ids:
                region = regions[region_index]
                region_string_ids[region_index] = struct.unpack_from(
                    "<" + str(region[4]) + "I", data, region[5]
                )
            entity_ids = region_string_ids[region_index]
            semantic = []
            for instruction in helpers.decode(code, table):
                raw = instruction["raw"]
                encoding = instruction["format"]
                if "string_id" in instruction["properties"] or instruction["name"] == "lda.str":
                    width_text = encoding.rsplit("_id_", 1)[1]
                    width = int(width_text.split("_", 1)[0]) // 8
                    string_id = int.from_bytes(raw[-width:], "little")
                    string_value = read_string_item(data, entity_ids[string_id], len(data))
                    semantic.append((instruction["name"], raw[:-width], string_value))
                else:
                    semantic.append((instruction["name"], raw))
            result.append((record["name"], method["name"], occurrence, tuple(semantic)))
    return result


def build(source: Path, out: Path, compiler: Path) -> None:
    if out.exists():
        raise FileExistsError(out)
    out.mkdir(parents=True)
    with zipfile.ZipFile(source) as archive:
        module_names = [name for name in archive.namelist() if name.endswith("/modules.abc")]
        if module_names != ["ets/modules.abc"]:
            raise ValueError(module_names)
        original = archive.read(module_names[0])

    method, region_index, region, region_header_offset = find_target(original)
    if shape(method) != EXPECTED_SHAPE:
        raise ValueError(("source shape", shape(method), EXPECTED_SHAPE))
    code = original[method["insn_off"]:method["insn_off"] + method["code_size"]]
    decoded = helpers.decode(code, helpers.instruction_table())
    insertion = next(item for item in decoded if item["offset"] == INSERTION_OFFSET)
    if insertion["name"] != "lda" or insertion["raw"] != bytes.fromhex("6000"):
        raise ValueError(("unexpected append start", insertion))
    if any(item["name"] in {"jmp", "jeqz", "jnez"} for item in decoded):
        raise ValueError("source getMenu unexpectedly contains a branch")

    entity_ids, found = region_strings(original, region)
    if region[4] != 65533:
        raise ValueError(("unexpected target id count", region[4]))
    if any(
        read_string_item(original, entity_offset, len(original)) == PREFIX
        for entity_offset in entity_ids
    ):
        raise ValueError("source already contains Linux tray prefix")

    patched = bytearray(original)
    while len(patched) % 4:
        patched.append(0)
    prefix_offset = len(patched)
    patched.extend(prefix_string_item())
    while len(patched) % 4:
        patched.append(0)
    prefix_id = len(entity_ids)
    entity_ids.append(prefix_offset)
    new_index_offset = len(patched)
    patched.extend(struct.pack("<" + str(len(entity_ids)) + "I", *entity_ids))

    new_code = code[:INSERTION_OFFSET] + build_guard(found, prefix_id) + code[INSERTION_OFFSET:]
    helpers.decode(new_code, helpers.instruction_table())
    new_code_offset = len(patched)
    patched.extend(uleb(method["num_vregs"]))
    patched.extend(uleb(method["num_args"]))
    patched.extend(uleb(len(new_code)))
    patched.extend(uleb(0))
    patched.extend(new_code)

    struct.pack_into("<I", patched, region_header_offset + 16, len(entity_ids))
    struct.pack_into("<I", patched, region_header_offset + 20, new_index_offset)
    struct.pack_into("<I", patched, method_code_pointer_offset(original, method), new_code_offset)
    struct.pack_into("<I", patched, 16, len(patched))

    original_path = out / "modules.original.abc"
    patched_path = out / "modules.patched.input.abc"
    original_path.write_bytes(original)
    patched_path.write_bytes(patched)
    struct.pack_into("<I", patched, 8, zlib.adler32(patched[12:]) & 0xFFFFFFFF)
    patched_path.write_bytes(patched)
    baseline = original
    candidate_path = out / "modules.candidate.abc"
    candidate_path.write_bytes(patched)
    candidate = bytes(patched)
    baseline_method, _, _, _ = find_target(baseline)
    candidate_method, _, candidate_region, _ = find_target(candidate)
    expected_candidate_shape = (3, 3, 105, 0)
    if shape(baseline_method) != EXPECTED_SHAPE:
        raise ValueError(("baseline roundtrip shape", shape(baseline_method)))
    if shape(candidate_method) != expected_candidate_shape:
        raise ValueError(("candidate roundtrip shape", shape(candidate_method)))

    candidate_code = candidate[
        candidate_method["insn_off"]:candidate_method["insn_off"] + candidate_method["code_size"]
    ]
    candidate_decoded = helpers.decode(candidate_code, helpers.instruction_table())
    branch = next(item for item in candidate_decoded if item["offset"] == 0x35)
    if branch["name"] != "jnez" or branch.get("target") != 0x3A:
        raise ValueError(("candidate Linux-host branch", branch))
    candidate_ids, candidate_found = region_strings(candidate, candidate_region)
    if len(candidate_ids) != 65534:
        raise ValueError(("candidate id count", len(candidate_ids)))
    prefix_matches = [
        index for index, entity_offset in enumerate(candidate_ids)
        if read_string_item(candidate, entity_offset, len(candidate)) == PREFIX
    ]
    if prefix_matches != [prefix_id]:
        raise ValueError(("candidate prefix id", prefix_matches, prefix_id))

    before = method_inventory(baseline)
    after = method_inventory(candidate)
    before_keys = [(record, name, occurrence) for record, name, occurrence, _ in before]
    after_keys = [(record, name, occurrence) for record, name, occurrence, _ in after]
    if before_keys != after_keys:
        raise ValueError("method inventory changed")
    changed = [
        f"{record}:{name}:{occurrence}"
        for (record, name, occurrence, left), (_, _, _, right) in zip(before, after)
        if left != right
    ]
    expected_changed = [f"{RECORD}:{METHOD}:0"]
    if changed != expected_changed:
        raise ValueError(("changed methods", changed, expected_changed))

    baseline_pa = out / "modules.baseline.pa"
    candidate_pa = out / "modules.candidate.pa"
    helpers.disassemble(original_path, baseline_pa, out / "baseline-disasm.log")
    helpers.disassemble(candidate_path, candidate_pa, out / "candidate-disasm.log")

    package = out / "SceneBoard.candidate-r149.hap"
    with zipfile.ZipFile(source) as before_zip, zipfile.ZipFile(package, "w") as after_zip:
        for entry in before_zip.infolist():
            payload = candidate if entry.filename == module_names[0] else before_zip.read(entry.filename)
            after_zip.writestr(entry, payload)
    with zipfile.ZipFile(source) as before_zip, zipfile.ZipFile(package) as after_zip:
        unchanged = [name for name in before_zip.namelist() if name != module_names[0]]
        if before_zip.namelist() != after_zip.namelist():
            raise ValueError("HAP member order changed")
        if not all(before_zip.read(name) == after_zip.read(name) for name in unchanged):
            raise ValueError("unrelated HAP member changed")
        if after_zip.read(module_names[0]) != candidate:
            raise ValueError("candidate ABC packaging failed")

    report = {
        "source": str(source.resolve()),
        "source_bytes": source.stat().st_size,
        "candidate": str(package.resolve()),
        "candidate_bytes": package.stat().st_size,
        "target_record": RECORD,
        "target_method": METHOD,
        "source_shape": list(EXPECTED_SHAPE),
        "candidate_shape": list(expected_candidate_shape),
        "target_index_region": region_index,
        "old_region_id_count": region[4],
        "new_region_id_count": candidate_region[4],
        "linux_tray_bundle_prefix": PREFIX,
        "changed_methods": changed,
        "unchanged_other_methods": len(before) - 1,
        "unchanged_zip_members": len(unchanged),
        "all_other_hap_members_byte_equal": True,
        "new_interface": False,
        "route": "Linux tray hosts use their supplied SNI menu without SceneBoard fixed More/Exit",
        "device_staged": False,
        "device_loaded": False,
        "runtime_verified": False,
        "ui_verified": False,
    }
    (out / "verification.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (out / "README.md").write_text(
        "# SceneBoard r149 Linux tray menu scope\n\n"
        "Only hidden `com.l410.linux.tray_` tray hosts return the provider-supplied "
        "SNI menu unchanged. Native tray providers retain SceneBoard's original fixed "
        "More and Exit items.\n",
        encoding="utf-8",
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--compiler", type=Path,
        default=ROOT / "audit-alignment-v306/tile-sdk/ets-windows/ets/build-tools/ets-loader/bin/ark/build-win/bin/es2abc.exe",
    )
    arguments = parser.parse_args()
    build(arguments.source, arguments.output, arguments.compiler)
