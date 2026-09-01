# Stage the Qt runtime beside ProjectMan.exe.
#
# Docked Console never needed this: it links the static CRT and imports nothing
# but system DLLs, so one file is the whole product. A Qt Widgets application
# needs its platform plugin and a handful of DLLs, and the installer payload has
# to contain them.
#
# pm.exe is deliberately absent from this: core is Qt-free, so the binary that
# goes on PATH stays a single standalone file.

function(pm_deploy_qt target)
    if(NOT WIN32)
        return()
    endif()

    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    if(NOT _qmake)
        message(WARNING "pm_deploy_qt: Qt6::qmake has no location; skipping ${target}")
        return()
    endif()
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)

    find_program(PM_WINDEPLOYQT windeployqt HINTS "${_qt_bin}")
    if(NOT PM_WINDEPLOYQT)
        message(WARNING "pm_deploy_qt: windeployqt not found; ${target} will only run "
                        "from a shell that already has Qt on PATH")
        return()
    endif()

    # --no-compiler-runtime: the VC redistributable is the installer's
    #   prerequisite to declare, not ours to scatter beside the exe.
    # --no-opengl-sw and --no-system-d3d-compiler drop about 20 MB of
    #   opengl32sw.dll and d3dcompiler_47.dll that a Widgets-only application
    #   never loads.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${PM_WINDEPLOYQT}"
                --no-translations
                --no-compiler-runtime
                --no-opengl-sw
                --no-system-d3d-compiler
                --no-quick-import
                "$<TARGET_FILE:${target}>"
        COMMENT "windeployqt ${target}"
        VERBATIM)

    # install(TARGETS) alone is not enough. windeployqt writes into the build
    # directory beside the exe, and CMake's install() only knows about the
    # target file itself, so the staged plugin tree has to be copied too.
    install(DIRECTORY "$<TARGET_FILE_DIR:${target}>/"
            DESTINATION .
            FILES_MATCHING
                PATTERN "*.dll"
                PATTERN "platforms/*"
                PATTERN "styles/*"
                PATTERN "imageformats/*"
                PATTERN "iconengines/*"
                PATTERN "generic/*"
                PATTERN "networkinformation/*"
                PATTERN "tls/*"
                PATTERN "*.pdb" EXCLUDE
                PATTERN "*.ilk" EXCLUDE
                PATTERN "*.exp" EXCLUDE
                PATTERN "*.lib" EXCLUDE
                # windeployqt stages the DirectX shader compiler for Qt's RHI
                # backends. A Widgets application paints through the raster
                # engine and never loads either, and together they are 16 MB of
                # a 42 MB payload. Verified by running the staged tree without
                # them.
                PATTERN "dxcompiler.dll" EXCLUDE
                PATTERN "dxil.dll" EXCLUDE)
endfunction()
