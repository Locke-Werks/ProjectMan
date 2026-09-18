pm_configure_rc(PM_GUI_RC "ProjectMan" "ProjectMan.exe" "ProjectMan")

# WIN32 is the subsystem, not the platform: it makes this a GUI binary so
# launching it never opens a console of its own.
qt_add_executable(ProjectMan WIN32
    main.cpp
    theme_qt.cpp
    project_model.cpp
    widgets.cpp
    dispatch_run_window.cpp
    agent_board.cpp
    agent_board_panel.cpp
    main_window.cpp
    resources.qrc
    "${PM_GUI_RC}"
)

set_target_properties(ProjectMan PROPERTIES OUTPUT_NAME "ProjectMan")

target_link_libraries(ProjectMan PRIVATE
    pm_core
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)

pm_apply_warnings(ProjectMan)
pm_apply_hardening(ProjectMan)
pm_embed_manifest(ProjectMan "${PM_MANIFEST}")
pm_deploy_qt(ProjectMan)

install(TARGETS ProjectMan RUNTIME DESTINATION .)
