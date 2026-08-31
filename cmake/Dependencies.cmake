include(FetchContent)

# ----------------------------------------------------------------- toml++ ---
# The house TOML parser: Forge and Scribble both use this exact tag. Header-only
# and the only third-party dependency in the tree, which keeps pm.exe a single
# standalone binary.
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)
