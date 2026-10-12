/**
 * tibia_spr_data.S — embeds Tibia.spr into the ELF binary
 * Assembled with the PS2 EE assembler, avoids objcopy ABI issues.
 *
 * The .spr file is placed in .rodata (read-only, loaded at boot).
 * The C code accesses it via _binary_Tibia_spr_start / _end symbols.
 */
    .section .rodata
    .global _binary_Tibia_spr_start
    .type _binary_Tibia_spr_start, @object
_binary_Tibia_spr_start:
    .incbin "Tibia.spr"
    .global _binary_Tibia_spr_end
    .type _binary_Tibia_spr_end, @object
_binary_Tibia_spr_end:

    .size _binary_Tibia_spr_start, _binary_Tibia_spr_end - _binary_Tibia_spr_start
