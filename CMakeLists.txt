cmake_minimum_required(VERSION 3.20)
project(SuperTerminal VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    # Static CRT: st.exe runs on any Windows 10/11 machine with nothing else installed.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
else()
    add_compile_options(-Wall -Wextra)
endif()
add_compile_definitions(ST_VERSION="${PROJECT_VERSION}")

# --- shell integration scripts, compiled into the binary -------------------
set(ST_SCRIPTS shell/superterminal.ps1 shell/superterminal.bash shell/superterminal.zsh)
string(REPLACE ";" "," ST_SCRIPTS_ARG "${ST_SCRIPTS}")
set(ST_EMBEDDED "${CMAKE_CURRENT_BINARY_DIR}/embedded_scripts.cpp")
add_custom_command(
    OUTPUT "${ST_EMBEDDED}"
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DFILES=${ST_SCRIPTS_ARG}
            -DOUT=${ST_EMBEDDED} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${ST_SCRIPTS} cmake/embed.cmake
    COMMENT "Embedding shell integration scripts"
    VERBATIM)

# --- core ------------------------------------------------------------------
add_library(stcore STATIC
    src/brain.cpp
    src/claude_client.cpp
    src/context.cpp
    src/output_log.cpp
    src/router.cpp
    src/router_posix.cpp
    src/safety.cpp
    src/settings.cpp
    src/scripts.cpp
    src/util.cpp
    "${ST_EMBEDDED}"
)
target_include_directories(stcore PUBLIC src third_party)

if(WIN32)
    target_sources(stcore PRIVATE src/win/conpty_host.cpp src/win/ipc_win.cpp src/win/http_win.cpp)
    target_link_libraries(stcore PUBLIC winhttp advapi32 shell32 ole32)
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    target_sources(stcore PRIVATE src/unix/pty_unix.cpp src/unix/ipc_unix.cpp src/unix/http_curl.cpp)
    target_link_libraries(stcore PUBLIC CURL::libcurl Threads::Threads)
    if(NOT APPLE)
        target_link_libraries(stcore PUBLIC util)  # forkpty
    endif()
endif()

add_executable(st src/main.cpp)
target_link_libraries(st PRIVATE stcore)

if(WIN32)
    # Icon and version details, so Explorer, the Start menu and Installed apps show "SuperTerminal".
    set(ST_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/st.ico")
    configure_file(packaging/windows/st.rc.in "${CMAKE_CURRENT_BINARY_DIR}/st.rc" @ONLY)
    target_sources(st PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/st.rc")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/st.rc" APPEND PROPERTY OBJECT_DEPENDS "${ST_ICON}")
endif()

if(WIN32)
    # End-to-end test: runs st.exe inside its own pseudo console and types at it.
    add_executable(st_e2e tests/e2e.cpp)
    target_link_libraries(st_e2e PRIVATE stcore)
endif()

install(TARGETS st RUNTIME DESTINATION bin)
