defold_log("sdk_ohos.cmake:")

# OHOS NDK toolchain is provided via the OHOS_NDK_PATH env var
# (set by the user via the extender launcher or local shell).
set(_OHOS_NDK_ROOT "$ENV{OHOS_NDK_PATH}")

if(NOT EXISTS "${_OHOS_NDK_ROOT}")
    message(FATAL_ERROR "sdk_ohos: OHOS_NDK_PATH is not set or does not exist: ${_OHOS_NDK_ROOT}")
endif()

set(_OHOS_TOOLCHAIN_FILE "${_OHOS_NDK_ROOT}/build/cmake/ohos.toolchain.cmake")
if(NOT EXISTS "${_OHOS_TOOLCHAIN_FILE}")
    message(FATAL_ERROR "sdk_ohos: ohos.toolchain.cmake not found at ${_OHOS_TOOLCHAIN_FILE}")
endif()

defold_log("sdk_ohos: Using OHOS NDK: ${_OHOS_NDK_ROOT}")
defold_log("sdk_ohos: Toolchain file: ${_OHOS_TOOLCHAIN_FILE}")

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${_OHOS_TOOLCHAIN_FILE}" CACHE FILEPATH "OHOS NDK toolchain file" FORCE)
endif()

# Tell the toolchain we target aarch64. OHOS_ARCH is consumed by
# ohos.toolchain.cmake.
set(OHOS_ARCH "arm64-v8a" CACHE STRING "OHOS ABI" FORCE)

defold_log("OHOS_ARCH: ${OHOS_ARCH}")

set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
include(${CMAKE_TOOLCHAIN_FILE})
