# CI's version is authoritative even when a build directory is restored from cache.
set(TENCHU_VERSION "dev" CACHE STRING "Version of this build (release tag or development label)")
if(DEFINED ENV{TENCHU_VERSION} AND NOT "$ENV{TENCHU_VERSION}" STREQUAL "")
    set(TENCHU_VERSION "$ENV{TENCHU_VERSION}" CACHE STRING "Version of this build (release tag or development label)" FORCE)
endif()
if(NOT TENCHU_VERSION MATCHES "^[A-Za-z0-9][A-Za-z0-9.+_-]*$")
    message(FATAL_ERROR "Invalid TENCHU_VERSION: ${TENCHU_VERSION}")
endif()
