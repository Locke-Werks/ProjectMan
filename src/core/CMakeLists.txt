# Deliberately Qt-free, matching Scribble's core.
#
# The payoff is pm.exe: it links this and nothing else, so the binary that goes
# on PATH is standalone rather than needing Qt6Core.dll beside it. It also
# removes the question of how to pump a QProcess or a QFutureWatcher without an
# event loop, since the console front end has none.
add_library(pm_core STATIC
    strutil.cpp
    proc.cpp
    json.cpp
    git.cpp
    discovery.cpp
    scanner.cpp
    config.cpp
    enrich.cpp
    dock.cpp
    workitems.cpp
    launcher.cpp
    claude_stream.cpp
    dispatch_run.cpp
)

target_include_directories(pm_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(pm_core PUBLIC
    PM_VERSION_STRING="${PM_VERSION}"
    PM_PRODUCT_NAME="${PM_PRODUCT}"
)

target_link_libraries(pm_core
    PUBLIC
        tomlplusplus::tomlplusplus
    PRIVATE
        shell32     # SHGetKnownFolderPath
        ole32       # CoTaskMemFree, for the above
)

pm_apply_warnings(pm_core)
pm_apply_hardening(pm_core)
