add_executable(display_settings_tests
    display_settings.cpp
    ../core/shell/displays.cpp
    ../core/settings.cpp
    ../core/log.cpp
    ../core/shell/logon_task.cpp)
target_include_directories(display_settings_tests PRIVATE ../core)
target_compile_definitions(display_settings_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN ATRIUM_DEVELOPMENT)
target_link_libraries(display_settings_tests PRIVATE windowsapp user32 shell32 ole32 oleaut32 taskschd advapi32)
add_test(NAME display_settings COMMAND display_settings_tests)

add_executable(app_processes_tests
    app_processes.cpp
    ../core/shell/app_processes.cpp
    ../core/log.cpp)
target_include_directories(app_processes_tests PRIVATE ../core)
target_compile_definitions(app_processes_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME app_processes COMMAND app_processes_tests)
