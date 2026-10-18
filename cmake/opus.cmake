# Keep Opus private and static: no new runtime DLL or exported host API.
enable_language(C)
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

function(kirakara_add_opus)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(OPUS_BUILD_SHARED_LIBRARY OFF)
    set(OPUS_BUILD_TESTING OFF)
    set(OPUS_BUILD_PROGRAMS OFF)
    set(OPUS_DRED OFF)
    set(OPUS_OSCE OFF)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
    set(INSTALL_DOCS OFF)
    set(INSTALL_PKG_CONFIG_MODULE OFF)
    set(INSTALL_CMAKE_PACKAGE_MODULE OFF)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/dependency.json" _lock)
    # GitHub source archives omit the generated package_version file.
    string(JSON OPUS_PACKAGE_VERSION GET "${_lock}" opus version)
    foreach(_name opus ogg opusfile)
        string(JSON _url GET "${_lock}" ${_name} url)
        string(JSON _sha GET "${_lock}" ${_name} sha256)
        string(JSON _archive GET "${_lock}" ${_name} archive)
        set(_local "${CMAKE_CURRENT_SOURCE_DIR}/.deps/sources/${_archive}")
        if(EXISTS "${_local}")
            set(_url "${_local}")
        endif()
        # opusfile uses its upstream four-file library source list; opusurl
        # and OpenSSL are unnecessary because the host uses Windows HTTP.
        if(_name STREQUAL "opusfile")
            FetchContent_Declare(${_name} URL "${_url}" URL_HASH "SHA256=${_sha}"
                SOURCE_SUBDIR kirakara-no-cmake)
        else()
            FetchContent_Declare(${_name} URL "${_url}" URL_HASH "SHA256=${_sha}")
        endif()
        FetchContent_MakeAvailable(${_name})
    endforeach()
    add_library(kirakara_opusfile STATIC
        "${opusfile_SOURCE_DIR}/src/info.c"
        "${opusfile_SOURCE_DIR}/src/internal.c"
        "${opusfile_SOURCE_DIR}/src/opusfile.c"
        "${opusfile_SOURCE_DIR}/src/stream.c")
    target_include_directories(kirakara_opusfile PUBLIC "${opusfile_SOURCE_DIR}/include")
    target_link_libraries(kirakara_opusfile PUBLIC opus Ogg::ogg)
    # opusfile includes <opus_multistream.h>, not <opus/opus_multistream.h>.
    target_include_directories(kirakara_opusfile PRIVATE "${opus_SOURCE_DIR}/include")
    add_library(kirakara_opus_audio STATIC apps/show_host/audio/opus_decode.cpp)
    target_compile_features(kirakara_opus_audio PUBLIC cxx_std_20)
    target_compile_definitions(kirakara_opus_audio PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(kirakara_opus_audio PUBLIC kirakara_opusfile PRIVATE winhttp)
    target_include_directories(kirakara_opus_audio PRIVATE "${opus_SOURCE_DIR}/include")
endfunction()
kirakara_add_opus()
