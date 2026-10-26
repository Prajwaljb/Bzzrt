# Cross-compile the Windows .exe from Linux with MinGW-w64 GCC or llvm-mingw:
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#   cmake --build build-win
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32 CACHE STRING "MinGW target triple prefix")

find_program(MINGW_CXX NAMES ${TOOLCHAIN_PREFIX}-clang++ ${TOOLCHAIN_PREFIX}-g++ ${TOOLCHAIN_PREFIX}-g++-posix REQUIRED)
find_program(MINGW_CC NAMES ${TOOLCHAIN_PREFIX}-clang ${TOOLCHAIN_PREFIX}-gcc ${TOOLCHAIN_PREFIX}-gcc-posix REQUIRED)
find_program(MINGW_RC NAMES ${TOOLCHAIN_PREFIX}-windres llvm-windres)
set(CMAKE_C_COMPILER ${MINGW_CC})
set(CMAKE_CXX_COMPILER ${MINGW_CXX})
if(MINGW_RC)
  set(CMAKE_RC_COMPILER ${MINGW_RC})
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
