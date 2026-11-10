# Atrium: resident process (glass bar, tray icon, settings pipe).

# HLSL is compiled at build time into headers; the runtime needs no compiler.
find_program(ATRIUM_FXC fxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    REQUIRED)

set(shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders)
set(shader_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${shader_out})
set(shader_headers)
function(atrium_shader source profile entry variable header)
    add_custom_command(
        OUTPUT ${shader_out}/${header}
        COMMAND ${ATRIUM_FXC} /nologo /O3 /WX /T ${profile} /E ${entry} /Vn ${variable} /Fh ${shader_out}/${header} ${shader_dir}/${source}
        DEPENDS ${shader_dir}/${source}
        COMMENT "fxc ${source} ${entry}"
        VERBATIM)
    set(shader_headers ${shader_headers} ${shader_out}/${header} PARENT_SCOPE)
endfunction()
atrium_shader(glass.hlsl vs_5_0 VS g_GlassVS glass_vs.h)
atrium_shader(glass.hlsl ps_5_0 PS g_GlassPS glass_ps.h)
atrium_shader(compare.hlsl cs_5_0 CS g_CompareCS compare_cs.h)
atrium_shader(backdrop_edge.hlsl ps_5_0 PS g_BackdropEdgePS backdrop_edge_ps.h)

add_executable(atrium_core WIN32
    main.cpp
    bar.cpp
    log.cpp
    settings.cpp
    settings_server.cpp
    shell/task_model.cpp
    shell/app_catalog.cpp
    shell/app_registration.cpp
    shell/update_checker.cpp
    shell/system_chrome.cpp
    shell/system_status.cpp
    shell/wifi.cpp
    shell/radios.cpp
    shell/lunar.cpp
    shell/audio_devices.cpp
    shell/projection.cpp
    shell/tray_model.cpp
    shell/tray_broker.cpp
    shell/wechat_source.cpp
    shell/feishu_source.cpp
    ui/surface.cpp
    ui/start_menu.cpp
    ui/notifications.cpp
    ui/tray_panel.cpp
    ui/calendar_panel.cpp
    ui/control_center.cpp
    ui/desktop_widgets.cpp
    ui/glass_menu.cpp
    ui/stage_manager.cpp
    ui/window_snapshot.cpp
    shell/weather.cpp
    shell/bluetooth_batteries.cpp
    shell/logon_task.cpp
    ui/window_preview.cpp
    ui/widget_faces.cpp
    shell/notification_listener.cpp
    render/bar_painter.cpp
    render/capture.cpp
    render/desktop_backdrop.cpp
    render/glass_renderer.cpp
    render/glass_panel.cpp
    render/ui_font.cpp
    render/gpu.cpp
    render/wifi_glyph.cpp
    core.rc
    core.manifest
    ${shader_headers})
# build/win-x64/<Config>/Atrium.exe
set_target_properties(atrium_core PROPERTIES OUTPUT_NAME "Atrium" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_include_directories(atrium_core PRIVATE ${shader_out})
# The interface face (D40) next to the exe, as in the package.
set(ui_fonts MiSans-Medium.otf MiSans-Demibold.otf MiSans-Semibold.otf)
list(TRANSFORM ui_fonts PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/fonts/)
add_custom_command(TARGET atrium_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:atrium_core>/Fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ui_fonts} $<TARGET_FILE_DIR:atrium_core>/Fonts
    VERBATIM)
# C++/WinRT headers come with the Windows SDK.
target_compile_options(atrium_core PRIVATE /bigobj)
target_compile_definitions(atrium_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(ATRIUM_DEVELOPMENT)
    target_compile_definitions(atrium_core PRIVATE ATRIUM_DEVELOPMENT)
endif()
target_include_directories(atrium_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(atrium_core PRIVATE
    windowsapp d3d11 dxgi dcomp d2d1 dwrite shcore user32 gdi32 shell32 ole32
    dwmapi version propsys shlwapi oleaut32 uuid imm32 powrprof wlanapi windowscodecs winhttp setupapi taskschd)
