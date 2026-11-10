/*
 * Embedded 512 KiB OpenBIOS image (tests/roms/bios/openbios.bin, MIT — see
 * tests/roms/bios/LICENSE.openbios). Linked into every target (3DS .cia/.3dsx/.cci,
 * ARM11 Linux, x86_64 host) so psx_load_bios(sys, "builtin") never needs romfs or SD files.
 *
 * The assembler include path (-Wa,-I<dir>) is set by CMakeLists.txt.
 */
    .section .rodata.psx_openbios_blob, "a"
    .balign 32
    .global psx_openbios_blob
    .type   psx_openbios_blob, %object
psx_openbios_blob:
    .incbin "openbios.bin"
psx_openbios_blob_end:
    .size   psx_openbios_blob, psx_openbios_blob_end - psx_openbios_blob

    .balign 4
    .global psx_openbios_blob_size
    .type   psx_openbios_blob_size, %object
psx_openbios_blob_size:
    .long   psx_openbios_blob_end - psx_openbios_blob
    .size   psx_openbios_blob_size, 4

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack, "", %progbits
#endif
