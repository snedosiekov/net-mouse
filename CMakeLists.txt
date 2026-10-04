cmake_minimum_required(VERSION 3.20)
project(net-mouse VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # Static runtime: a single .exe can be copied to every PC without installing redistributables.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /utf-8 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

if(WIN32)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
endif()

# Platform-independent core: protocol, layout, config, networking and the master/slave engines.
add_library(nm_core STATIC
  src/core/protocol.cpp
  src/core/layout.cpp
  src/core/config.cpp
  src/core/log.cpp
  src/core/udp.cpp
  src/app/master_engine.cpp
  src/app/slave_engine.cpp
  src/app/network_loop.cpp
)
target_include_directories(nm_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(nm_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(nm_core PUBLIC ws2_32 iphlpapi)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Service for both roles: evdev capture + uinput injection, independent of X11/Wayland.
  add_executable(net-mouse-daemon
    src/platform/linux/uinput_device.cpp
    src/platform/linux/linux_platform.cpp
    src/linux/main_linux.cpp
  )
  target_link_libraries(net-mouse-daemon PRIVATE nm_core)
  install(TARGETS net-mouse-daemon RUNTIME DESTINATION bin)
endif()

if(WIN32)
  enable_language(RC)
  configure_file(src/ui/win/resources.rc.in ${CMAKE_CURRENT_BINARY_DIR}/resources.rc @ONLY)
  add_executable(net-mouse WIN32
    src/platform/win/win_platform.cpp
    src/ui/win/layout_view.cpp
    src/ui/win/app_win.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/resources.rc
  )
  target_link_libraries(net-mouse PRIVATE nm_core comctl32 shell32 user32 gdi32)

  # Installer: cmake --build build --config Release --target installer
  find_program(ISCC_EXECUTABLE ISCC
    PATHS "$ENV{ProgramFiles\(x86\)}/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6"
          "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6")
  if(ISCC_EXECUTABLE)
    add_custom_target(installer
      COMMAND "${ISCC_EXECUTABLE}" /Q
              "/DAppVersion=${PROJECT_VERSION}"
              "/DBinDir=$<TARGET_FILE_DIR:net-mouse>"
              "/O${CMAKE_BINARY_DIR}/installer"
              "${CMAKE_SOURCE_DIR}/installer/net-mouse.iss"
      DEPENDS net-mouse
      COMMENT "Building the Inno Setup installer"
      VERBATIM)
  else()
    message(STATUS "Inno Setup (ISCC) not found: the 'installer' target is unavailable")
  endif()
endif()

enable_testing()
add_executable(nm_tests tests/core_tests.cpp)
target_link_libraries(nm_tests PRIVATE nm_core)
add_test(NAME nm_tests COMMAND nm_tests)
