# Exploit mitigations, following Forge's cmake/LwiHardening.cmake.
#
# /Qspectre is conditional. It needs the "MSVC v143 - VS 2022 C++ x64/x86
# Spectre-mitigated libs" component, which is NOT installed by default in Visual
# Studio Community. Requiring it unconditionally makes a fresh clone fail to
# configure for a reason that reads like a code error, so it is detected and
# reported instead.
#
# /DEPENDENTLOADFLAG is deliberately absent here, unlike dockedconsole. That
# binary is fully static and imports nothing but system DLLs. ProjectMan.exe
# loads the Qt DLLs from its own install directory, and restricting the implicit
# import search to System32 would stop it starting at all.

set(PM_SPECTRE_LIBS_FOUND FALSE)
if(MSVC AND DEFINED CMAKE_CXX_COMPILER)
    get_filename_component(_pm_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64 -> .../VC/Tools/MSVC/<ver>
    get_filename_component(_pm_msvc_root "${_pm_msvc_bin}/../../.." ABSOLUTE)
    if(EXISTS "${_pm_msvc_root}/lib/spectre/x64")
        set(PM_SPECTRE_LIBS_FOUND TRUE)
    endif()
endif()

if(PM_HARDENING AND NOT PM_SPECTRE_LIBS_FOUND)
    message(STATUS
        "projectman: Spectre-mitigated libs not found, building without /Qspectre.\n"
        "            To enable: Visual Studio Installer > Modify > Individual components >\n"
        "            \"MSVC v143 - VS 2022 C++ x64/x86 Spectre-mitigated libs (Latest)\"")
endif()

function(pm_apply_hardening target)
    if(NOT MSVC OR NOT PM_HARDENING)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        /GS                       # stack buffer overrun detection
        /guard:cf                 # Control Flow Guard
        /sdl                      # additional security checks
        /Gy /Gw                   # function and data COMDATs, lets /OPT:REF work
        /guard:ehcont
        $<$<BOOL:${PM_SPECTRE_LIBS_FOUND}>:/Qspectre>
    )

    get_target_property(_pm_type ${target} TYPE)
    if(_pm_type STREQUAL "STATIC_LIBRARY")
        return()
    endif()

    target_link_options(${target} PRIVATE
        /GUARD:CF
        /GUARD:EHCONT
        /DYNAMICBASE              # ASLR
        /HIGHENTROPYVA            # 64-bit ASLR entropy
        /NXCOMPAT                 # DEP
        /CETCOMPAT                # shadow stack
        /OPT:REF /OPT:ICF
        /INCREMENTAL:NO
    )
endfunction()
