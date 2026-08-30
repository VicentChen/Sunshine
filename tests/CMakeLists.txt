cmake_minimum_required(VERSION 3.13)
# https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md#foundational-c-support

project(test_sunshine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

include_directories("${CMAKE_SOURCE_DIR}")

enable_testing()

# Add GoogleTest directory to the project
set(GTEST_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/lizardbyte-common/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
if(NOT TARGET gtest)
    add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
endif()
include_directories("${GTEST_SOURCE_DIR}/googletest/include" "${GTEST_SOURCE_DIR}")

# coverage
# https://gcovr.com/en/stable/guide/compiling.html#compiler-options
option(SUNSHINE_LLVM_COVERAGE "Use LLVM source-based coverage" OFF)
if(SUNSHINE_LLVM_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -ggdb -O0)
    add_link_options(-fprofile-instr-generate)
else()
    string(APPEND CMAKE_CXX_FLAGS " -fprofile-arcs -ftest-coverage -ggdb -O0")
    string(APPEND CMAKE_C_FLAGS " -fprofile-arcs -ftest-coverage -ggdb -O0")
endif()

# Find the correct libgcov library path matching the gcc compiler version
# This ensures the test executable links against the same libgcov version used during compilation
if(UNIX AND NOT APPLE)
    # Get the gcc compiler version
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -dumpversion
        OUTPUT_VARIABLE GCC_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Extract major version
    string(REGEX MATCH "^[0-9]+" GCC_MAJOR_VERSION "${GCC_VERSION}")

    # Search for the gcc library directory matching this version
    file(GLOB GCC_LIB_DIRS "/usr/lib/gcc/*/*${GCC_MAJOR_VERSION}.*")

    if(GCC_LIB_DIRS)
        list(GET GCC_LIB_DIRS 0 GCC_LIB_DIR)
        message(STATUS "Found GCC library directory: ${GCC_LIB_DIR}")

        # Look for libgcov.a in the gcc library directory
        find_library(LIBGCOV_LIBRARY
            NAMES gcov
            PATHS ${GCC_LIB_DIR}
            NO_DEFAULT_PATH
        )

        if(LIBGCOV_LIBRARY)
            message(STATUS "Found libgcov: ${LIBGCOV_LIBRARY}")
            # Store this to link against later
            set(GCOV_LINK_LIBRARY ${LIBGCOV_LIBRARY})
        else()
            message(WARNING "Could not find libgcov in ${GCC_LIB_DIR}")
        endif()
    else()
        message(WARNING "Could not find GCC library directory for version ${GCC_VERSION}")
    endif()
endif()

# if windows
if (WIN32)
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif ()

# modify SUNSHINE_DEFINITIONS
if (WIN32)
    list(APPEND
            SUNSHINE_DEFINITIONS SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
elseif (NOT APPLE)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/linux/assets/shaders/opengl")
endif ()

set(TEST_DEFINITIONS)  # list will be appended as needed

# this indicates we're building tests in case sunshine needs to adjust some code or add private tests
list(APPEND TEST_DEFINITIONS SUNSHINE_TESTS)
list(APPEND TEST_DEFINITIONS SUNSHINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
list(APPEND TEST_DEFINITIONS SUNSHINE_TEST_BIN_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# Override SUNSHINE_ASSETS_DIR to use a writable temp directory for tests
# Remove the existing definition from SUNSHINE_DEFINITIONS to avoid redefinition error
list(FILTER SUNSHINE_DEFINITIONS EXCLUDE REGEX "^SUNSHINE_ASSETS_DIR=")
list(APPEND TEST_DEFINITIONS SUNSHINE_ASSETS_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets")

if(NOT WIN32)
    find_package(Udev 255)  # we need 255+ for udevadm verify
    message(STATUS "UDEV_FOUND: ${UDEV_FOUND}")
    if(UDEV_FOUND)
        list(APPEND TEST_DEFINITIONS UDEVADM_EXECUTABLE="${UDEVADM_EXECUTABLE}")
    endif()
endif()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tests/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cpp)
if(NOT SUNSHINE_BUILD_XBOX_REMOTE_PROBE)
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_xbox_remote_transport.cpp)
endif()
if(SUNSHINE_ENABLE_TRAY AND (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    set(SUNSHINE_TRAY_TEST_ICON_SOURCE_DIR
            "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/images")
    set(SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/test_assets/web/images")
    file(MAKE_DIRECTORY "${SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR}")

    configure_file(
            "${CMAKE_SOURCE_DIR}/sunshine.svg"
            "${SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR}/logo-sunshine.svg"
            COPYONLY)

    if(WIN32)
        configure_file(
                "${CMAKE_SOURCE_DIR}/third-party/libvirtualhid/libvirtualhid.svg"
                "${SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR}/logo-libvirtualhid.svg"
                COPYONLY)
    endif()

    set(SUNSHINE_TRAY_TEST_ICONS
            sunshine-locked.svg
            sunshine-playing.svg
            sunshine-pausing.svg)

    foreach(icon IN LISTS SUNSHINE_TRAY_TEST_ICONS)
        configure_file(
                "${SUNSHINE_TRAY_TEST_ICON_SOURCE_DIR}/${icon}"
                "${SUNSHINE_TRAY_TEST_ICON_DESTINATION_DIR}/${icon}"
                COPYONLY)
    endforeach()

    add_library(sunshine_tray_test_helpers STATIC
            ${CMAKE_SOURCE_DIR}/third-party/tray/tests/notification_utils.cpp
            ${CMAKE_SOURCE_DIR}/third-party/tray/tests/screenshot_utils.cpp)
    set_target_properties(sunshine_tray_test_helpers PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON)
    target_include_directories(sunshine_tray_test_helpers PRIVATE
            ${CMAKE_SOURCE_DIR}/third-party/tray)
    target_link_libraries(sunshine_tray_test_helpers PRIVATE
            lizardbyte::common)

    if(WIN32)
        target_link_libraries(sunshine_tray_test_helpers PRIVATE
                gdi32
                gdiplus)
    else()
        find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Gui)
        find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Gui)
        set(SUNSHINE_TRAY_TEST_QT_GUI_TARGET Qt${QT_VERSION_MAJOR}::Gui)

        target_link_libraries(sunshine_tray_test_helpers PRIVATE
                ${SUNSHINE_TRAY_TEST_QT_GUI_TARGET})
        if(APPLE)
            target_link_libraries(sunshine_tray_test_helpers PRIVATE
                    "-framework Cocoa")
        endif()
    endif()
endif()
# The HDMI RX smoke has its own main() and is intentionally not part of gtest.
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/rkmpp_hdmirx_smoke.cpp)
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/rkmpp_rkmpp_smoke.cpp)
# The layout probe owns its main() and is built as its own RKMPP-only executable.
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/rkmpp_rkmpp_layout_test.cpp)
# The Annex B probe also owns its main() and must never be pulled into the
# generic test target by the recursive source glob. Its dedicated executable
# is created only when the current configure pass enabled RKMPP below.
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/rkmpp_rkmpp_annexb_test.cpp)
# The RGA hardware smoke has its own main() and is built only after CMake has
# compiled the required im2d API probe.
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/rkmpp_rga_smoke.cpp)
# These sources depend on RKMPP headers and are excluded entirely when support
# is disabled; the ON build supplies them through the platform source list and
# the dedicated layout target below.
if(NOT RKMPP_FOUND)
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_hdmirx.cpp)
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_edid.cpp)
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_edid_negotiator.cpp)
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_rkmpp_input.cpp)
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_input_state_machine.cpp)
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_rkmpp_stress.cpp)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_rga.cpp)
    list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/platform/test_edid.cpp)
endif()
# Add macOS-specific test files only when building tests for macOS
if (APPLE)
    file(GLOB_RECURSE MACOS_TEST_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/tests/*.mm)
    list(APPEND TEST_SOURCES ${MACOS_TEST_SOURCES})
endif ()

set(SUNSHINE_SOURCES
        ${SUNSHINE_TARGET_FILES})

# remove main.cpp from the list of sources
list(REMOVE_ITEM SUNSHINE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_executable(${PROJECT_NAME}
        ${TEST_SOURCES}
        ${SUNSHINE_SOURCES})

if(SUNSHINE_BUILD_XBOX_REMOTE_PROBE)
    target_link_libraries(${PROJECT_NAME} LibDataChannel::LibDataChannelStatic)
endif()

# Copy files needed for config consistency tests to build directory
# This ensures both CLI and CLion can access the same files relative to the test executable
# Using configure_file ensures files are copied when they change between builds
set(INTEGRATION_TEST_FILES
    "src/config.cpp"
    "src_assets/common/assets/web/config.html"
    "docs/configuration.md"
    "src_assets/common/assets/web/public/assets/locale/en.json"
    "src_assets/common/assets/web/configs/tabs/General.vue"
    "src_assets/linux/misc/60-sunshine.rules"
)

foreach(file ${INTEGRATION_TEST_FILES})
    configure_file(
        "${CMAKE_SOURCE_DIR}/${file}"
        "${CMAKE_CURRENT_BINARY_DIR}/${file}"
        COPYONLY
    )
endforeach()

# Copy all locale files for locale consistency tests
# Use a custom command to properly handle both adding and removing files
set(LOCALE_SRC_DIR "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/assets/locale")
set(LOCALE_DST_DIR "${CMAKE_CURRENT_BINARY_DIR}/src_assets/common/assets/web/public/assets/locale")
add_custom_target(sync_locale_files ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LOCALE_SRC_DIR}" "${LOCALE_DST_DIR}"
    COMMENT "Synchronizing locale files for tests"
    VERBATIM
)

foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(${PROJECT_NAME} ${dep})  # compile these before sunshine
endforeach()

# Ensure locale files are synchronized before building the test executable
add_dependencies(${PROJECT_NAME} sync_locale_files)

# Build the list of libraries to link
set(TEST_LINK_LIBRARIES
    ${SUNSHINE_EXTERNAL_LIBRARIES}
    lizardbyte::test_support
    ${PLATFORM_LIBRARIES}
)

if(SUNSHINE_ENABLE_TRAY AND (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    list(APPEND TEST_LINK_LIBRARIES
            sunshine_tray_test_helpers)
    if(SUNSHINE_TRAY_TEST_QT_GUI_TARGET)
        list(APPEND TEST_LINK_LIBRARIES
                ${SUNSHINE_TRAY_TEST_QT_GUI_TARGET})
    endif()
endif()

# Add the specific libgcov library if found
if(GCOV_LINK_LIBRARY)
    list(APPEND TEST_LINK_LIBRARIES ${GCOV_LINK_LIBRARY})
endif()

if(RKMPP_FOUND)
    add_executable(rkmpp_rkmpp_annexb_test
        "${CMAKE_SOURCE_DIR}/tests/rkmpp_rkmpp_annexb_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/rkmpp_annexb.cpp")
    target_link_libraries(rkmpp_rkmpp_annexb_test PRIVATE RockchipMPP::rockchip_mpp)
    target_compile_definitions(rkmpp_rkmpp_annexb_test PRIVATE SUNSHINE_BUILD_RKMPP)
    target_compile_options(rkmpp_rkmpp_annexb_test PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

    add_executable(rkmpp_rkmpp_layout_test
        ${CMAKE_SOURCE_DIR}/tests/rkmpp_rkmpp_layout_test.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/rkmpp.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/rkmpp_annexb.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/hdmirx_policy.cpp)
    target_link_libraries(rkmpp_rkmpp_layout_test PRIVATE RockchipMPP::rockchip_mpp Boost::asio Boost::format Boost::log Boost::process)
    target_compile_definitions(rkmpp_rkmpp_layout_test PRIVATE SUNSHINE_BUILD_RKMPP)
    target_compile_options(rkmpp_rkmpp_layout_test PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

    add_executable(rkmpp_rkmpp_smoke
        ${CMAKE_SOURCE_DIR}/tests/rkmpp_rkmpp_smoke.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/hdmirx.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/rkmpp.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/edid.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/input_state_machine.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/hdmirx_policy.cpp)
    target_link_libraries(rkmpp_rkmpp_smoke PRIVATE RockchipMPP::rockchip_mpp Boost::asio Boost::format Boost::log Boost::process)
    target_compile_definitions(rkmpp_rkmpp_smoke PRIVATE SUNSHINE_BUILD_RKMPP)
    target_compile_options(rkmpp_rkmpp_smoke PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
endif()

if(RGA_FOUND)
    add_executable(rkmpp_rga_smoke
        ${CMAKE_SOURCE_DIR}/tests/rkmpp_rga_smoke.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/rga.cpp)
    target_link_libraries(rkmpp_rga_smoke PRIVATE LibRGA::librga)
    target_compile_definitions(rkmpp_rga_smoke PRIVATE SUNSHINE_BUILD_RGA)
    target_compile_options(rkmpp_rga_smoke PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
endif()

target_link_libraries(${PROJECT_NAME} ${TEST_LINK_LIBRARIES})
target_compile_definitions(${PROJECT_NAME} PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(${PROJECT_NAME} PRIVATE ${SUNSHINE_LINK_OPTIONS})

# Hardware-only RKMPP smoke.  It is intentionally independent of gtest so
# it can be run directly on a Rockchip HDMI RX host without the full test suite.
if(RKMPP_FOUND)
    add_executable(rkmpp_hdmirx_smoke
        ${CMAKE_SOURCE_DIR}/tests/rkmpp_hdmirx_smoke.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/hdmirx.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/edid.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/input_state_machine.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/linux/rkmpp.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/hdmirx_policy.cpp)
    target_link_libraries(rkmpp_hdmirx_smoke PRIVATE RockchipMPP::rockchip_mpp Boost::asio Boost::format Boost::log Boost::process)
    target_compile_definitions(rkmpp_hdmirx_smoke PRIVATE SUNSHINE_BUILD_RKMPP)
    target_compile_options(rkmpp_hdmirx_smoke PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
endif()

if (WIN32)
    # prefer static libraries since we're linking statically
    # this fixes libcurl linking errors when using non MSYS2 version of CMake
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_SEARCH_START_STATIC 1)

    # Copy minhook-detours DLL to test binary directory for ARM64
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64" AND DEFINED _MINHOOK_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_MINHOOK_DLL}"
                "${CMAKE_CURRENT_BINARY_DIR}"
            COMMENT "Copying minhook-detours DLL to test binary directory"
            VERBATIM
        )
    endif()
endif ()
