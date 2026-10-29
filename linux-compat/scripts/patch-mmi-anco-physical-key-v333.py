"""Prepare an exact-device AnCo physical-key route using stock SimulateKeyExt.

The device MMI skips ordinary, unflagged keys after recognizing an AnCo
focused window. Replace only that branch with NOP, so both flagged and
unflagged AnCo keys execute the complete original device/local/0x100 checks,
including shared_ptr setup, before stock SimulateKeyExt. This is an offline
candidate; this script does not install, bind, or restart anything.
"""

from io import BytesIO
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile


BASE = Path(__file__).resolve().parents[1]
SOURCE = BASE / "build/host/libmmi-server-anco-identity-v324.z.so"
OUTPUT = BASE / "build/host/libmmi-server-anco-physical-key-v333.z.so"
SITE = 0x239640
OLD_TARGET = 0x239820
NEXT = 0x239644
DISPATCH = 0x23976C


def file_offset(elf, address, length):
    for segment in elf.iter_segments():
        if (segment["p_type"] == "PT_LOAD" and
                segment["p_vaddr"] <= address and
                address + length <= segment["p_vaddr"] + segment["p_filesz"]):
            return segment["p_offset"] + address - segment["p_vaddr"]
    raise ValueError(f"address outside file-backed segment: {address:#x}")


def main():
    original = SOURCE.read_bytes()
    elf = ELFFile(BytesIO(original))
    offset = file_offset(elf, SITE, 4)
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    old = list(decoder.disasm(original[offset:offset + 4], SITE))
    if len(old) != 1 or old[0].mnemonic != "tbz" or old[0].op_str != f"w0, #0, #{OLD_TARGET:#x}":
        raise ValueError("exact-device unflagged AnCo key branch differs")
    next_offset = file_offset(elf, NEXT, 4)
    next_instruction = list(decoder.disasm(original[next_offset:next_offset + 4], NEXT))
    if (len(next_instruction) != 1 or next_instruction[0].mnemonic != "add" or
            next_instruction[0].op_str != "x8, sp, #0x38"):
        raise ValueError("stock key context setup differs")
    dispatch_offset = file_offset(elf, DISPATCH, 4)
    dispatch = list(decoder.disasm(original[dispatch_offset:dispatch_offset + 4], DISPATCH))
    if (len(dispatch) != 1 or dispatch[0].mnemonic != "bl" or
            "SimulateKeyExt" not in dispatch[0].op_str and dispatch[0].op_str != "#0x255900"):
        raise ValueError("stock SimulateKeyExt call differs")
    candidate = bytearray(original)
    struct.pack_into("<I", candidate, offset, 0xD503201F)
    updated = list(decoder.disasm(candidate[offset:offset + 4], SITE))
    if len(updated) != 1 or updated[0].mnemonic != "nop":
        raise ValueError("AnCo key gate does not decode as NOP")
    if (candidate[:offset] != original[:offset] or
            candidate[offset + 4:] != original[offset + 4:] or
            len(candidate) != len(original)):
        raise ValueError("candidate differs outside one audited instruction")
    OUTPUT.write_bytes(candidate)
    if OUTPUT.read_bytes() != candidate:
        raise ValueError("candidate direct-byte readback mismatch")
    print("MMI_STOCK_PHYSICAL_KEY_CANDIDATE_PASS", len(candidate))
    print(f"branch {SITE:#x}: {OLD_TARGET:#x} -> fallthrough {NEXT:#x}")
    print(f"stock_dispatch {DISPATCH:#x}: {dispatch[0].op_str}")


if __name__ == "__main__":
    main()
