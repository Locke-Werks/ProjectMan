# The house warning set, matching Forge's cmake/LwiWarnings.cmake and
# dockedconsole's cmake/DockWarnings.cmake.
#
# /WX is off by default and on in CI via -DPM_WERROR=ON. CI is where a new
# warning should stop the line, not a developer's machine.

function(pm_apply_warnings target)
    if(NOT MSVC)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /Zc:throwingNew
        /utf-8
        /EHsc
        $<$<BOOL:${PM_WERROR}>:/WX>
    )

    target_compile_definitions(${target} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endfunction()
