cmake_minimum_required(VERSION 3.20)

project(KirakaraShow VERSION 0.1.0 LANGUAGES CXX)

option(KIRAKARA_BUILD_TESTS "Build Kirakara Show tests" ON)

add_library(kirakara_show
    src/json_value.cpp
    src/lrc_parser.cpp
    src/project_parser.cpp
    src/song_title.cpp
    src/stage_frame.cpp
    src/stage_overlay.cpp
    src/timeline.cpp
    src/config.cpp
)
add_library(Kirakara::Show ALIAS kirakara_show)

target_compile_features(kirakara_show PUBLIC cxx_std_20)
target_include_directories(kirakara_show
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(kirakara_show PRIVATE /W4 /permissive-)
else()
    target_compile_options(kirakara_show PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(KIRAKARA_BUILD_TESTS)
    enable_testing()
    add_executable(kirakara_show_tests tests/timeline_tests.cpp)
    target_link_libraries(kirakara_show_tests PRIVATE Kirakara::Show)
    add_test(NAME kirakara_show_tests COMMAND kirakara_show_tests)

    add_executable(kirakara_show_song_title_tests
        tests/song_title_tests.cpp)
    target_link_libraries(kirakara_show_song_title_tests
        PRIVATE Kirakara::Show)
    add_test(NAME kirakara_show_song_title_tests
        COMMAND kirakara_show_song_title_tests)

    add_executable(kirakara_show_processed_pcm_ring_tests
        tests/processed_pcm_ring_tests.cpp)
    target_compile_features(kirakara_show_processed_pcm_ring_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_processed_pcm_ring_tests
        COMMAND kirakara_show_processed_pcm_ring_tests)

    add_executable(kirakara_show_stage_output_lease_tests
        tests/stage_output_lease_tests.cpp)
    target_compile_features(kirakara_show_stage_output_lease_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_stage_output_lease_tests
        COMMAND kirakara_show_stage_output_lease_tests)

    add_executable(kirakara_show_stage_frame_tests
        tests/stage_frame_tests.cpp)
    target_link_libraries(kirakara_show_stage_frame_tests
        PRIVATE Kirakara::Show)
    add_test(NAME kirakara_show_stage_frame_tests
        COMMAND kirakara_show_stage_frame_tests)

    add_executable(kirakara_show_stage_overlay_tests
        tests/stage_overlay_tests.cpp)
    target_link_libraries(kirakara_show_stage_overlay_tests
        PRIVATE Kirakara::Show)
    add_test(NAME kirakara_show_stage_overlay_tests
        COMMAND kirakara_show_stage_overlay_tests)

    add_executable(kirakara_show_mpeg_ts_muxer_tests
        tests/mpeg_ts_muxer_tests.cpp
        apps/show_host/cast/mpeg_ts_muxer.cpp)
    target_compile_features(kirakara_show_mpeg_ts_muxer_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_mpeg_ts_muxer_tests
        COMMAND kirakara_show_mpeg_ts_muxer_tests)

    add_executable(kirakara_show_aac_adts_packetizer_tests
        tests/aac_adts_packetizer_tests.cpp
        apps/show_host/cast/aac_adts_packetizer.cpp)
    target_compile_features(kirakara_show_aac_adts_packetizer_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_aac_adts_packetizer_tests
        COMMAND kirakara_show_aac_adts_packetizer_tests)

    add_executable(kirakara_show_cast_pcm_resampler_tests
        tests/cast_pcm_resampler_tests.cpp
        apps/show_host/cast/cast_pcm_resampler.cpp)
    target_compile_features(kirakara_show_cast_pcm_resampler_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_cast_pcm_resampler_tests
        COMMAND kirakara_show_cast_pcm_resampler_tests)

    add_executable(kirakara_show_mpeg_ts_stream_buffer_tests
        tests/mpeg_ts_stream_buffer_tests.cpp
        apps/show_host/cast/mpeg_ts_stream_buffer.cpp)
    target_compile_features(kirakara_show_mpeg_ts_stream_buffer_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_mpeg_ts_stream_buffer_tests
        COMMAND kirakara_show_mpeg_ts_stream_buffer_tests)

    add_executable(kirakara_show_native_cast_stream_tests
        tests/native_cast_stream_tests.cpp
        apps/show_host/cast/cast_pipeline_diagnostics.cpp
        apps/show_host/cast/mpeg_ts_muxer.cpp
        apps/show_host/cast/mpeg_ts_stream_buffer.cpp
        apps/show_host/cast/native_cast_stream.cpp)
    target_compile_features(kirakara_show_native_cast_stream_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_native_cast_stream_tests
        COMMAND kirakara_show_native_cast_stream_tests)

    add_executable(kirakara_show_cast_pipeline_diagnostics_tests
        tests/cast_pipeline_diagnostics_tests.cpp
        apps/show_host/cast/cast_pipeline_diagnostics.cpp)
    target_compile_features(kirakara_show_cast_pipeline_diagnostics_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_cast_pipeline_diagnostics_tests
        COMMAND kirakara_show_cast_pipeline_diagnostics_tests)

    add_executable(kirakara_show_cast_program_clock_tests
        tests/cast_program_clock_tests.cpp
        apps/show_host/cast/cast_program_clock.cpp
        apps/show_host/cast/cast_video_buffer_state.cpp)
    target_compile_features(kirakara_show_cast_program_clock_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_cast_program_clock_tests
        COMMAND kirakara_show_cast_program_clock_tests)

    add_executable(kirakara_show_cast_session_lifecycle_tests
        tests/cast_session_lifecycle_tests.cpp
        apps/show_host/cast/cast_session_lifecycle.cpp)
    target_compile_features(kirakara_show_cast_session_lifecycle_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_cast_session_lifecycle_tests
        COMMAND kirakara_show_cast_session_lifecycle_tests)

    add_executable(kirakara_show_d3d11_cast_capability_policy_tests
        tests/d3d11_cast_capability_policy_tests.cpp)
    target_compile_features(kirakara_show_d3d11_cast_capability_policy_tests
        PRIVATE cxx_std_20)
    add_test(NAME kirakara_show_d3d11_cast_capability_policy_tests
        COMMAND kirakara_show_d3d11_cast_capability_policy_tests)
endif()

if(WIN32)
    add_library(kirakara_show_win32
        src/win32/d3d11_texture_surface.cpp
        src/win32/offscreen_surface.cpp
        src/win32/song_title_renderer.cpp
        src/win32/stage_compositor.cpp
        src/win32/stage_frame_resource.cpp
        src/win32/stage_window_presenter.cpp
        src/win32/swap_chain_surface.cpp
        src/win32/unified_stage_renderer.cpp
        src/win32/lyric_renderer.cpp
    )
    add_library(Kirakara::ShowWin32 ALIAS kirakara_show_win32)
    target_compile_features(kirakara_show_win32 PUBLIC cxx_std_20)
    target_include_directories(kirakara_show_win32
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(kirakara_show_win32
        PUBLIC Kirakara::Show
        PRIVATE d2d1 d3d11 dxgi dwrite windowscodecs ole32 uuid)

    add_executable(kirakara_show_demo WIN32 apps/win32_demo/main.cpp)
    target_compile_features(kirakara_show_demo PRIVATE cxx_std_20)
    target_link_libraries(kirakara_show_demo PRIVATE
        Kirakara::ShowWin32 d2d1 dwrite windowscodecs comdlg32 comctl32
        winmm mf mfplat mfuuid ole32 oleaut32 uuid)
    target_compile_definitions(kirakara_show_demo PRIVATE UNICODE _UNICODE NOMINMAX)
    if(MINGW)
        target_link_options(kirakara_show_demo PRIVATE
            -municode -static -static-libgcc -static-libstdc++)
    endif()
    set_target_properties(kirakara_show_demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    add_executable(kirakara_video_demo WIN32 apps/video_demo/main.cpp)
    target_compile_features(kirakara_video_demo PRIVATE cxx_std_20)
    target_link_libraries(kirakara_video_demo PRIVATE
        Kirakara::ShowWin32 d2d1 d3d11 dxgi dwmapi mf mfplat mfuuid ole32
        oleaut32 uuid shell32 shlwapi)
    target_compile_definitions(kirakara_video_demo PRIVATE UNICODE _UNICODE NOMINMAX)
    if(MINGW)
        target_link_options(kirakara_video_demo PRIVATE
            -municode -static -static-libgcc -static-libstdc++)
    endif()
    set_target_properties(kirakara_video_demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    if(KIRAKARA_BUILD_TESTS)
        add_executable(kirakara_show_d3d11_cast_capabilities_probe
            tests/d3d11_cast_capabilities_probe.cpp
            apps/show_host/cast/d3d11_cast_capabilities.cpp)
        target_compile_features(kirakara_show_d3d11_cast_capabilities_probe
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_d3d11_cast_capabilities_probe
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_d3d11_cast_capabilities_probe
            PRIVATE d3d11 dxgi mf mfplat mfreadwrite mfuuid ole32 shlwapi uuid)
        if(MINGW)
            target_link_options(kirakara_show_d3d11_cast_capabilities_probe
                PRIVATE -municode -static -static-libgcc -static-libstdc++)
        endif()

        add_executable(kirakara_show_cast_nv12_surface_pool_tests
            tests/cast_nv12_surface_pool_tests.cpp
            apps/show_host/cast/cast_nv12_surface_pool.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/cast/d3d11_cast_capabilities.cpp)
        target_compile_features(kirakara_show_cast_nv12_surface_pool_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_cast_nv12_surface_pool_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_cast_nv12_surface_pool_tests
            PRIVATE d3d11 dxgi mf mfplat mfreadwrite mfuuid ole32 shlwapi uuid)
        add_test(NAME kirakara_show_cast_nv12_surface_pool_tests
            COMMAND kirakara_show_cast_nv12_surface_pool_tests)

        add_executable(kirakara_show_cast_video_frame_normalizer_tests
            tests/cast_video_frame_normalizer_tests.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/cast/cast_nv12_surface_pool.cpp
            apps/show_host/cast/cast_video_frame_normalizer.cpp)
        target_compile_features(kirakara_show_cast_video_frame_normalizer_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(
            kirakara_show_cast_video_frame_normalizer_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_cast_video_frame_normalizer_tests
            PRIVATE d3d11)
        if(MINGW)
            target_link_options(
                kirakara_show_cast_video_frame_normalizer_tests
                PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME kirakara_show_cast_video_frame_normalizer_tests
            COMMAND kirakara_show_cast_video_frame_normalizer_tests)

        add_executable(kirakara_show_cast_overlay_renderer_tests
            tests/cast_overlay_renderer_tests.cpp
            apps/show_host/cast/cast_overlay_renderer.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp)
        target_compile_features(kirakara_show_cast_overlay_renderer_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_cast_overlay_renderer_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_cast_overlay_renderer_tests
            PRIVATE Kirakara::ShowWin32 d2d1 d3d11 ole32)
        if(MINGW)
            target_link_options(kirakara_show_cast_overlay_renderer_tests
                PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME kirakara_show_cast_overlay_renderer_tests
            COMMAND kirakara_show_cast_overlay_renderer_tests)

        add_executable(kirakara_show_d3d11_nv12_overlay_compositor_tests
            tests/d3d11_nv12_overlay_compositor_tests.cpp
            apps/show_host/cast/cast_nv12_surface_pool.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/cast/d3d11_cast_capabilities.cpp
            apps/show_host/cast/d3d11_nv12_overlay_compositor.cpp)
        target_compile_features(
            kirakara_show_d3d11_nv12_overlay_compositor_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(
            kirakara_show_d3d11_nv12_overlay_compositor_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(
            kirakara_show_d3d11_nv12_overlay_compositor_tests
            PRIVATE Kirakara::ShowWin32 d2d1 d3d11 dxgi mf mfplat
                mfreadwrite mfuuid ole32 shlwapi uuid)
        if(MINGW)
            target_link_options(
                kirakara_show_d3d11_nv12_overlay_compositor_tests
                PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME kirakara_show_d3d11_nv12_overlay_compositor_tests
            COMMAND kirakara_show_d3d11_nv12_overlay_compositor_tests)

        add_executable(kirakara_show_offscreen_tests
            tests/offscreen_surface_tests.cpp)
        target_link_libraries(kirakara_show_offscreen_tests
            PRIVATE Kirakara::ShowWin32 d2d1)
        add_test(NAME kirakara_show_offscreen_tests
            COMMAND kirakara_show_offscreen_tests)

        add_executable(kirakara_show_lyric_renderer_tests
            tests/lyric_renderer_tests.cpp)
        target_link_libraries(kirakara_show_lyric_renderer_tests
            PRIVATE Kirakara::ShowWin32 d2d1 ole32)
        add_test(NAME kirakara_show_lyric_renderer_tests
            COMMAND kirakara_show_lyric_renderer_tests)

        add_executable(kirakara_show_song_title_renderer_tests
            tests/song_title_renderer_tests.cpp)
        target_link_libraries(kirakara_show_song_title_renderer_tests
            PRIVATE Kirakara::ShowWin32 d2d1 ole32)
        add_test(NAME kirakara_show_song_title_renderer_tests
            COMMAND kirakara_show_song_title_renderer_tests)

        add_executable(kirakara_show_stage_compositor_tests
            tests/stage_compositor_tests.cpp)
        target_link_libraries(kirakara_show_stage_compositor_tests
            PRIVATE Kirakara::ShowWin32 d2d1 d3d11 dxgi)
        add_test(NAME kirakara_show_stage_compositor_tests
            COMMAND kirakara_show_stage_compositor_tests)

        add_executable(kirakara_show_unified_stage_renderer_tests
            tests/unified_stage_renderer_tests.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/host/host_utils.cpp
            apps/show_host/media/mf_d3d11_video_decoder.cpp)
        target_compile_definitions(kirakara_show_unified_stage_renderer_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_unified_stage_renderer_tests
            PRIVATE Kirakara::ShowWin32 d2d1 d3d11 dxgi mf mfplat mfreadwrite
                mfuuid ole32 shlwapi uuid)
        if(MINGW)
            target_link_options(kirakara_show_unified_stage_renderer_tests
                PRIVATE -municode -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME kirakara_show_unified_stage_renderer_tests
            COMMAND kirakara_show_unified_stage_renderer_tests)

        add_executable(kirakara_show_native_stage_pipeline_diagnostics_tests
            tests/native_stage_pipeline_diagnostics_tests.cpp
            apps/show_host/stage/native_stage_pipeline_diagnostics.cpp)
        target_compile_features(
            kirakara_show_native_stage_pipeline_diagnostics_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(
            kirakara_show_native_stage_pipeline_diagnostics_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        add_test(NAME kirakara_show_native_stage_pipeline_diagnostics_tests
            COMMAND kirakara_show_native_stage_pipeline_diagnostics_tests)

        add_executable(kirakara_show_stage_texture_publication_cadence_tests
            tests/stage_texture_publication_cadence_tests.cpp)
        target_compile_features(
            kirakara_show_stage_texture_publication_cadence_tests
            PRIVATE cxx_std_20)
        add_test(NAME kirakara_show_stage_texture_publication_cadence_tests
            COMMAND kirakara_show_stage_texture_publication_cadence_tests)

        add_executable(kirakara_show_native_stage_frame_scheduler_tests
            tests/native_stage_frame_scheduler_tests.cpp)
        target_compile_features(
            kirakara_show_native_stage_frame_scheduler_tests
            PRIVATE cxx_std_20)
        add_test(NAME kirakara_show_native_stage_frame_scheduler_tests
            COMMAND kirakara_show_native_stage_frame_scheduler_tests)

        add_executable(kirakara_show_stage_window_presenter_tests
            tests/stage_window_presenter_tests.cpp)
        target_compile_definitions(kirakara_show_stage_window_presenter_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_stage_window_presenter_tests
            PRIVATE Kirakara::ShowWin32 d2d1 d3d11 dxgi ole32 uuid)
        if(MINGW)
            target_link_options(kirakara_show_stage_window_presenter_tests
                PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME kirakara_show_stage_window_presenter_tests
            COMMAND kirakara_show_stage_window_presenter_tests)

        add_executable(kirakara_show_cast_http_server_tests
            tests/cast_http_server_tests.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/cast/cast_http_server.cpp
            apps/show_host/cast/mpeg_ts_muxer.cpp
            apps/show_host/cast/mpeg_ts_stream_buffer.cpp
            apps/show_host/cast/native_cast_stream.cpp)
        target_compile_features(kirakara_show_cast_http_server_tests
            PRIVATE cxx_std_20)
        target_link_libraries(kirakara_show_cast_http_server_tests
            PRIVATE ws2_32)
        add_test(NAME kirakara_show_cast_http_server_tests
            COMMAND kirakara_show_cast_http_server_tests)

        add_executable(kirakara_show_native_cast_backend_tests
            tests/native_cast_backend_tests.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/cast/cast_http_server.cpp
            apps/show_host/cast/mpeg_ts_muxer.cpp
            apps/show_host/cast/mpeg_ts_stream_buffer.cpp
            apps/show_host/cast/native_cast_backend.cpp
            apps/show_host/cast/native_cast_stream.cpp)
        target_compile_features(kirakara_show_native_cast_backend_tests
            PRIVATE cxx_std_20)
        target_link_libraries(kirakara_show_native_cast_backend_tests
            PRIVATE ws2_32)
        add_test(NAME kirakara_show_native_cast_backend_tests
            COMMAND kirakara_show_native_cast_backend_tests)

        add_executable(kirakara_show_mf_d3d11_video_decoder_tests
            tests/mf_d3d11_video_decoder_tests.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/cast/cast_nv12_surface_pool.cpp
            apps/show_host/cast/cast_stage_renderer.cpp
            apps/show_host/cast/cast_video_frame_normalizer.cpp
            apps/show_host/cast/d3d11_cast_capabilities.cpp
            apps/show_host/host/host_utils.cpp
            apps/show_host/media/mf_d3d11_video_decoder.cpp)
        target_compile_features(kirakara_show_mf_d3d11_video_decoder_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_mf_d3d11_video_decoder_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_mf_d3d11_video_decoder_tests
            PRIVATE Kirakara::ShowWin32 d3d11 mf mfplat mfreadwrite mfuuid
                ole32 shlwapi uuid)
        if(MINGW)
            target_link_options(kirakara_show_mf_d3d11_video_decoder_tests
                PRIVATE -municode -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME kirakara_show_mf_d3d11_video_decoder_tests
            COMMAND kirakara_show_mf_d3d11_video_decoder_tests)

        add_executable(kirakara_show_mf_h264_encoder_probe
            tests/mf_h264_encoder_probe.cpp
            apps/show_host/cast/cast_nv12_surface_pool.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/cast/cast_stage_renderer.cpp
            apps/show_host/cast/d3d11_bgra_to_nv12.cpp
            apps/show_host/cast/mf_d3d11_h264_encoder.cpp)
        target_compile_features(kirakara_show_mf_h264_encoder_probe
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_mf_h264_encoder_probe
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_mf_h264_encoder_probe
            PRIVATE Kirakara::ShowWin32 d3d11 mf mfplat mfreadwrite mfuuid
                ole32 uuid)
        if(MINGW)
            target_link_options(kirakara_show_mf_h264_encoder_probe
                PRIVATE -municode -static -static-libgcc -static-libstdc++)
        endif()

        add_executable(kirakara_show_mf_software_h264_encoder_tests
            tests/mf_software_h264_encoder_tests.cpp
            apps/show_host/cast/cast_nv12_surface_pool.cpp
            apps/show_host/cast/cast_pipeline_diagnostics.cpp
            apps/show_host/cast/cast_stage_renderer.cpp
            apps/show_host/cast/d3d11_bgra_to_nv12.cpp
            apps/show_host/cast/mpeg_ts_muxer.cpp
            apps/show_host/cast/mf_d3d11_h264_encoder.cpp)
        target_compile_features(kirakara_show_mf_software_h264_encoder_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_mf_software_h264_encoder_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_mf_software_h264_encoder_tests
            PRIVATE Kirakara::ShowWin32 d3d11 mf mfplat mfreadwrite mfuuid
                ole32 uuid)
        if(MINGW)
            target_link_options(kirakara_show_mf_software_h264_encoder_tests
                PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME kirakara_show_mf_software_h264_encoder_tests
            COMMAND kirakara_show_mf_software_h264_encoder_tests)

        add_executable(kirakara_show_mf_aac_encoder_tests
            tests/mf_aac_encoder_tests.cpp
            apps/show_host/cast/aac_adts_packetizer.cpp
            apps/show_host/cast/mf_aac_encoder.cpp
            apps/show_host/host/host_utils.cpp)
        target_compile_features(kirakara_show_mf_aac_encoder_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_mf_aac_encoder_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_mf_aac_encoder_tests
            PRIVATE mf mfplat mfuuid ole32 shlwapi)
        add_test(NAME kirakara_show_mf_aac_encoder_tests
            COMMAND kirakara_show_mf_aac_encoder_tests)

        add_test(NAME kirakara_show_render_smoke
            COMMAND kirakara_show_demo
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dom-parity.lrc"
                --time=2.5
                --font-bold=0
                --ruby-bold=1
                --ruby2-bold=1
                "--export-frame=${CMAKE_BINARY_DIR}/offscreen-render-smoke.png"
                --exit-after-export)
        set_tests_properties(kirakara_show_render_smoke PROPERTIES
            FIXTURES_SETUP kirakara_rendered_frame)

        add_executable(kirakara_show_rendered_frame_tests
            tests/rendered_frame_tests.cpp)
        target_link_libraries(kirakara_show_rendered_frame_tests
            PRIVATE windowscodecs ole32 uuid)
        target_compile_definitions(kirakara_show_rendered_frame_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        if(MINGW)
            target_link_options(kirakara_show_rendered_frame_tests PRIVATE
                -municode -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME kirakara_show_rendered_frame_tests
            COMMAND kirakara_show_rendered_frame_tests
                "${CMAKE_BINARY_DIR}/offscreen-render-smoke.png")
        set_tests_properties(kirakara_show_rendered_frame_tests PROPERTIES
            FIXTURES_REQUIRED kirakara_rendered_frame)
    endif()

    # ── show_host DLL for Flutter embedding ──
    add_library(show_host SHARED
        apps/show_host/show_host_api.cpp
        apps/show_host/host/host_utils.cpp
        apps/show_host/host/host_layout.cpp
        apps/show_host/host/host_program.cpp
        apps/show_host/host/host_output.cpp
        apps/show_host/host/host_render.cpp
        apps/show_host/host/host_windows.cpp
        apps/show_host/host/host_commands.cpp
        apps/show_host/stage/native_stage_session.cpp
        apps/show_host/stage/native_stage_pipeline_diagnostics.cpp
        apps/show_host/stage/native_stage_trace.cpp
        apps/show_host/cast/cast_session.cpp
        apps/show_host/cast/cast_session_lifecycle.cpp
        apps/show_host/audio/audio_decode.cpp
        apps/show_host/audio/directshow_backend.cpp
        apps/show_host/audio/directsound_backend.cpp
        apps/show_host/cast/aac_adts_packetizer.cpp
        apps/show_host/cast/cast_pipeline_diagnostics.cpp
        apps/show_host/cast/cast_program_clock.cpp
        apps/show_host/cast/cast_pcm_resampler.cpp
        apps/show_host/cast/cast_nv12_surface_pool.cpp
        apps/show_host/cast/cast_overlay_renderer.cpp
        apps/show_host/cast/cast_video_frame_normalizer.cpp
        apps/show_host/cast/cast_video_buffer_state.cpp
        apps/show_host/cast/d3d11_cast_capabilities.cpp
        apps/show_host/cast/cast_stage_renderer.cpp
        apps/show_host/cast/d3d11_bgra_to_nv12.cpp
        apps/show_host/cast/d3d11_nv12_overlay_compositor.cpp
        apps/show_host/cast/mf_d3d11_h264_encoder.cpp
        apps/show_host/cast/mf_aac_encoder.cpp
        apps/show_host/cast/cast_http_server.cpp
        apps/show_host/cast/mpeg_ts_muxer.cpp
        apps/show_host/cast/mpeg_ts_stream_buffer.cpp
        apps/show_host/cast/native_cast_backend.cpp
        apps/show_host/cast/native_cast_stream.cpp
        apps/show_host/media/mf_d3d11_video_decoder.cpp
    )
    target_compile_features(show_host PRIVATE cxx_std_20)
    target_compile_definitions(show_host PRIVATE
        SHOW_HOST_EXPORTS UNICODE _UNICODE NOMINMAX)
    target_include_directories(show_host
        PRIVATE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch>)
    target_link_libraries(show_host PRIVATE
        Kirakara::ShowWin32 d2d1 d3d11 dxgi dwrite mf mfplat mfreadwrite mfuuid
        advapi32 ole32 oleaut32 quartz shlwapi strmiids uuid winmm dsound
        ws2_32)
    if(MINGW)
        target_link_options(show_host PRIVATE
            -static -static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL)
    endif()
    set_target_properties(show_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    if(KIRAKARA_BUILD_TESTS)
        add_executable(kirakara_show_unified_cast_smoke
            tests/show_host_unified_cast_smoke.cpp)
        target_compile_features(kirakara_show_unified_cast_smoke
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_unified_cast_smoke
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_unified_cast_smoke
            PRIVATE show_host ws2_32 psapi)
        if(MINGW)
            target_link_options(kirakara_show_unified_cast_smoke
                PRIVATE -municode -static -static-libgcc -static-libstdc++)
        endif()
        set_target_properties(kirakara_show_unified_cast_smoke PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

        add_executable(kirakara_show_unified_native_smoke
            tests/show_host_unified_native_smoke.cpp)
        target_compile_features(kirakara_show_unified_native_smoke
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_unified_native_smoke
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_unified_native_smoke
            PRIVATE show_host d3d11 psapi)
        if(MINGW)
            target_link_options(kirakara_show_unified_native_smoke PRIVATE
                -municode -static -static-libgcc -static-libstdc++)
        endif()
        set_target_properties(kirakara_show_unified_native_smoke PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

        add_executable(kirakara_show_stage_visual_smoke
            tests/show_host_stage_visual_smoke.cpp)
        target_compile_features(kirakara_show_stage_visual_smoke
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_stage_visual_smoke
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_stage_visual_smoke
            PRIVATE show_host d3d11 dxgi)
        if(MINGW)
            target_link_options(kirakara_show_stage_visual_smoke PRIVATE
                -municode -static -static-libgcc -static-libstdc++)
        endif()
        set_target_properties(kirakara_show_stage_visual_smoke PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

        # Headless host-contract checks: playback command semantics, the output
        # lease side effects and the runtime half of the C ABI (struct sizes).
        # No media, no window and no GPU required, so this runs everywhere.
        add_executable(kirakara_show_host_lifecycle_tests
            tests/show_host_lifecycle_tests.cpp)
        target_compile_features(kirakara_show_host_lifecycle_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(kirakara_show_host_lifecycle_tests
            PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(kirakara_show_host_lifecycle_tests
            PRIVATE show_host)
        if(MINGW)
            target_link_options(kirakara_show_host_lifecycle_tests PRIVATE
                -static -static-libgcc -static-libstdc++)
        endif()
        set_target_properties(kirakara_show_host_lifecycle_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        add_test(NAME kirakara_show_host_lifecycle_tests
            COMMAND kirakara_show_host_lifecycle_tests)

        # The exported symbol set is the Flutter FFI contract. Refactors that
        # move host code between translation units must not change it, so every
        # test run compares the built DLL against a committed baseline.
        # Regenerate intentionally with tools/show_host_abi.ps1 -Update.
        find_program(KIRAKARA_POWERSHELL NAMES powershell.exe)
        if(WIN32 AND KIRAKARA_POWERSHELL)
            if(CMAKE_OBJDUMP)
                set(KIRAKARA_ABI_OBJDUMP "${CMAKE_OBJDUMP}")
            else()
                set(KIRAKARA_ABI_OBJDUMP "objdump")
            endif()
            add_test(NAME kirakara_show_host_abi
                COMMAND "${KIRAKARA_POWERSHELL}"
                    -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/show_host_abi.ps1"
                    -Dll "$<TARGET_FILE:show_host>"
                    -Baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/abi/show_host_exports.txt"
                    -Objdump "${KIRAKARA_ABI_OBJDUMP}")
        endif()
    endif()
endif()
