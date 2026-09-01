# A separate product that happens to live in this repository, so it carries its
# own icon and its own ProductName rather than ProjectMan's.
pm_configure_rc(PM_MCP_RC "pm-mcp" "pm-mcp.exe" "ProjectMan MCP server"
    "${CMAKE_SOURCE_DIR}/assets/projectman-mcp.ico"
    "ProjectMan MCP")

# Console subsystem, and it must stay that way: the whole transport is stdin and
# stdout. WIN32_EXECUTABLE is forced off because the GUI target in this same
# tree turns it on.
add_executable(pm-mcp
    main.cpp
    server.cpp
    json.cpp
    projects.cpp
    "${PM_MCP_RC}"
)

set_target_properties(pm-mcp PROPERTIES
    OUTPUT_NAME      "pm-mcp"
    WIN32_EXECUTABLE OFF
)

target_link_libraries(pm-mcp PRIVATE pm_core)

pm_apply_warnings(pm-mcp)
pm_apply_hardening(pm-mcp)
pm_embed_manifest(pm-mcp "${PM_MANIFEST}")

# Its own component. ProjectMan's installer stages the default component, so
# without this the MCP server would end up inside a product that is documented
# as not shipping it.
install(TARGETS pm-mcp RUNTIME DESTINATION . COMPONENT mcp EXCLUDE_FROM_ALL)
