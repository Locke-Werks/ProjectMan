# Single source of truth for the version. Everything else derives from it:
# project(), both VERSIONINFO resources, and the tag check in CI.
#
# installer.toml carries the same number and CI asserts the two agree, because
# Forge stamps the installer from the TOML and cannot see this file.
set(PM_VERSION_MAJOR 1)
set(PM_VERSION_MINOR 0)
set(PM_VERSION_PATCH 1)

set(PM_VERSION "${PM_VERSION_MAJOR}.${PM_VERSION_MINOR}.${PM_VERSION_PATCH}")
set(PM_VERSION_RC "${PM_VERSION_MAJOR},${PM_VERSION_MINOR},${PM_VERSION_PATCH},0")
set(PM_VERSION_RC_STR "${PM_VERSION}.0")

set(PM_PRODUCT   "ProjectMan")
set(PM_COMPANY   "Locke Werks")
set(PM_COPYRIGHT "Copyright (C) 2026 Locke Werks")
