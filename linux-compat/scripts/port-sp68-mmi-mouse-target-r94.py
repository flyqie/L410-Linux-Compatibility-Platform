"""Port SP68 MouseTargetIsInAnco semantics to the retained 6.0 MMI.

SP68 obtains a fresh PointerItem from the real PointerEvent after
UpdatePointerEvent.  It evaluates AnCo ownership on BUTTON_DOWN, retains that
decision while the item is pressed, and reuses it for BUTTON_UP.  The retained
6.0 build has the same PointerEvent/PointerItem operations and the older
three-argument IsInAncoWindow ABI.  This offline generator replaces only the
old direct IsInAncoWindow call with an internal implementation of that exact
sequence.  It adds no IPC, callback, service, or public symbol.
"""

from io import BytesIO
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from keystone import Ks, KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN


BASE = Path(__file__).resolve().parents[1]
SOURCE = BASE / "build/host/libmmi-server-anco-physical-key-v333.z.so"
OUTPUT = BASE / "build/host/libmmi-server-anco-mouse-target-r94.z.so"
REPORT = BASE / "out/original-sp68-mmi-mouse-target-r94-20261011/verification.txt"

CALL_SITE = 0x224BC4
ORIGINAL_IS_IN_ANCO = 0x25563C
HELPER = 0x1170A0
MEMSET = 0x32A580
POINTER_ITEM_CTOR = 0x2F75B8
POINTER_ITEM_DTOR = 0x114138
GET_POINTER_ID = 0x2F4FA0
GET_POINTER_ITEM = 0x2FA8E0
GET_POINTER_ACTION = 0x2FA708
POINTER_ITEM_IS_PRESSED = 0x2F761C
POINTER_ITEM_SIZE = 216
FRAME_SIZE = 0x140
ITEM_OFFSET = 0x10
STATE_SIZE = 8


def file_offset(elf, address, length):
    for segment in elf.iter_segments():
        if (segment["p_type"] == "PT_LOAD" and
                segment["p_vaddr"] <= address and
                address + length <= segment["p_vaddr"] + segment["p_filesz"]):
            return segment["p_offset"] + address - segment["p_vaddr"]
    raise ValueError(f"address outside file-backed segment: {address:#x}")


def branch(address, target, link=False):
    delta = target - address
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError(f"unencodable ARM64 branch: {address:#x}->{target:#x}")
    return (0x94000000 if link else 0x14000000) | ((delta // 4) & 0x03FFFFFF)


def main():
    original = SOURCE.read_bytes()
    if len(original) != 3550456:
        raise ValueError("unexpected exact-device MMI size")
    elf = ELFFile(BytesIO(original))
    text = elf.get_section_by_name(".text")
    bss = elf.get_section_by_name(".bss")
    if text is None or bss is None or bss["sh_type"] != "SHT_NOBITS":
        raise ValueError("audited text/bss sections missing")

    loads = [(index, segment) for index, segment in enumerate(elf.iter_segments())
             if segment["p_type"] == "PT_LOAD"]
    load_index, final_load = loads[-1]
    if final_load["p_flags"] != 6:
        raise ValueError("final PT_LOAD is not RW")
    state = bss["sh_addr"] + bss["sh_size"]
    if state != final_load["p_vaddr"] + final_load["p_memsz"]:
        raise ValueError("final bss and RW PT_LOAD do not share the audited end")
    mapped_page_end = (state + 0xFFF) & ~0xFFF
    if state + STATE_SIZE > mapped_page_end:
        raise ValueError("RW extension crosses the audited page boundary")

    call_offset = file_offset(elf, CALL_SITE, 4)
    cave_offset = file_offset(elf, HELPER, 0x180)
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    old_call = list(decoder.disasm(original[call_offset:call_offset + 4], CALL_SITE))
    if (len(old_call) != 1 or old_call[0].mnemonic != "bl" or
            old_call[0].op_str != f"#{ORIGINAL_IS_IN_ANCO:#x}"):
        raise ValueError("6.0 UpdateMouseTarget call site differs")

    # Entry is the retained call ABI: x0=manager, x1=WindowInfo, w2/w3=coords.
    # UpdateMouseTarget keeps the real shared_ptr<PointerEvent>* in callee-saved
    # x21 at this exact audited call site.  The helper copies it to x24 before
    # any call and otherwise follows SP68 MouseTargetIsInAnco step for step.
    assembly = f"""
        sub sp, sp, #{FRAME_SIZE:#x}
        stp x29, x30, [sp, #0xf0]
        stp x19, x20, [sp, #0x100]
        stp x21, x22, [sp, #0x110]
        stp x23, x24, [sp, #0x120]
        stp x25, x26, [sp, #0x130]
        add x29, sp, #0xf0
        mov x19, x0
        mov x20, x1
        mov w22, w2
        mov w23, w3
        mov x24, x21

        add x0, sp, #{ITEM_OFFSET:#x}
        mov w1, wzr
        mov w2, #{POINTER_ITEM_SIZE}
        bl #{MEMSET:#x}
        add x0, sp, #{ITEM_OFFSET:#x}
        bl #{POINTER_ITEM_CTOR:#x}

        ldr x0, [x24]
        bl #{GET_POINTER_ID:#x}
        mov w1, w0
        ldr x0, [x24]
        add x2, sp, #{ITEM_OFFSET:#x}
        bl #{GET_POINTER_ITEM:#x}
        tbz w0, #0, return_false

        ldr x0, [x24]
        bl #{GET_POINTER_ACTION:#x}
        mov w21, w0
        cmp w21, #8
        b.ne check_pressed
        mov x0, x19
        mov x1, x20
        mov w2, w22
        mov w3, w23
        bl #{ORIGINAL_IS_IN_ANCO:#x}
        and w0, w0, #1
        adrp x8, {state & ~0xFFF:#x}
        strb w0, [x8, #{state & 0xFFF:#x}]

    check_pressed:
        add x0, sp, #{ITEM_OFFSET:#x}
        bl #{POINTER_ITEM_IS_PRESSED:#x}
        cbnz w0, use_cached
        cmp w21, #9
        b.eq use_cached
        mov x0, x19
        mov x1, x20
        mov w2, w22
        mov w3, w23
        bl #{ORIGINAL_IS_IN_ANCO:#x}
        and w19, w0, #1
        b destroy_item

    use_cached:
        adrp x8, {state & ~0xFFF:#x}
        ldrb w19, [x8, #{state & 0xFFF:#x}]
        b destroy_item

    return_false:
        mov w19, wzr

    destroy_item:
        add x0, sp, #{ITEM_OFFSET:#x}
        bl #{POINTER_ITEM_DTOR:#x}
        mov w0, w19
        ldp x25, x26, [sp, #0x130]
        ldp x23, x24, [sp, #0x120]
        ldp x21, x22, [sp, #0x110]
        ldp x19, x20, [sp, #0x100]
        ldp x29, x30, [sp, #0xf0]
        add sp, sp, #{FRAME_SIZE:#x}
        ret
    """
    assembler = Ks(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN)
    helper = bytes(assembler.asm(assembly, addr=HELPER)[0])
    if len(helper) > 0x180:
        raise ValueError("helper exceeds audited executable cave")
    if original[cave_offset:cave_offset + len(helper)] != bytes(len(helper)):
        raise ValueError("audited executable cave is occupied")

    candidate = bytearray(original)
    struct.pack_into("<I", candidate, call_offset, branch(CALL_SITE, HELPER, link=True))
    candidate[cave_offset:cave_offset + len(helper)] = helper

    phdr_memsz_offset = elf["e_phoff"] + load_index * elf["e_phentsize"] + 40
    old_memsz = struct.unpack_from("<Q", original, phdr_memsz_offset)[0]
    if old_memsz != final_load["p_memsz"]:
        raise ValueError("program-header p_memsz offset mismatch")
    struct.pack_into("<Q", candidate, phdr_memsz_offset, old_memsz + STATE_SIZE)

    bss_index = elf.get_section_index(".bss")
    shdr_size_offset = elf["e_shoff"] + bss_index * elf["e_shentsize"] + 32
    old_bss_size = struct.unpack_from("<Q", original, shdr_size_offset)[0]
    if old_bss_size != bss["sh_size"]:
        raise ValueError("section-header .bss size offset mismatch")
    struct.pack_into("<Q", candidate, shdr_size_offset, old_bss_size + STATE_SIZE)

    allowed = set(range(call_offset, call_offset + 4))
    allowed.update(range(cave_offset, cave_offset + len(helper)))
    allowed.update(range(phdr_memsz_offset, phdr_memsz_offset + 8))
    allowed.update(range(shdr_size_offset, shdr_size_offset + 8))
    differences = [index for index, (before, after) in
                   enumerate(zip(original, candidate)) if before != after]
    if any(index not in allowed for index in differences):
        raise ValueError("candidate changes bytes outside audited regions")
    if len(candidate) != len(original):
        raise ValueError("candidate file size changed")

    parsed = ELFFile(BytesIO(candidate))
    parsed_bss = parsed.get_section_by_name(".bss")
    parsed_loads = [segment for segment in parsed.iter_segments()
                    if segment["p_type"] == "PT_LOAD"]
    if parsed_bss["sh_size"] != old_bss_size + STATE_SIZE:
        raise ValueError("candidate .bss extension not visible")
    if parsed_loads[-1]["p_memsz"] != old_memsz + STATE_SIZE:
        raise ValueError("candidate RW PT_LOAD extension not visible")

    new_call = list(decoder.disasm(candidate[call_offset:call_offset + 4], CALL_SITE))
    decoded = list(decoder.disasm(helper, HELPER))
    if (len(new_call) != 1 or new_call[0].mnemonic != "bl" or
            new_call[0].op_str != f"#{HELPER:#x}"):
        raise ValueError("candidate call does not target helper")
    required_calls = {
        MEMSET, POINTER_ITEM_CTOR, POINTER_ITEM_DTOR, GET_POINTER_ID,
        GET_POINTER_ITEM, GET_POINTER_ACTION, POINTER_ITEM_IS_PRESSED,
        ORIGINAL_IS_IN_ANCO,
    }
    decoded_calls = {int(item.op_str[1:], 16) for item in decoded
                     if item.mnemonic == "bl" and item.op_str.startswith("#0x")}
    if not required_calls.issubset(decoded_calls):
        raise ValueError("candidate omits an audited SP68 semantic operation")
    required_ops = {("cmp", "w21, #8"), ("cmp", "w21, #9")}
    actual_ops = {(item.mnemonic, item.op_str) for item in decoded}
    if not required_ops.issubset(actual_ops) or decoded[-1].mnemonic != "ret":
        raise ValueError("candidate helper does not decode as the SP68 sequence")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(candidate)
    if OUTPUT.read_bytes() != candidate:
        raise ValueError("candidate direct-byte readback mismatch")

    REPORT.parent.mkdir(parents=True, exist_ok=True)
    report = [
        "SP68_MMI_MOUSE_TARGET_R94_STATIC_PASS",
        f"source={SOURCE}",
        f"output={OUTPUT}",
        f"file_bytes={len(candidate)}",
        f"call_site={CALL_SITE:#x} old={old_call[0].op_str} new={new_call[0].op_str}",
        f"helper={HELPER:#x} helper_bytes={len(helper)}",
        f"pointer_item_bytes={POINTER_ITEM_SIZE}",
        f"state={state:#x} bss={old_bss_size:#x}->{old_bss_size + STATE_SIZE:#x}",
        f"rw_memsz={old_memsz:#x}->{old_memsz + STATE_SIZE:#x}",
        f"changed_bytes={len(differences)}",
        "sp68_semantics=fresh_pointer_item,button_down_cache,pressed_cache,button_up_cache,recompute",
        "public_interfaces_added=0",
        "runtime_verified=0",
    ]
    report.extend(f"{item.address:#x} {item.mnemonic} {item.op_str}" for item in decoded)
    REPORT.write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report[:13]))


if __name__ == "__main__":
    main()
