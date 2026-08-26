# Find Rockchip MPP without using the pkg-config version as a capability gate.
# pkg-config supplies hints only; headers and the link library decide availability.

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_ROCKCHIPMPP QUIET rockchip_mpp)
endif()

find_path(
        ROCKCHIPMPP_INCLUDE_DIR
        NAMES rk_mpi.h
        HINTS ${PC_ROCKCHIPMPP_INCLUDE_DIRS}
        PATH_SUFFIXES rockchip)
find_file(
        ROCKCHIPMPP_MPP_BUFFER_HEADER
        NAMES mpp_buffer.h
        PATHS ${ROCKCHIPMPP_INCLUDE_DIR}
        NO_DEFAULT_PATH)
find_file(
        ROCKCHIPMPP_MPP_FRAME_HEADER
        NAMES mpp_frame.h
        PATHS ${ROCKCHIPMPP_INCLUDE_DIR}
        NO_DEFAULT_PATH)
find_library(
        ROCKCHIPMPP_LIBRARY
        NAMES rockchip_mpp
        HINTS ${PC_ROCKCHIPMPP_LIBRARY_DIRS})

set(ROCKCHIPMPP_CAPABILITY_CHECK FALSE)
if(ROCKCHIPMPP_INCLUDE_DIR AND ROCKCHIPMPP_MPP_BUFFER_HEADER AND ROCKCHIPMPP_MPP_FRAME_HEADER AND ROCKCHIPMPP_LIBRARY)
    set(_ROCKCHIPMPP_REQUIRED_INCLUDES_DEFINED FALSE)
    if(DEFINED CMAKE_REQUIRED_INCLUDES)
        set(_ROCKCHIPMPP_REQUIRED_INCLUDES_DEFINED TRUE)
        set(_ROCKCHIPMPP_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
    endif()
    set(_ROCKCHIPMPP_REQUIRED_LIBRARIES_DEFINED FALSE)
    if(DEFINED CMAKE_REQUIRED_LIBRARIES)
        set(_ROCKCHIPMPP_REQUIRED_LIBRARIES_DEFINED TRUE)
        set(_ROCKCHIPMPP_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
    endif()
    set(_ROCKCHIPMPP_REQUIRED_QUIET_DEFINED FALSE)
    if(DEFINED CMAKE_REQUIRED_QUIET)
        set(_ROCKCHIPMPP_REQUIRED_QUIET_DEFINED TRUE)
        set(_ROCKCHIPMPP_REQUIRED_QUIET "${CMAKE_REQUIRED_QUIET}")
    endif()

    set(CMAKE_REQUIRED_INCLUDES "${ROCKCHIPMPP_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_LIBRARIES "${ROCKCHIPMPP_LIBRARY}")
    set(CMAKE_REQUIRED_QUIET TRUE)
    include(CheckCXXSourceCompiles)
    unset(ROCKCHIPMPP_CAPABILITY_CHECK CACHE)
    unset(ROCKCHIPMPP_CAPABILITY_CHECK)
    check_cxx_source_compiles(
            "#include <rk_mpi.h>\n#include <mpp_buffer.h>\n#include <mpp_frame.h>\nint main() {\n  MppCtx context = nullptr;\n  MppApi *api = nullptr;\n  MppBuffer buffer = nullptr;\n  MppBufferInfo info {};\n  MppFrame frame = nullptr;\n  return mpp_create(&context, &api) + mpp_buffer_import(&buffer, &info) + mpp_frame_init(&frame);\n}"
            ROCKCHIPMPP_CAPABILITY_CHECK)

    if(_ROCKCHIPMPP_REQUIRED_INCLUDES_DEFINED)
        set(CMAKE_REQUIRED_INCLUDES "${_ROCKCHIPMPP_REQUIRED_INCLUDES}")
    else()
        unset(CMAKE_REQUIRED_INCLUDES)
    endif()
    if(_ROCKCHIPMPP_REQUIRED_LIBRARIES_DEFINED)
        set(CMAKE_REQUIRED_LIBRARIES "${_ROCKCHIPMPP_REQUIRED_LIBRARIES}")
    else()
        unset(CMAKE_REQUIRED_LIBRARIES)
    endif()
    if(_ROCKCHIPMPP_REQUIRED_QUIET_DEFINED)
        set(CMAKE_REQUIRED_QUIET "${_ROCKCHIPMPP_REQUIRED_QUIET}")
    else()
        unset(CMAKE_REQUIRED_QUIET)
    endif()
    unset(_ROCKCHIPMPP_REQUIRED_INCLUDES_DEFINED)
    unset(_ROCKCHIPMPP_REQUIRED_INCLUDES)
    unset(_ROCKCHIPMPP_REQUIRED_LIBRARIES_DEFINED)
    unset(_ROCKCHIPMPP_REQUIRED_LIBRARIES)
    unset(_ROCKCHIPMPP_REQUIRED_QUIET_DEFINED)
    unset(_ROCKCHIPMPP_REQUIRED_QUIET)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
        RockchipMPP
        REQUIRED_VARS ROCKCHIPMPP_INCLUDE_DIR ROCKCHIPMPP_MPP_BUFFER_HEADER ROCKCHIPMPP_MPP_FRAME_HEADER ROCKCHIPMPP_LIBRARY ROCKCHIPMPP_CAPABILITY_CHECK)

if(RockchipMPP_FOUND)
    set(ROCKCHIPMPP_INCLUDE_DIRS ${ROCKCHIPMPP_INCLUDE_DIR})
    set(ROCKCHIPMPP_LIBRARIES ${ROCKCHIPMPP_LIBRARY})
    if(NOT TARGET RockchipMPP::rockchip_mpp)
        add_library(RockchipMPP::rockchip_mpp UNKNOWN IMPORTED)
        set_target_properties(
                RockchipMPP::rockchip_mpp
                PROPERTIES
                    IMPORTED_LOCATION "${ROCKCHIPMPP_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${ROCKCHIPMPP_INCLUDE_DIR}")
    endif()
endif()
