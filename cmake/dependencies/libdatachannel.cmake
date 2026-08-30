# Xbox Remote Play WebRTC transport dependency, enabled only by the explicit feature option.
# The full immutable commit pin is recorded in the Step 04 and Step 11 validation notes.
include(FetchContent)

set(_sunshine_build_shared_libs "${BUILD_SHARED_LIBS}")
set(_sunshine_c_flags "${CMAKE_C_FLAGS}")
set(_sunshine_enable_warnings_as_errors "${ENABLE_WARNINGS_AS_ERRORS}")
set(_sunshine_no_examples "${NO_EXAMPLES}")
set(_sunshine_no_media "${NO_MEDIA}")
set(_sunshine_no_tests "${NO_TESTS}")
set(_sunshine_no_websocket "${NO_WEBSOCKET}")
set(_sunshine_use_nice "${USE_NICE}")
set(_sunshine_use_system_srtp "${USE_SYSTEM_SRTP}")
set(_sunshine_use_system_usrsctp "${USE_SYSTEM_USRSCTP}")

set(BUILD_SHARED_LIBS OFF)
string(REPLACE "-fuse-ld=/usr/bin/ld" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
set(ENABLE_WARNINGS_AS_ERRORS OFF)
set(NO_EXAMPLES ON)
set(NO_MEDIA OFF)
set(NO_TESTS ON)
set(NO_WEBSOCKET ON)
set(USE_NICE OFF)
set(USE_SYSTEM_SRTP OFF)
set(USE_SYSTEM_USRSCTP OFF)

FetchContent_Declare(libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG c6696d157b5612df2a741d9a03b192b47ab6cefb
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE TRUE
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(libdatachannel)

set(BUILD_SHARED_LIBS "${_sunshine_build_shared_libs}")
set(CMAKE_C_FLAGS "${_sunshine_c_flags}")
set(ENABLE_WARNINGS_AS_ERRORS "${_sunshine_enable_warnings_as_errors}")
set(NO_EXAMPLES "${_sunshine_no_examples}")
set(NO_MEDIA "${_sunshine_no_media}")
set(NO_TESTS "${_sunshine_no_tests}")
set(NO_WEBSOCKET "${_sunshine_no_websocket}")
set(USE_NICE "${_sunshine_use_nice}")
set(USE_SYSTEM_SRTP "${_sunshine_use_system_srtp}")
set(USE_SYSTEM_USRSCTP "${_sunshine_use_system_usrsctp}")

unset(_sunshine_build_shared_libs)
unset(_sunshine_c_flags)
unset(_sunshine_enable_warnings_as_errors)
unset(_sunshine_no_examples)
unset(_sunshine_no_media)
unset(_sunshine_no_tests)
unset(_sunshine_no_websocket)
unset(_sunshine_use_nice)
unset(_sunshine_use_system_srtp)
unset(_sunshine_use_system_usrsctp)
