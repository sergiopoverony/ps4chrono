#!/usr/bin/env python3
# Turns ps4chrono.elf into ps4chrono.bin: a flat payload for loaders that take
# raw .bin files (copy to memory, jump to byte 0).
#
# Layout of the .bin:
#   0x0000  starter: fixes the payload's own addresses, then jumps to _start
#   0x0040  table for the starter: count, then (offset, addend) pairs
#   STUB    ELF memory image from address 0: code, data, and .bss as zeros
#           (STUB = 16 KB, or more if the table is big; keeps the image aligned)
#
# Why the starter is needed: an ELF loader fixes R_X86_64_RELATIVE addresses
# (pointers inside the payload) before running it. A raw .bin loader doesn't,
# and the SDK startup code reads its own start address from one of them.
# The starter fixes them; everything else (system functions, .bss) is done
# by the SDK startup code itself, same as with the .elf.
#
# Created by SergioPoverony and Mr.Claude. GPLv3.

import struct
import sys

PAGE = 0x4000          # largest segment alignment of the payload
TABLE_OFF = 0x40       # where the relocation table starts inside the starter

PT_LOAD, PT_DYNAMIC = 1, 2
DT_NULL, DT_RELA, DT_RELASZ, DT_RELAENT = 0, 7, 8, 9
R_X86_64_RELATIVE = 8


def fail(msg):
    sys.stderr.write("mkbin: " + msg + "\n")
    sys.exit(1)


def starter_code(stub_size, entry):
    """x86-64 machine code, position independent, uses only scratch registers
    (rax rcx rdx r8 r11), so rdi/rsi from the loader reach _start unchanged."""
    code = bytearray()
    # 0:  lea rax, [rip + stub_size-7]        ; rax = image base
    code += b"\x48\x8d\x05" + struct.pack("<i", stub_size - 7)
    # 7:  lea rcx, [rip + TABLE_OFF-14]       ; rcx = table
    code += b"\x48\x8d\x0d" + struct.pack("<i", TABLE_OFF - 14)
    # 14: mov rdx, [rcx]                      ; rdx = count
    code += b"\x48\x8b\x11"
    # 17: add rcx, 8
    code += b"\x48\x83\xc1\x08"
    # 21: loop: test rdx, rdx
    code += b"\x48\x85\xd2"
    # 24: jz done (49)
    code += b"\x74" + struct.pack("<b", 49 - 26)
    # 26: mov r8, [rcx]                       ; offset
    code += b"\x4c\x8b\x01"
    # 29: mov r11, [rcx+8]                    ; addend
    code += b"\x4c\x8b\x59\x08"
    # 33: add r11, rax                        ; base + addend
    code += b"\x49\x01\xc3"
    # 36: mov [rax+r8], r11
    code += b"\x4e\x89\x1c\x00"
    # 40: add rcx, 16
    code += b"\x48\x83\xc1\x10"
    # 44: dec rdx
    code += b"\x48\xff\xca"
    # 47: jmp loop (21)
    code += b"\xeb" + struct.pack("<b", 21 - 49)
    # 49: done: jmp image_base + entry
    code += b"\xe9" + struct.pack("<i", stub_size + entry - 54)
    assert len(code) == 54 <= TABLE_OFF
    return code


def main():
    if len(sys.argv) != 3:
        fail("usage: mkbin.py input.elf output.bin")
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()

    if data[:4] != b"\x7fELF":
        fail("%s is not an ELF file" % src)
    if data[4] != 2 or data[5] != 1:
        fail("not a 64-bit little-endian ELF")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_machine != 62:
        fail("not x86_64")
    if e_type != 3:
        fail("not a position-independent payload (ET_DYN expected)")

    e_entry, e_phoff = struct.unpack_from("<QQ", data, 24)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)

    loads, dyn_vaddr = [], None
    for i in range(e_phnum):
        p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<IIQQQQQQ", data, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD:
            loads.append((p_vaddr, p_offset, p_filesz, p_memsz, p_align))
        elif p_type == PT_DYNAMIC:
            dyn_vaddr = p_vaddr
    if not loads:
        fail("no loadable segments")
    if min(l[0] for l in loads) != 0:
        fail("first segment is not at address 0")
    if max(l[4] for l in loads) > PAGE:
        fail("segment alignment is larger than 0x%x" % PAGE)

    # memory image, .bss included as zeros
    end = max(v + m for v, _o, _f, m, _a in loads)
    end = (end + 0xFFF) & ~0xFFF
    image = bytearray(end)
    for v, o, f, _m, _a in loads:
        image[v:v + f] = data[o:o + f]
    if e_entry >= end:
        fail("entry point 0x%x is outside the image" % e_entry)

    # R_X86_64_RELATIVE relocations from the dynamic section
    relocs = []
    if dyn_vaddr is not None:
        rela = relasz = None
        relaent = 24
        pos = dyn_vaddr
        while True:
            tag, val = struct.unpack_from("<qQ", image, pos)
            if tag == DT_NULL:
                break
            if tag == DT_RELA:
                rela = val
            elif tag == DT_RELASZ:
                relasz = val
            elif tag == DT_RELAENT:
                relaent = val
            pos += 16
        if rela is not None and relasz:
            for k in range(relasz // relaent):
                r_offset, r_info, r_addend = struct.unpack_from("<QQq", image, rela + k * relaent)
                if r_info & 0xffffffff == R_X86_64_RELATIVE:
                    if r_offset + 8 > end:
                        fail("relocation at 0x%x is outside the image" % r_offset)
                    relocs.append((r_offset, r_addend))

    table = struct.pack("<Q", len(relocs))
    for off, add in relocs:
        table += struct.pack("<Qq", off, add)
    stub_size = TABLE_OFF + len(table)
    stub_size = (stub_size + PAGE - 1) // PAGE * PAGE

    stub = bytearray(stub_size)
    code = starter_code(stub_size, e_entry)
    stub[0:len(code)] = code
    stub[TABLE_OFF:TABLE_OFF + len(table)] = table

    with open(dst, "wb") as out:
        out.write(stub)
        out.write(image)

    print("mkbin: %s -> %s (%d bytes, %d addresses fixed by the starter)"
          % (src, dst, stub_size + len(image), len(relocs)))


if __name__ == "__main__":
    main()
