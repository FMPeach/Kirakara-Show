# FFmpeg is always imported as DLLs; never link static GPL/nonfree libraries.
set(KIRAKARA_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.deps/ffmpeg-sdk"
    CACHE PATH "LGPL FFmpeg shared SDK (include/lib/bin)")
if(NOT EXISTS "${KIRAKARA_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg SDK missing. Run tools/prepare_ffmpeg.ps1 or set KIRAKARA_FFMPEG_ROOT to an LGPL shared SDK.")
endif()
if(NOT EXISTS "${KIRAKARA_FFMPEG_ROOT}/bin/ffmpeg.exe")
    message(FATAL_ERROR "FFmpeg SDK must include ffmpeg.exe for configuration/license validation.")
endif()
execute_process(COMMAND "${KIRAKARA_FFMPEG_ROOT}/bin/ffmpeg.exe" -version
    OUTPUT_VARIABLE _ffmpeg_version ERROR_VARIABLE _ffmpeg_error RESULT_VARIABLE _ffmpeg_status)
if(NOT _ffmpeg_status EQUAL 0 OR _ffmpeg_version MATCHES "--enable-(gpl|nonfree)( |\n)")
    message(FATAL_ERROR "FFmpeg must be a working LGPL shared build without GPL/nonfree components: ${_ffmpeg_error}")
endif()
if(NOT _ffmpeg_version MATCHES "--enable-shared")
    message(FATAL_ERROR "FFmpeg must be built with --enable-shared.")
endif()
foreach(_component avcodec avformat avutil swscale swresample)
    file(GLOB _dll "${KIRAKARA_FFMPEG_ROOT}/bin/${_component}-*.dll")
    list(LENGTH _dll _dll_count)
    if(NOT _dll_count EQUAL 1)
        message(FATAL_ERROR "Expected one FFmpeg ${_component} DLL, found ${_dll_count}.")
    endif()
    if(MSVC)
        set(_implib "${KIRAKARA_FFMPEG_ROOT}/lib/${_component}.lib")
    else()
        set(_implib "${KIRAKARA_FFMPEG_ROOT}/lib/lib${_component}.dll.a")
    endif()
    if(NOT EXISTS "${_implib}")
        message(FATAL_ERROR "FFmpeg import library missing: ${_implib}")
    endif()
    add_library(FFmpeg::${_component} SHARED IMPORTED GLOBAL)
    set_target_properties(FFmpeg::${_component} PROPERTIES
        IMPORTED_LOCATION "${_dll}" IMPORTED_IMPLIB "${_implib}"
        INTERFACE_INCLUDE_DIRECTORIES "${KIRAKARA_FFMPEG_ROOT}/include")
    list(APPEND KIRAKARA_FFMPEG_DLLS "${_dll}")
endforeach()

add_library(kirakara_ffmpeg_decode STATIC apps/show_host/media/ffmpeg_decode.cpp)
target_compile_features(kirakara_ffmpeg_decode PUBLIC cxx_std_20)
target_compile_definitions(kirakara_ffmpeg_decode PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(kirakara_ffmpeg_decode PUBLIC FFmpeg::avcodec FFmpeg::avformat
    FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample)

function(kirakara_link_ffmpeg target)
    get_property(_attached TARGET ${target} PROPERTY KIRAKARA_FFMPEG_ATTACHED)
    if(_attached)
        return()
    endif()
    set_property(TARGET ${target} PROPERTY KIRAKARA_FFMPEG_ATTACHED TRUE)
    target_link_libraries(${target} PRIVATE kirakara_ffmpeg_decode)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KIRAKARA_FFMPEG_DLLS}
            "$<TARGET_FILE_DIR:${target}>" VERBATIM)
    if(EXISTS "${KIRAKARA_FFMPEG_ROOT}/third_party/ffmpeg")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${KIRAKARA_FFMPEG_ROOT}/third_party/ffmpeg"
                "$<TARGET_FILE_DIR:${target}>/third_party/ffmpeg" VERBATIM)
    endif()
endfunction()
