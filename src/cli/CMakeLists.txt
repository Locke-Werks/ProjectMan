pm_configure_rc(PM_CLI_RC "pm" "pm.exe" "ProjectMan console")

# Console subsystem: no WIN32 here, and WIN32_EXECUTABLE forced OFF because the
# GUI target in this same tree sets it ON. pm must own a real console to hand
# it to Claude Code.
add_executable(pm
    main.cpp
    console.cpp
    dispatch_console.cpp
    hook.cpp
    tui.cpp
    "${PM_CLI_RC}"
)

set_target_properties(pm PROPERTIES
    OUTPUT_NAME      "pm"
    WIN32_EXECUTABLE OFF
)

target_link_libraries(pm PRIVATE pm_core)

pm_apply_warnings(pm)
pm_apply_hardening(pm)
pm_embed_manifest(pm "${PM_MANIFEST}")

install(TARGETS pm RUNTIME DESTINATION .)
