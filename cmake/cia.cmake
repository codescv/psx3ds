# =============================================================================
# Nintendo 3DS Custom SMDH Icon, Home Menu Banner (.bnr), Installable .cia,
# and Emulator-Bootable .cci Packaging Rules (PSX3DS v0.2.4)
#
# Include in CMakeLists.txt right after ctr_create_3dsx(psx3ds ...):
#   include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cia.cmake")
# =============================================================================

if(NINTENDO_3DS AND TARGET psx3ds)
    option(PSX3DS_BUILD_CIA "Build Nintendo 3DS .cia and .cci packages when makerom and bannertool are installed" ON)

    set(PSX3DS_CIA_VERSION_MAJOR 0)
    set(PSX3DS_CIA_VERSION_MINOR 2)
    set(PSX3DS_CIA_VERSION_PATCH 4)

    set(PSX3DS_ASSETS_3DS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/3ds")
    set(PSX3DS_ICON_PNG       "${PSX3DS_ASSETS_3DS_DIR}/icon.png")
    set(PSX3DS_BANNER_PNG     "${PSX3DS_ASSETS_3DS_DIR}/banner.png")
    set(PSX3DS_BANNER_WAV     "${PSX3DS_ASSETS_3DS_DIR}/banner.wav")
    set(PSX3DS_RSF_FILE       "${PSX3DS_ASSETS_3DS_DIR}/psx3ds.rsf")

    set(PSX3DS_SMDH_FILE      "${CMAKE_CURRENT_BINARY_DIR}/psx3ds.smdh")
    set(PSX3DS_CUSTOM_SMDH    "${CMAKE_CURRENT_BINARY_DIR}/psx3ds_custom.smdh")
    set(PSX3DS_ICON_STAMP     "${CMAKE_CURRENT_BINARY_DIR}/psx3ds_icon.stamp")
    set(PSX3DS_BNR_FILE       "${CMAKE_CURRENT_BINARY_DIR}/psx3ds.bnr")
    set(PSX3DS_CIA_FILE       "${CMAKE_CURRENT_BINARY_DIR}/psx3ds.cia")
    set(PSX3DS_CCI_FILE       "${CMAKE_CURRENT_BINARY_DIR}/psx3ds.cci")
    set(PSX3DS_3DSX_FILE      "${CMAKE_CURRENT_BINARY_DIR}/psx3ds.3dsx")

    # 1. Upgrade psx3ds.smdh (and psx3ds.3dsx embedded SMDH) from default_icon.png
    #    to our custom 48x48 PSX3DS icon cleanly without conflicting with CMakeLists.txt.
    if(CTR_SMDHTOOL_EXE AND CTR_3DSXTOOL_EXE AND EXISTS "${PSX3DS_ICON_PNG}")
        add_custom_command(
            OUTPUT "${PSX3DS_CUSTOM_SMDH}"
            COMMAND "${CTR_SMDHTOOL_EXE}" --create
                    "PSX3DS"
                    "PlayStation 1 Emulator for Nintendo 3DS (v0.2.4)"
                    "psx3ds"
                    "${PSX3DS_ICON_PNG}"
                    "${PSX3DS_CUSTOM_SMDH}"
            DEPENDS "${PSX3DS_ICON_PNG}"
            COMMENT "Generating custom PSX3DS SMDH icon metadata"
            VERBATIM
        )

        set(_psx3ds_icon_deps "${PSX3DS_CUSTOM_SMDH}" psx3ds)
        if(TARGET psx3ds_3dsx)
            list(APPEND _psx3ds_icon_deps psx3ds_3dsx "${PSX3DS_3DSX_FILE}")
        endif()

        add_custom_command(
            OUTPUT "${PSX3DS_ICON_STAMP}"
            COMMAND "${CMAKE_COMMAND}" -E copy "${PSX3DS_CUSTOM_SMDH}" "${PSX3DS_SMDH_FILE}"
            COMMAND "${CTR_3DSXTOOL_EXE}" "$<TARGET_FILE:psx3ds>" "${PSX3DS_3DSX_FILE}" "--smdh=${PSX3DS_SMDH_FILE}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${PSX3DS_ICON_STAMP}"
            DEPENDS ${_psx3ds_icon_deps}
            COMMENT "Embedding custom PSX3DS icon into psx3ds.smdh and psx3ds.3dsx"
            VERBATIM
        )

        add_custom_target(psx3ds_smdh_icon ALL DEPENDS "${PSX3DS_ICON_STAMP}")
    endif()

    # 2. Build 3DS Home Menu Banner (.bnr), Installable .cia (FBI / Luma3DS),
    #    and NCCH Cartridge Image .cci (Azahar / Citra direct boot with full RSF ExHeader)
    if(PSX3DS_BUILD_CIA)
        find_program(PSX3DS_BANNERTOOL_EXE
            NAMES bannertool
            HINTS "$ENV{DEVKITPRO}/tools/bin" "${DEVKITPRO}/tools/bin"
        )
        find_program(PSX3DS_MAKEROM_EXE
            NAMES makerom
            HINTS "$ENV{DEVKITPRO}/tools/bin" "${DEVKITPRO}/tools/bin"
        )

        if(PSX3DS_BANNERTOOL_EXE AND PSX3DS_MAKEROM_EXE)
            add_custom_command(
                OUTPUT "${PSX3DS_BNR_FILE}"
                COMMAND "${PSX3DS_BANNERTOOL_EXE}" makebanner
                        -i "${PSX3DS_BANNER_PNG}"
                        -a "${PSX3DS_BANNER_WAV}"
                        -o "${PSX3DS_BNR_FILE}"
                DEPENDS "${PSX3DS_BANNER_PNG}" "${PSX3DS_BANNER_WAV}"
                COMMENT "Building 3DS Home Menu banner psx3ds.bnr"
                VERBATIM
            )

            set(_psx3ds_cia_icon_file "${PSX3DS_SMDH_FILE}")
            set(_psx3ds_cia_smdh_dep  "${PSX3DS_SMDH_FILE}")
            if(TARGET psx3ds_smdh_icon)
                set(_psx3ds_cia_icon_file "${PSX3DS_CUSTOM_SMDH}")
                set(_psx3ds_cia_smdh_dep  "${PSX3DS_CUSTOM_SMDH}" "${PSX3DS_ICON_STAMP}")
            endif()

            add_custom_command(
                OUTPUT "${PSX3DS_CIA_FILE}"
                COMMAND "${PSX3DS_MAKEROM_EXE}"
                        -f cia
                        -o "${PSX3DS_CIA_FILE}"
                        -DAPP_ENCRYPTED=false
                        -rsf "${PSX3DS_RSF_FILE}"
                        -target t
                        -exefslogo
                        -elf "$<TARGET_FILE:psx3ds>"
                        -icon "${_psx3ds_cia_icon_file}"
                        -banner "${PSX3DS_BNR_FILE}"
                        -major "${PSX3DS_CIA_VERSION_MAJOR}"
                        -minor "${PSX3DS_CIA_VERSION_MINOR}"
                        -micro "${PSX3DS_CIA_VERSION_PATCH}"
                DEPENDS psx3ds "${PSX3DS_RSF_FILE}" "${PSX3DS_BNR_FILE}" ${_psx3ds_cia_smdh_dep}
                COMMENT "Packaging Nintendo 3DS installable archive psx3ds.cia (v0.2.4)"
                VERBATIM
            )

            add_custom_command(
                OUTPUT "${PSX3DS_CCI_FILE}"
                COMMAND "${PSX3DS_MAKEROM_EXE}"
                        -f cci
                        -o "${PSX3DS_CCI_FILE}"
                        -DAPP_ENCRYPTED=false
                        -rsf "${PSX3DS_RSF_FILE}"
                        -target t
                        -exefslogo
                        -elf "$<TARGET_FILE:psx3ds>"
                        -icon "${_psx3ds_cia_icon_file}"
                        -banner "${PSX3DS_BNR_FILE}"
                DEPENDS psx3ds "${PSX3DS_RSF_FILE}" "${PSX3DS_BNR_FILE}" ${_psx3ds_cia_smdh_dep}
                COMMENT "Packaging Nintendo 3DS cartridge image psx3ds.cci (v0.2.4)"
                VERBATIM
            )

            add_custom_target(psx3ds_cia ALL DEPENDS "${PSX3DS_CIA_FILE}" "${PSX3DS_CCI_FILE}")
            if(TARGET psx3ds_smdh_icon)
                add_dependencies(psx3ds_cia psx3ds_smdh_icon)
            endif()
        else()
            message(STATUS "psx3ds: bannertool or makerom not found in PATH/$DEVKITPRO/tools/bin; skipping .cia/.cci packaging")
        endif()
    endif()
endif()
