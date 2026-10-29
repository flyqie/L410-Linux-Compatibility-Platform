#!/usr/bin/env python3
"""Fast, read-only PANDA/ABC class-record index enumerator.

The parser intentionally reads only File::Header, the class index and the
fixed prefix of each ClassDataAccessor record.  It does not walk methods or
decode bytecode, so it is suitable for very large merged ABC files.

Layout reference:
  OpenHarmony arkcompiler_runtime_core/libpandafile/file.h
  OpenHarmony arkcompiler_runtime_core/libpandafile/class_data_accessor.cpp
"""

from __future__ import annotations

import argparse
import mmap
import pathlib
import shutil
import struct
import sys


HEADER = struct.Struct("<8sI4s11I")
MAGIC = b"PANDA\x00\x00\x00"


def read_uleb128(data: mmap.mmap, offset: int, limit: int) -> tuple[int, int]:
    value = 0
    shift = 0
    start = offset
    while offset < limit and shift < 35:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7
    raise ValueError(f"invalid ULEB128 at 0x{start:x}")


def parse_record_prefix(data: mmap.mmap, offset: int, file_size: int) -> dict[str, int | str]:
    utf16_len, cursor = read_uleb128(data, offset, file_size)
    nul = data.find(b"\x00", cursor, file_size)
    if nul < 0:
        raise ValueError(f"unterminated class descriptor at 0x{offset:x}")
    raw_name = data[cursor:nul]
    name = raw_name.decode("utf-8", errors="backslashreplace")
    cursor = nul + 1
    if cursor + 4 > file_size:
        raise ValueError(f"truncated class prefix at 0x{offset:x}")
    super_class_off = struct.unpack_from("<I", data, cursor)[0]
    cursor += 4
    access_flags, cursor = read_uleb128(data, cursor, file_size)
    num_fields, cursor = read_uleb128(data, cursor, file_size)
    num_methods, cursor = read_uleb128(data, cursor, file_size)
    return {
        "offset": offset,
        "name": name,
        "utf16_len": utf16_len,
        "super_class_off": super_class_off,
        "access_flags": access_flags,
        "num_fields": num_fields,
        "num_methods": num_methods,
        "prefix_end": cursor,
    }


def read_string_item(data: mmap.mmap, offset: int, file_size: int) -> str:
    _tagged_len, cursor = read_uleb128(data, offset, file_size)
    nul = data.find(b"\x00", cursor, file_size)
    if nul < 0:
        raise ValueError(f"unterminated string item at 0x{offset:x}")
    return data[cursor:nul].decode("utf-8", errors="backslashreplace")


def skip_class_tags(data: mmap.mmap, cursor: int, file_size: int) -> int:
    # ClassTag::INTERFACES precedes all optional fixed-size tagged values.
    if data[cursor] == 1:
        cursor += 1
        count, cursor = read_uleb128(data, cursor, file_size)
        cursor += count * 2
    if data[cursor] == 2:  # SOURCE_LANG: uint8_t
        cursor += 2
    for tag in (3, 4, 5, 6):
        while data[cursor] == tag:
            cursor += 5  # tag + EntityId
    if data[cursor] == 7:  # SOURCE_FILE
        cursor += 5
    if data[cursor] != 0:
        raise ValueError(f"unexpected ClassTag {data[cursor]} at 0x{cursor:x}")
    return cursor + 1


def skip_field(data: mmap.mmap, cursor: int, file_size: int) -> tuple[int, dict[str, int | str]]:
    start = cursor
    if cursor + 8 > file_size:
        raise ValueError(f"truncated field at 0x{start:x}")
    class_idx, type_idx, name_off = struct.unpack_from("<HHI", data, cursor)
    cursor += 8
    access_flags, cursor = read_uleb128(data, cursor, file_size)
    while data[cursor] != 0:
        tag = data[cursor]
        cursor += 1
        if tag == 1:  # INT_VALUE: SLEB128; byte length matches ULEB scan
            _value, cursor = read_uleb128(data, cursor, file_size)
        elif 2 <= tag <= 6:
            cursor += 4
        else:
            raise ValueError(f"unexpected FieldTag {tag} at 0x{cursor - 1:x}")
    cursor += 1
    return cursor, {
        "offset": start,
        "class_idx": class_idx,
        "type_idx": type_idx,
        "name_off": name_off,
        "name": read_string_item(data, name_off, file_size),
        "access_flags": access_flags,
    }


def skip_method(data: mmap.mmap, cursor: int, file_size: int) -> tuple[int, dict[str, int | str | None]]:
    start = cursor
    if cursor + 8 > file_size:
        raise ValueError(f"truncated method at 0x{start:x}")
    class_idx, proto_idx, name_off = struct.unpack_from("<HHI", data, cursor)
    cursor += 8
    access_flags, cursor = read_uleb128(data, cursor, file_size)
    code_off = None
    if data[cursor] == 1:  # CODE
        code_off = struct.unpack_from("<I", data, cursor + 1)[0]
        cursor += 5
    if data[cursor] == 2:  # SOURCE_LANG: uint8_t
        cursor += 2
    for tag in range(3, 10):
        while data[cursor] == tag:
            cursor += 5
    if data[cursor] != 0:
        raise ValueError(f"unexpected MethodTag {data[cursor]} at 0x{cursor:x}")
    cursor += 1
    result: dict[str, int | str | None] = {
        "offset": start,
        "class_idx": class_idx,
        "proto_idx": proto_idx,
        "name_off": name_off,
        "name": read_string_item(data, name_off, file_size),
        "access_flags": access_flags,
        "code_off": code_off,
    }
    if code_off is not None:
        code_cursor = code_off
        num_vregs, code_cursor = read_uleb128(data, code_cursor, file_size)
        num_args, code_cursor = read_uleb128(data, code_cursor, file_size)
        code_size, code_cursor = read_uleb128(data, code_cursor, file_size)
        tries_size, code_cursor = read_uleb128(data, code_cursor, file_size)
        result.update(
            num_vregs=num_vregs,
            num_args=num_args,
            code_size=code_size,
            tries_size=tries_size,
            insn_off=code_cursor,
        )
    return cursor, result


def parse_record_members(data: mmap.mmap, record: dict[str, int | str], file_size: int) -> tuple[list[dict], list[dict]]:
    cursor = skip_class_tags(data, int(record["prefix_end"]), file_size)
    fields = []
    for _ in range(int(record["num_fields"])):
        cursor, field = skip_field(data, cursor, file_size)
        fields.append(field)
    methods = []
    for _ in range(int(record["num_methods"])):
        cursor, method = skip_method(data, cursor, file_size)
        methods.append(method)
    return fields, methods


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("abc", type=pathlib.Path)
    ap.add_argument("patterns", nargs="*", help="case-insensitive substrings; omit to list all")
    ap.add_argument("--members", action="store_true", help="show fields and methods for matching records")
    ap.add_argument(
        "--make-single-record",
        type=pathlib.Path,
        metavar="OUTPUT_ABC",
        help="copy the ABC and expose only the single matching class in its header index",
    )
    args = ap.parse_args()

    with args.abc.open("rb") as fp, mmap.mmap(fp.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if len(data) < HEADER.size:
            raise ValueError("file is smaller than PANDA header")
        fields = HEADER.unpack_from(data, 0)
        magic, checksum, version = fields[:3]
        (
            file_size,
            foreign_off,
            foreign_size,
            num_classes,
            class_idx_off,
            num_lnps,
            lnp_idx_off,
            num_literalarrays,
            literalarray_idx_off,
            num_indexes,
            index_section_off,
        ) = fields[3:]
        if magic != MAGIC:
            raise ValueError(f"unexpected magic {magic!r}")
        if file_size > len(data) or class_idx_off + num_classes * 4 > file_size:
            raise ValueError("header offsets exceed file size")

        print(
            f"file={args.abc} bytes={len(data)} header_size={file_size} "
            f"version={'.'.join(map(str, version))} checksum=0x{checksum:08x} "
            f"classes={num_classes} class_idx_off=0x{class_idx_off:x} "
            f"literalarrays={num_literalarrays} indexes={num_indexes}"
        )
        needles = [p.casefold() for p in args.patterns]
        matches = 0
        matching_offsets: list[int] = []
        for index in range(num_classes):
            record_off = struct.unpack_from("<I", data, class_idx_off + index * 4)[0]
            record = parse_record_prefix(data, record_off, file_size)
            if needles and not all(p in str(record["name"]).casefold() for p in needles):
                continue
            matches += 1
            matching_offsets.append(record_off)
            print(
                f"[{index:6d}] off=0x{record_off:08x} methods={record['num_methods']:4d} "
                f"fields={record['num_fields']:3d} super=0x{record['super_class_off']:08x} "
                f"flags=0x{record['access_flags']:x} {record['name']}"
            )
            if args.members:
                member_fields, member_methods = parse_record_members(data, record, file_size)
                for field_index, field in enumerate(member_fields):
                    print(
                        f"  F[{field_index:02d}] off=0x{int(field['offset']):08x} "
                        f"flags=0x{int(field['access_flags']):x} {field['name']}"
                    )
                for method_index, method in enumerate(member_methods):
                    code = method["code_off"]
                    if code is None:
                        code_text = "external"
                    else:
                        code_text = (
                            f"code=0x{int(code):08x} insn=0x{int(method['insn_off']):08x} "
                            f"bytes={int(method['code_size']):6d} vregs={int(method['num_vregs']):3d} "
                            f"args={int(method['num_args']):2d} tries={int(method['tries_size']):2d}"
                        )
                    print(
                        f"  M[{method_index:02d}] off=0x{int(method['offset']):08x} "
                        f"flags=0x{int(method['access_flags']):x} {code_text} {method['name']}"
                    )
        print(f"matches={matches}")
    if args.make_single_record is not None:
        if len(matching_offsets) != 1:
            raise ValueError(f"single-record copy requires exactly one match, got {len(matching_offsets)}")
        shutil.copyfile(args.abc, args.make_single_record)
        with args.make_single_record.open("r+b") as out:
            # File::Header::num_classes is at 0x1c.  The first class-index
            # slot is replaced; the remaining bytes are preserved so all
            # referenced strings, literals and index sections stay valid.
            out.seek(0x1C)
            out.write(struct.pack("<I", 1))
            out.seek(class_idx_off)
            out.write(struct.pack("<I", matching_offsets[0]))
            out.flush()
        print(f"single_record_copy={args.make_single_record} record_off=0x{matching_offsets[0]:08x}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
