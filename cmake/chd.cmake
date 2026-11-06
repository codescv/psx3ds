# =============================================================================
# cmake/chd.cmake — CHD v1–v5 Compressed Disc Support (vendored libchdr)
# =============================================================================
# Supports all 4 CHD v5 CD codecs:
#   - cdzl (Deflate / zlib)
#   - cdlz (LZMA)
#   - cdzs (Zstandard)
#   - cdfl (FLAC CD-DA audio + Deflate subcode)
# plus legacy v1–v4 codecs (zlib, lzma, huff, flac, zstd, avhuff) & raw hunks.
#
# Tier linkage strategy:
#   - T1 (Nintendo 3DS): links prebuilt ARMv6K portlibs from
#     $DEVKITPRO/portlibs/3ds/lib (libz.a, libzstd.a, liblzma.a, libFLAC.a) when
#     installed, falling back automatically to vendored pure-C codecs on minimal
#     CI containers that omit optional portlibs.
#   - T2 (arm11-linux static QEMU) & T3 (x86_64 Host ASan+UBSan): 100% standalone
#     using vendored pure-C miniz + 7z LzmaDec + zstddeclib + dr_flac with zero
#     host -dev library dependencies.
# =============================================================================

set(PSX3DS_CHDR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchdr")

set(PSX3DS_CHDR_SRCS
    "${PSX3DS_CHDR_DIR}/src/libchdr_bitstream.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_cdrom.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_chd.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_avhuff.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_cdfl.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_cdlz.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_cdzl.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_cdzs.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_flac.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_huff.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_lzma.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_zlib.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_codec_zstd.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_flac.c"
    "${PSX3DS_CHDR_DIR}/src/libchdr_huffman.c"
    "${PSX3DS_CHDR_DIR}/deps/lzma-26.02/src/LzmaDec.c"
)

set(PSX3DS_CHDR_DEFS
    WANT_RAW_DATA_SECTOR=1
    WANT_SUBCODE=1
    VERIFY_BLOCK_CRC=1
    LOWRAM_TARGET=0
)

set(PSX3DS_CHD_EXT_LIBS "")
set(PSX3DS_CHD_USE_SYS_ZLIB FALSE)
set(PSX3DS_CHD_USE_SYS_ZSTD FALSE)
set(PSX3DS_CHD_3DS_INC "")

if(PSX3DS_IS_3DS)
    set(_psx3ds_dkp "${DEVKITPRO}")
    if(NOT _psx3ds_dkp AND DEFINED ENV{DEVKITPRO})
        set(_psx3ds_dkp "$ENV{DEVKITPRO}")
    endif()

    if(_psx3ds_dkp AND EXISTS "${_psx3ds_dkp}/portlibs/3ds/lib")
        set(_psx3ds_3ds_portlib_inc "${_psx3ds_dkp}/portlibs/3ds/include")
        set(_psx3ds_3ds_portlib_lib "${_psx3ds_dkp}/portlibs/3ds/lib")

        find_library(PSX3DS_3DS_ZLIB_LIB   NAMES z    PATHS "${_psx3ds_3ds_portlib_lib}" NO_DEFAULT_PATH)
        find_library(PSX3DS_3DS_ZSTD_LIB   NAMES zstd PATHS "${_psx3ds_3ds_portlib_lib}" NO_DEFAULT_PATH)
        find_library(PSX3DS_3DS_LZMA_LIB   NAMES lzma PATHS "${_psx3ds_3ds_portlib_lib}" NO_DEFAULT_PATH)
        find_library(PSX3DS_3DS_FLAC_LIB   NAMES FLAC PATHS "${_psx3ds_3ds_portlib_lib}" NO_DEFAULT_PATH)
        find_library(PSX3DS_3DS_OGG_LIB    NAMES ogg  PATHS "${_psx3ds_3ds_portlib_lib}" NO_DEFAULT_PATH)

        if(PSX3DS_3DS_ZLIB_LIB AND EXISTS "${_psx3ds_3ds_portlib_inc}/zlib.h")
            set(PSX3DS_CHD_USE_SYS_ZLIB TRUE)
            list(APPEND PSX3DS_CHD_EXT_LIBS "${PSX3DS_3DS_ZLIB_LIB}")
        endif()
        if(PSX3DS_3DS_ZSTD_LIB AND EXISTS "${_psx3ds_3ds_portlib_inc}/zstd.h")
            set(PSX3DS_CHD_USE_SYS_ZSTD TRUE)
            list(APPEND PSX3DS_CHD_EXT_LIBS "${PSX3DS_3DS_ZSTD_LIB}")
        endif()
        if(PSX3DS_3DS_LZMA_LIB)
            list(APPEND PSX3DS_CHD_EXT_LIBS "${PSX3DS_3DS_LZMA_LIB}")
        endif()
        if(PSX3DS_3DS_FLAC_LIB)
            list(APPEND PSX3DS_CHD_EXT_LIBS "${PSX3DS_3DS_FLAC_LIB}")
            if(PSX3DS_3DS_OGG_LIB)
                list(APPEND PSX3DS_CHD_EXT_LIBS "${PSX3DS_3DS_OGG_LIB}")
            endif()
        endif()
        if(EXISTS "${_psx3ds_3ds_portlib_inc}")
            set(PSX3DS_CHD_3DS_INC "${_psx3ds_3ds_portlib_inc}")
        endif()
    endif()
endif()

if(PSX3DS_CHD_USE_SYS_ZLIB)
    list(APPEND PSX3DS_CHDR_DEFS CHDR_SYSTEM_ZLIB)
else()
    list(APPEND PSX3DS_CHDR_SRCS "${PSX3DS_CHDR_DIR}/deps/miniz-3.1.2/miniz.c")
    list(APPEND PSX3DS_CHDR_DEFS
        MINIZ_NO_ARCHIVE_APIS
        MINIZ_NO_DEFLATE_APIS
        MINIZ_NO_STDIO
        MINIZ_NO_TIME
    )
endif()

if(PSX3DS_CHD_USE_SYS_ZSTD)
    list(APPEND PSX3DS_CHDR_DEFS CHDR_SYSTEM_ZSTD)
else()
    list(APPEND PSX3DS_CHDR_SRCS "${PSX3DS_CHDR_DIR}/deps/zstd-1.5.7/zstddeclib.c")
    list(APPEND PSX3DS_CHDR_DEFS
        MEM_FORCE_MEMORY_ACCESS=0
        ZSTD_DISABLE_ASM=1
    )
endif()

add_library(psx3ds_chdr STATIC ${PSX3DS_CHDR_SRCS})
target_include_directories(psx3ds_chdr SYSTEM PUBLIC
    "${PSX3DS_CHDR_DIR}/include"
)
if(PSX3DS_CHD_3DS_INC)
    target_include_directories(psx3ds_chdr SYSTEM PRIVATE "${PSX3DS_CHD_3DS_INC}")
endif()
target_compile_definitions(psx3ds_chdr PRIVATE ${PSX3DS_CHDR_DEFS})
target_compile_options(psx3ds_chdr PRIVATE
    -fno-strict-aliasing
    -fno-common
    -Wall
    -Werror=implicit-function-declaration
    -Werror=return-type
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    ${PSX3DS_TARGET_C_FLAGS}
)

# Expose libchdr headers cleanly to cdrom_chd.c inside psx3ds_core_objs
target_include_directories(psx3ds_core_objs SYSTEM PRIVATE
    "${PSX3DS_CHDR_DIR}/include"
)

# Prepend psx3ds_chdr (+ T1 3DS portlibs when active) before system/libm libs
list(INSERT PSX3DS_TARGET_LIBS 0 psx3ds_chdr ${PSX3DS_CHD_EXT_LIBS})
