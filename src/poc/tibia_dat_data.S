/**
 * tibia_dat_data.S — embeds Tibia.dat into the ELF binary
 */
    .section .rodata
    .global _binary_Tibia_dat_start
    .type _binary_Tibia_dat_start, @object
_binary_Tibia_dat_start:
    .incbin "Tibia.dat"
    .global _binary_Tibia_dat_end
    .type _binary_Tibia_dat_end, @object
_binary_Tibia_dat_end:

    .size _binary_Tibia_dat_start, _binary_Tibia_dat_end - _binary_Tibia_dat_start
