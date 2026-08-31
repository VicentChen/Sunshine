# Find Rockchip librga by compiling the exact im2d API Sunshine needs.
# A pkg-config version is deliberately only a search hint: distributions often
# package librga without a .pc file, and API availability cannot be inferred
# from a version string alone.

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_LIBRGA QUIET librga)
endif()

find_path(
        LIBRGA_INCLUDE_DIR
        NAMES im2d.h rga/im2d.h
        HINTS ${CMAKE_SOURCE_DIR}/third-party/librga/include ${PC_LIBRGA_INCLUDE_DIRS})
find_library(
        LIBRGA_LIBRARY
        NAMES rga librga
        HINTS ${CMAKE_SOURCE_DIR}/third-party/librga/libs/Linux/gcc-aarch64 ${PC_LIBRGA_LIBRARY_DIRS})
# The official prebuilt librga is a C++ shared object and has a DT_NEEDED
# dependency on libstdc++; expose it to consumers so --as-needed links work.
find_library(LIBRGA_CXX_LIBRARY NAMES stdc++ libstdc++.so.6)
set(LIBRGA_CAPABILITY_CHECK FALSE)
if(LIBRGA_INCLUDE_DIR AND LIBRGA_LIBRARY)
    # Official librga headers rely on NULL, memset(), and printf() without
    # including their standard declarations, so a C++ consumer must provide them.

    if(EXISTS "${LIBRGA_INCLUDE_DIR}/im2d.h")
        set(_LIBRGA_IM2D_INCLUDE "#include <cstddef>\n#include <cstdio>\n#include <cstring>\n#include <im2d.h>")
        set(LIBRGA_IM2D_HEADER_RGA FALSE)
    elseif(EXISTS "${LIBRGA_INCLUDE_DIR}/rga/im2d.h")
        set(_LIBRGA_IM2D_INCLUDE "#include <cstddef>\n#include <cstdio>\n#include <cstring>\n#include <rga/im2d.h>")
        set(LIBRGA_IM2D_HEADER_RGA TRUE)
    endif()

    if(DEFINED _LIBRGA_IM2D_INCLUDE)
        set(_LIBRGA_REQUIRED_INCLUDES_DEFINED FALSE)
        if(DEFINED CMAKE_REQUIRED_INCLUDES)
            set(_LIBRGA_REQUIRED_INCLUDES_DEFINED TRUE)
            set(_LIBRGA_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
        endif()
        set(_LIBRGA_REQUIRED_LIBRARIES_DEFINED FALSE)
        if(DEFINED CMAKE_REQUIRED_LIBRARIES)
            set(_LIBRGA_REQUIRED_LIBRARIES_DEFINED TRUE)
            set(_LIBRGA_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
        endif()
        set(_LIBRGA_REQUIRED_QUIET_DEFINED FALSE)
        if(DEFINED CMAKE_REQUIRED_QUIET)
            set(_LIBRGA_REQUIRED_QUIET_DEFINED TRUE)
            set(_LIBRGA_REQUIRED_QUIET "${CMAKE_REQUIRED_QUIET}")
        endif()

        set(CMAKE_REQUIRED_INCLUDES "${LIBRGA_INCLUDE_DIR}")
        set(CMAKE_REQUIRED_LIBRARIES "${LIBRGA_LIBRARY}")
        set(CMAKE_REQUIRED_QUIET TRUE)
        include(CheckCXXSourceCompiles)
        unset(LIBRGA_CAPABILITY_CHECK CACHE)
        unset(LIBRGA_CAPABILITY_CHECK)
        check_cxx_source_compiles(
                "${_LIBRGA_IM2D_INCLUDE}\nint main() {\n  rga_buffer_t source {};\n  rga_buffer_t destination {};\n  im_rect source_rect {};\n  im_rect destination_rect {};\n  rga_buffer_handle_t handle = importbuffer_fd(-1, 1);\n  const IM_STATUS released = releasebuffer_handle(handle);\n  const rga_buffer_t rgba = wrapbuffer_handle(handle, 128, 64, RK_FORMAT_RGBA_8888, 128, 64);\n  const rga_buffer_t bgr = wrapbuffer_handle(handle, 128, 64, RK_FORMAT_BGR_888, 128, 64);\n  const rga_buffer_t nv24 = wrapbuffer_handle(handle, 128, 64, RK_FORMAT_YCbCr_444_SP, 128, 64);\n  const rga_buffer_t nv16 = wrapbuffer_handle(handle, 128, 64, RK_FORMAT_YCbCr_422_SP, 128, 64);\n  const rga_buffer_t nv12 = wrapbuffer_handle(handle, 128, 64, RK_FORMAT_YCbCr_420_SP, 128, 64);\n  imsetColorSpace(&source, IM_RGB_FULL_RANGE);\n  imsetColorSpace(&destination, IM_YUV_BT709_LIMIT_RANGE);\n  const IM_STATUS checked_fill = imcheck(rga_buffer_t {}, destination, im_rect {}, destination_rect, IM_COLOR_FILL);\n  const IM_STATUS checked_resize = imcheck(source, destination, source_rect, destination_rect);\n  const IM_STATUS filled = imfill(destination, destination_rect, 0U, 1);\n  const IM_STATUS resized = imresize(source, destination, 0.0, 0.0, IM_INTERP_LINEAR, 1);\n  const IM_STATUS processed = improcess(source, destination, {}, source_rect, destination_rect, {}, IM_SYNC);\n  const IM_STATUS converted = imcvtcolor(source, destination, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_YCbCr_420_SP, IM_COLOR_SPACE_DEFAULT, 1);\n  return released == IM_STATUS_SUCCESS && checked_fill == IM_STATUS_NOERROR && checked_resize == IM_STATUS_NOERROR && filled == IM_STATUS_SUCCESS && resized == IM_STATUS_SUCCESS && processed == IM_STATUS_SUCCESS && converted == IM_STATUS_SUCCESS && imStrError(IM_STATUS_FAILED) != nullptr && sizeof(rgba) + sizeof(bgr) + sizeof(nv24) + sizeof(nv16) + sizeof(nv12) != 0;\n}"
                LIBRGA_CAPABILITY_CHECK)

        if(_LIBRGA_REQUIRED_INCLUDES_DEFINED)
            set(CMAKE_REQUIRED_INCLUDES "${_LIBRGA_REQUIRED_INCLUDES}")
        else()
            unset(CMAKE_REQUIRED_INCLUDES)
        endif()
        if(_LIBRGA_REQUIRED_LIBRARIES_DEFINED)
            set(CMAKE_REQUIRED_LIBRARIES "${_LIBRGA_REQUIRED_LIBRARIES}")
        else()
            unset(CMAKE_REQUIRED_LIBRARIES)
        endif()
        if(_LIBRGA_REQUIRED_QUIET_DEFINED)
            set(CMAKE_REQUIRED_QUIET "${_LIBRGA_REQUIRED_QUIET}")
        else()
            unset(CMAKE_REQUIRED_QUIET)
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibRGA REQUIRED_VARS LIBRGA_INCLUDE_DIR LIBRGA_LIBRARY LIBRGA_CXX_LIBRARY LIBRGA_CAPABILITY_CHECK)

if(LibRGA_FOUND)
    set(LIBRGA_INCLUDE_DIRS "${LIBRGA_INCLUDE_DIR}")
    set(LIBRGA_LIBRARIES "${LIBRGA_LIBRARY}")
    if(NOT TARGET LibRGA::librga)
        add_library(LibRGA::librga UNKNOWN IMPORTED)
        set_target_properties(
                LibRGA::librga
                PROPERTIES
                    IMPORTED_LOCATION "${LIBRGA_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${LIBRGA_INCLUDE_DIR}"
                    INTERFACE_LINK_LIBRARIES "${LIBRGA_CXX_LIBRARY}")
        if(LIBRGA_IM2D_HEADER_RGA)
            set_property(TARGET LibRGA::librga APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS SUNSHINE_LIBRGA_IM2D_HEADER_RGA)
        endif()
    endif()
endif()

unset(_LIBRGA_IM2D_INCLUDE)
unset(_LIBRGA_REQUIRED_INCLUDES_DEFINED)
unset(_LIBRGA_REQUIRED_INCLUDES)
unset(_LIBRGA_REQUIRED_LIBRARIES_DEFINED)
unset(_LIBRGA_REQUIRED_LIBRARIES)
unset(_LIBRGA_REQUIRED_QUIET_DEFINED)
unset(_LIBRGA_REQUIRED_QUIET)
