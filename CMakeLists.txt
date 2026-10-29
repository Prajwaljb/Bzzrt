cmake_minimum_required(VERSION 3.16)
project(vrrdiag VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

set(COMMON_SOURCES
  src/common/app.cpp
  src/common/guided.cpp
  src/common/clock.cpp
  src/common/config.cpp
  src/common/csv_log.cpp
  src/common/edid.cpp
  src/common/image_load.cpp
  src/common/overlay.cpp
  src/common/patterns.cpp
  src/common/range_store.cpp
  src/common/scheduler.cpp
  src/common/stats.cpp
)

if(WIN32)
  # ---------------------------------------------------------------- Windows
  option(VRRDIAG_D3D11 "Build the optional Direct3D 11 fallback backend" ON)

  # Embed the HLSL source; it is compiled at runtime with D3DCompile (d3dcompiler_47.dll ships with Windows).
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pattern.hlsl HLSL_SOURCE)
  file(WRITE ${GEN_DIR}/pattern_hlsl.h.tmp
    "// Generated from shaders/pattern.hlsl\n#pragma once\nstatic const char kPatternHlsl[] = R\"HLSL(${HLSL_SOURCE})HLSL\";\n")
  configure_file(${GEN_DIR}/pattern_hlsl.h.tmp ${GEN_DIR}/pattern_hlsl.h COPYONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pattern.hlsl)

  set(WIN_SOURCES
    src/win/main_win.cpp
    src/win/win_platform.cpp
    src/win/dxgi_backend_base.cpp
    src/win/d3d12_backend.cpp
  )
  if(VRRDIAG_D3D11)
    list(APPEND WIN_SOURCES src/win/d3d11_backend.cpp)
  endif()

  add_executable(vrrdiag ${COMMON_SOURCES} ${WIN_SOURCES} packaging/windows/bzzrt.rc)
  # The include path also lets the resource compiler find bzzrt.ico next to bzzrt.rc.
  target_include_directories(vrrdiag PRIVATE ${GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows)
  target_compile_definitions(vrrdiag PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(vrrdiag PRIVATE d3d12 dxgi d3dcompiler dxguid winmm avrt setupapi)
  if(VRRDIAG_D3D11)
    target_compile_definitions(vrrdiag PRIVATE VRRDIAG_HAVE_D3D11)
    target_link_libraries(vrrdiag PRIVATE d3d11)
  endif()
  if(MSVC)
    target_compile_options(vrrdiag PRIVATE /W3 /permissive- /utf-8 /MP)
    # Static CRT: the .exe runs without the VC++ redistributable.
    set_property(TARGET vrrdiag PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  elseif(MINGW)
    target_compile_options(vrrdiag PRIVATE -Wall -Wextra)
    # Static runtime: the .exe runs without libstdc++/libc++ or winpthread DLLs next to it.
    target_link_options(vrrdiag PRIVATE -static -static-libgcc -static-libstdc++)
  endif()

elseif(UNIX AND NOT APPLE)
  # ------------------------------------------------------------------ Linux
  find_package(Vulkan REQUIRED)
  find_package(PkgConfig REQUIRED)
  find_package(Threads REQUIRED)

  option(VRRDIAG_WAYLAND "Wayland WSI support" ON)
  option(VRRDIAG_X11 "X11 (XCB) WSI support" ON)
  option(VRRDIAG_KMS "Direct DRM/KMS backend" ON)

  set(LINUX_SOURCES
    src/linux/main_linux.cpp
    src/linux/vk_backend.cpp
    src/linux/evdev_keys.cpp
    src/linux/sysfs_edid.cpp
  )
  set(LINUX_DEFS "")
  set(LINUX_LIBS Vulkan::Vulkan Threads::Threads)
  set(LINUX_INCS ${GEN_DIR})

  # SPIR-V shaders: compile if glslangValidator is available, otherwise use the checked-in headers.
  find_program(GLSLANG_VALIDATOR glslangValidator HINTS $ENV{VULKAN_SDK}/bin)
  if(GLSLANG_VALIDATOR)
    foreach(stage vert frag)
      add_custom_command(
        OUTPUT ${GEN_DIR}/pattern_${stage}.h
        COMMAND ${GLSLANG_VALIDATOR} -V --vn kPattern$<IF:$<STREQUAL:${stage},vert>,Vert,Frag>Spv
                -o ${GEN_DIR}/pattern_${stage}.h ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pattern.${stage}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pattern.${stage}
        COMMENT "Compiling pattern.${stage} to SPIR-V")
      list(APPEND LINUX_SOURCES ${GEN_DIR}/pattern_${stage}.h)
    endforeach()
  else()
    message(STATUS "glslangValidator not found: using prebuilt SPIR-V from shaders/prebuilt")
    list(APPEND LINUX_INCS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/prebuilt)
  endif()

  if(VRRDIAG_WAYLAND)
    pkg_check_modules(WAYLAND_CLIENT wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)
    find_program(WAYLAND_SCANNER wayland-scanner)
    if(WAYLAND_CLIENT_FOUND AND WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER)
      pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
      macro(wayland_protocol xml base)
        add_custom_command(
          OUTPUT ${GEN_DIR}/${base}-client-protocol.h ${GEN_DIR}/${base}-protocol.c
          COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${GEN_DIR}/${base}-client-protocol.h
          COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${GEN_DIR}/${base}-protocol.c
          DEPENDS ${xml})
        list(APPEND LINUX_SOURCES ${GEN_DIR}/${base}-client-protocol.h ${GEN_DIR}/${base}-protocol.c)
      endmacro()
      wayland_protocol(${WL_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml xdg-shell)
      set(CONTENT_TYPE_XML ${WL_PROTOCOLS_DIR}/staging/content-type/content-type-v1.xml)
      if(EXISTS ${CONTENT_TYPE_XML})
        wayland_protocol(${CONTENT_TYPE_XML} content-type-v1)
        list(APPEND LINUX_DEFS VRRDIAG_HAVE_CONTENT_TYPE)
      endif()
      list(APPEND LINUX_SOURCES src/linux/wayland_window.cpp)
      list(APPEND LINUX_DEFS VRRDIAG_HAVE_WAYLAND VK_USE_PLATFORM_WAYLAND_KHR)
      list(APPEND LINUX_LIBS ${WAYLAND_CLIENT_LIBRARIES})
      list(APPEND LINUX_INCS ${WAYLAND_CLIENT_INCLUDE_DIRS})
      message(STATUS "Wayland support: ON")
    else()
      message(WARNING "Wayland support disabled (need wayland-client, wayland-protocols, wayland-scanner)")
    endif()
  endif()

  if(VRRDIAG_X11)
    pkg_check_modules(XCB xcb)
    pkg_check_modules(XCB_RANDR xcb-randr)
    if(XCB_FOUND)
      list(APPEND LINUX_SOURCES src/linux/x11_window.cpp)
      list(APPEND LINUX_DEFS VRRDIAG_HAVE_X11 VK_USE_PLATFORM_XCB_KHR)
      list(APPEND LINUX_LIBS ${XCB_LIBRARIES})
      list(APPEND LINUX_INCS ${XCB_INCLUDE_DIRS})
      if(XCB_RANDR_FOUND)
        list(APPEND LINUX_DEFS VRRDIAG_HAVE_XCB_RANDR)
        list(APPEND LINUX_LIBS ${XCB_RANDR_LIBRARIES})
      endif()
      message(STATUS "X11 support: ON (RandR: ${XCB_RANDR_FOUND})")
    else()
      message(WARNING "X11 support disabled (need xcb)")
    endif()
  endif()

  if(VRRDIAG_KMS)
    pkg_check_modules(DRM libdrm)
    if(DRM_FOUND)
      list(APPEND LINUX_SOURCES src/linux/kms_backend.cpp src/linux/tty_input.cpp)
      list(APPEND LINUX_DEFS VRRDIAG_HAVE_KMS)
      list(APPEND LINUX_LIBS ${DRM_LIBRARIES})
      list(APPEND LINUX_INCS ${DRM_INCLUDE_DIRS})
      message(STATUS "DRM/KMS backend: ON")
    else()
      message(WARNING "DRM/KMS backend disabled (need libdrm)")
    endif()
  endif()

  if(NOT "VRRDIAG_HAVE_WAYLAND" IN_LIST LINUX_DEFS AND NOT "VRRDIAG_HAVE_X11" IN_LIST LINUX_DEFS)
    message(FATAL_ERROR "Neither Wayland nor X11 development files were found.")
  endif()

  add_executable(vrrdiag ${COMMON_SOURCES} ${LINUX_SOURCES})
  target_compile_definitions(vrrdiag PRIVATE ${LINUX_DEFS})
  target_include_directories(vrrdiag PRIVATE ${LINUX_INCS})
  target_link_libraries(vrrdiag PRIVATE ${LINUX_LIBS})
  target_compile_options(vrrdiag PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
elseif(APPLE)
  # ------------------------------------------------------------------ macOS
  enable_language(OBJCXX)
  # Embed the Metal source; it is compiled at runtime with newLibraryWithSource.
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pattern.metal METAL_SOURCE)
  file(WRITE ${GEN_DIR}/pattern_metal.h.tmp
    "// Generated from shaders/pattern.metal\n#pragma once\nstatic const char kPatternMetal[] = R\"METAL(${METAL_SOURCE})METAL\";\n")
  configure_file(${GEN_DIR}/pattern_metal.h.tmp ${GEN_DIR}/pattern_metal.h COPYONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pattern.metal)

  set(MAC_SOURCES
    src/mac/main_mac.mm
    src/mac/mac_platform.mm
    src/mac/metal_backend.mm
  )
  set(MAC_LIBS "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework CoreGraphics")
  add_executable(vrrdiag ${COMMON_SOURCES} ${MAC_SOURCES})
  target_include_directories(vrrdiag PRIVATE ${GEN_DIR})
  target_link_libraries(vrrdiag PRIVATE ${MAC_LIBS})
  target_compile_options(vrrdiag PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
                         $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

  # Compiles the embedded Metal source on this machine's GPU (used by CI to catch shader errors).
  add_executable(metal_shader_check tools/metal_shader_check.mm)
  target_include_directories(metal_shader_check PRIVATE ${GEN_DIR})
  target_link_libraries(metal_shader_check PRIVATE "-framework Foundation" "-framework Metal")
  target_compile_options(metal_shader_check PRIVATE -fobjc-arc)
else()
  message(FATAL_ERROR "Only Windows, Linux and macOS are supported.")
endif()

install(TARGETS vrrdiag RUNTIME DESTINATION bin)
