cmake_minimum_required(VERSION 3.18)
project(CsoundNativeInput)

set(CMAKE_CXX_STANDARD 17)
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ============================================================
# Output destination (the Unity CsoundUnity package).
# Provide via -DCSOUNDUNITY_PACKAGE_PATH=... on the CMake command line,
# or via the CSOUNDUNITY_PACKAGE_PATH environment variable.
# Build artefacts are copied to:
#   <package>/Runtime/macOS/CsoundNativeInput.bundle
#   <package>/Runtime/iOS/libCsoundNativeInput.a
#   <package>/Runtime/VisionOS/libCsoundNativeInput.a
#   <package>/Runtime/Android/<abi>/libcsnativeinput.so
#   <package>/Runtime/Win64/CsoundNativeInput.dll
# ============================================================

if(NOT DEFINED CSOUNDUNITY_PACKAGE_PATH)
    set(CSOUNDUNITY_PACKAGE_PATH "$ENV{CSOUNDUNITY_PACKAGE_PATH}")
endif()

if(NOT CSOUNDUNITY_PACKAGE_PATH)
    message(WARNING
        "CSOUNDUNITY_PACKAGE_PATH is not set. Build artefacts will NOT be "
        "copied into the Unity package. "
        "Pass -DCSOUNDUNITY_PACKAGE_PATH=/path/to/Packages/CsoundUnity or set the env var.")
endif()

# Shared Apple source (macOS + iOS + visionOS use cni_apple.mm)
set(APPLE_SRC "${SRC_DIR}/cni_apple.mm")

# ============================================================
# macOS — universal bundle (arm64 + x86_64)
# ============================================================
# cmake .. -DTARGET_MACOS=ON \
#          -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
#          -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
#          -DCMAKE_BUILD_TYPE=Release
# ============================================================

if(TARGET_MACOS OR (APPLE AND NOT IOS AND NOT VISIONOS))
    add_library(CsoundNativeInput MODULE "${APPLE_SRC}")
    target_include_directories(CsoundNativeInput PRIVATE "${SRC_DIR}")

    set_target_properties(CsoundNativeInput PROPERTIES
        BUNDLE              TRUE
        BUNDLE_EXTENSION    bundle
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY        ""
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED     "NO"
    )

    find_library(COREAUDIO_LIB    CoreAudio    REQUIRED)
    find_library(AUDIOUNIT_LIB    AudioUnit    REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(AVFOUNDATION_LIB AVFoundation REQUIRED)
    find_library(FOUNDATION_LIB   Foundation   REQUIRED)
    target_link_libraries(CsoundNativeInput
        "${COREAUDIO_LIB}" "${AUDIOUNIT_LIB}"
        "${AUDIOTOOLBOX_LIB}" "${AVFOUNDATION_LIB}" "${FOUNDATION_LIB}")

    if(CSOUNDUNITY_PACKAGE_PATH)
        add_custom_command(TARGET CsoundNativeInput POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:CsoundNativeInput>"
                "${CSOUNDUNITY_PACKAGE_PATH}/Runtime/macOS/CsoundNativeInput.bundle"
            COMMENT "Copying CsoundNativeInput.bundle to Unity package")
    endif()
endif()

# ============================================================
# iOS — static library (arm64)
# ============================================================
# cmake .. -G Xcode \
#          -DCMAKE_SYSTEM_NAME=iOS \
#          -DCMAKE_OSX_ARCHITECTURES=arm64 \
#          -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
#          -DCMAKE_BUILD_TYPE=Release
# cmake --build . --config Release
# ============================================================

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_library(CsoundNativeInput STATIC "${APPLE_SRC}")
    target_include_directories(CsoundNativeInput PRIVATE "${SRC_DIR}")

    set_target_properties(CsoundNativeInput PROPERTIES
        XCODE_ATTRIBUTE_ENABLE_BITCODE         "NO"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY     ""
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED  "NO"
    )

    find_library(AUDIOUNIT_LIB   AudioUnit    REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(AVFOUNDATION_LIB AVFoundation REQUIRED)
    find_library(FOUNDATION_LIB  Foundation   REQUIRED)
    target_link_libraries(CsoundNativeInput
        "${AUDIOUNIT_LIB}" "${AUDIOTOOLBOX_LIB}"
        "${AVFOUNDATION_LIB}" "${FOUNDATION_LIB}")

    if(CSOUNDUNITY_PACKAGE_PATH)
        add_custom_command(TARGET CsoundNativeInput POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:CsoundNativeInput>"
                "${CSOUNDUNITY_PACKAGE_PATH}/Runtime/iOS/libCsoundNativeInput.a"
            COMMENT "Copying libCsoundNativeInput.a to Unity package (iOS)")
    endif()
endif()

# ============================================================
# visionOS — static library (arm64)
# ============================================================
# cmake .. -G Xcode \
#          -DCMAKE_SYSTEM_NAME=visionOS \
#          -DCMAKE_OSX_ARCHITECTURES=arm64 \
#          -DCMAKE_OSX_DEPLOYMENT_TARGET=1.0 \
#          -DCMAKE_BUILD_TYPE=Release
# ============================================================

if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    add_library(CsoundNativeInput STATIC "${APPLE_SRC}")
    target_include_directories(CsoundNativeInput PRIVATE "${SRC_DIR}")

    set_target_properties(CsoundNativeInput PROPERTIES
        XCODE_ATTRIBUTE_ENABLE_BITCODE        "NO"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY    ""
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    )

    find_library(AUDIOUNIT_LIB    AudioUnit    REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(AVFOUNDATION_LIB AVFoundation REQUIRED)
    find_library(FOUNDATION_LIB   Foundation   REQUIRED)
    target_link_libraries(CsoundNativeInput
        "${AUDIOUNIT_LIB}" "${AUDIOTOOLBOX_LIB}"
        "${AVFOUNDATION_LIB}" "${FOUNDATION_LIB}")

    if(CSOUNDUNITY_PACKAGE_PATH)
        add_custom_command(TARGET CsoundNativeInput POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:CsoundNativeInput>"
                "${CSOUNDUNITY_PACKAGE_PATH}/Runtime/VisionOS/libCsoundNativeInput.a"
            COMMENT "Copying libCsoundNativeInput.a to Unity package (visionOS)")
    endif()
endif()

# ============================================================
# Windows — DLL (WASAPI, x86_64)
# ============================================================
# cmake .. -G "Visual Studio 17 2022" -A x64
# cmake --build . --config Release
# ============================================================

if(WIN32 AND NOT ANDROID)
    add_library(CsoundNativeInput SHARED "${SRC_DIR}/cni_wasapi.cpp")
    target_include_directories(CsoundNativeInput PRIVATE "${SRC_DIR}")
    target_compile_definitions(CsoundNativeInput PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(CsoundNativeInput ole32 oleaut32 avrt uuid winmm)
    set_target_properties(CsoundNativeInput PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

    if(CSOUNDUNITY_PACKAGE_PATH)
        add_custom_command(TARGET CsoundNativeInput POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:CsoundNativeInput>"
                "${CSOUNDUNITY_PACKAGE_PATH}/Runtime/Win64/CsoundNativeInput.dll"
            COMMENT "Copying CsoundNativeInput.dll to Unity package")
    endif()
endif()

# ============================================================
# Android — shared library (AAudio, per-ABI)
# ============================================================
# cmake .. -DANDROID_ABI=arm64-v8a \
#          -DANDROID_PLATFORM=android-26 \
#          -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#          -DCMAKE_BUILD_TYPE=Release
# ============================================================

if(ANDROID)
    add_library(csnativeinput SHARED "${SRC_DIR}/cni_aaudio.cpp")
    target_include_directories(csnativeinput PRIVATE "${SRC_DIR}")
    target_link_libraries(csnativeinput aaudio android log)

    if(CSOUNDUNITY_PACKAGE_PATH)
        add_custom_command(TARGET csnativeinput POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:csnativeinput>"
                "${CSOUNDUNITY_PACKAGE_PATH}/Runtime/Android/${ANDROID_ABI}/libcsnativeinput.so"
            COMMENT "Copying libcsnativeinput.so to Unity package (${ANDROID_ABI})")
    endif()
endif()
