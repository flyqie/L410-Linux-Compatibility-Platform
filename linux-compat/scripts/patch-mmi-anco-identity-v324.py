"""Add only com.l410.linux.engine to the L410 MMI UDS write whitelist.

Offline generator for the exact device library. No install/restart/hash.
"""
from io import BytesIO
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile

BASE = Path(__file__).resolve().parents[1]
SOURCE = BASE / "build/host/libmmi-server-device.z.so"
OUTPUT = BASE / "build/host/libmmi-server-anco-identity-v324.z.so"
PROCESS_NAME = b"com.l410.linux.engine"
BRANCH_SITE = 0x1FC068
CAVE = 0x117070
COMPARE = 0x140734
ALLOW = 0x1FC06C
DENY = 0x1FC0B0
LITERAL = CAVE + 20


def file_offset(elf, va, length):
    for segment in elf.iter_segments():
        if (segment["p_type"] == "PT_LOAD" and
                segment["p_vaddr"] <= va and
                va + length <= segment["p_vaddr"] + segment["p_filesz"]):
            return segment["p_offset"] + va - segment["p_vaddr"]
    raise ValueError(f"address outside file-backed segment: {va:#x}")


def imm(delta, bits):
    if delta % 4 or not -(1 << (bits + 1)) <= delta < (1 << (bits + 1)):
        raise ValueError(f"unencodable ARM64 branch offset {delta}")
    return (delta // 4) & ((1 << bits) - 1)


def b_cond(address, target, condition):
    return 0x54000000 | (imm(target - address, 19) << 5) | condition


def branch(address, target, link=False):
    return (0x94000000 if link else 0x14000000) | imm(target - address, 26)


def cbnz_w0(address, target):
    return 0x35000000 | (imm(target - address, 19) << 5)


def adr_x0(address, target):
    delta = target - address
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError("ADR literal out of range")
    return 0x10000000 | ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5)


def main():
    original = SOURCE.read_bytes()
    elf = ELFFile(BytesIO(original))
    site = file_offset(elf, BRANCH_SITE, 4)
    cave = file_offset(elf, CAVE, 48)
    text_section = elf.get_section_by_name(".text")
    if not (text_section["sh_addr"] <= CAVE and
            CAVE + 48 <= text_section["sh_addr"] + text_section["sh_size"]):
        raise ValueError("code cave is not in executable .text")
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    old = list(decoder.disasm(original[site:site + 4], BRANCH_SITE))
    if len(old) != 1 or old[0].mnemonic != "b.eq" or old[0].op_str != f"#{DENY:#x}":
        raise ValueError("device whitelist branch is not the audited version")
    if original[cave:cave + 48] != bytes(48):
        raise ValueError("audited executable code cave is occupied")
    if len(PROCESS_NAME) != 21:
        raise ValueError("unexpected project process name length")

    # std::__h::string short representation: length*2, bytes, NUL, padding.
    literal = (bytes([len(PROCESS_NAME) * 2]) + PROCESS_NAME + b"\0").ljust(24, b"\0")
    instructions = (
        adr_x0(CAVE, LITERAL),
        0xAA1303E1,  # orr x1, xzr, x19
        branch(CAVE + 8, COMPARE, link=True),
        cbnz_w0(CAVE + 12, ALLOW),
        branch(CAVE + 16, DENY),
    )
    injected = struct.pack("<5I", *instructions) + literal
    candidate = bytearray(original)
    struct.pack_into("<I", candidate, site, b_cond(BRANCH_SITE, CAVE, 0))
    candidate[cave:cave + len(injected)] = injected

    site_decoded = list(decoder.disasm(candidate[site:site + 4], BRANCH_SITE))
    cave_decoded = list(decoder.disasm(candidate[cave:cave + 20], CAVE))
    expected = [
        ("adr", f"x0, #{LITERAL:#x}"),
        ("mov", "x1, x19"),
        ("bl", f"#{COMPARE:#x}"),
        ("cbnz", f"w0, #{ALLOW:#x}"),
        ("b", f"#{DENY:#x}"),
    ]
    if (len(site_decoded) != 1 or site_decoded[0].mnemonic != "b.eq" or
            site_decoded[0].op_str != f"#{CAVE:#x}" or
            [(item.mnemonic, item.op_str) for item in cave_decoded] != expected):
        raise ValueError("patched ARM64 control flow did not decode as expected")
    if candidate[cave + 20:cave + 44] != literal:
        raise ValueError("short-string literal mismatch")
    if (candidate[:cave] != original[:cave] or
            candidate[cave + len(injected):site] != original[cave + len(injected):site] or
            candidate[site + 4:] != original[site + 4:]):
        raise ValueError("candidate modifies bytes outside two audited regions")
    if len(candidate) != len(original):
        raise ValueError("candidate ELF size changed")
    OUTPUT.write_bytes(candidate)
    if OUTPUT.read_bytes() != candidate:
        raise ValueError("candidate direct-byte readback mismatch")
    print("MMI_IDENTITY_WHITELIST_CANDIDATE_PASS")
    print("source_bytes", len(original), "candidate_bytes", len(candidate))
    print("branch", hex(BRANCH_SITE), old[0].op_str, "->", site_decoded[0].op_str)
    print("cave", hex(CAVE), "patch_bytes", len(injected), "process", PROCESS_NAME.decode())
    for item in cave_decoded:
        print(hex(item.address), item.mnemonic, item.op_str)


if __name__ == "__main__":
    main()
