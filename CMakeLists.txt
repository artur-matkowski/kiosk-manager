cmake_minimum_required(VERSION 3.16)
project(kiosk-manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(kiosk-manager
  src/main.cpp
  src/config.cpp
  src/daemon.cpp
  src/cli.cpp
)

target_include_directories(kiosk-manager PRIVATE third_party src)
target_compile_options(kiosk-manager PRIVATE -Wall -Wextra)

# cpp-httplib is header-only; we only use plain HTTP on localhost (no OpenSSL).
find_package(Threads REQUIRED)
target_link_libraries(kiosk-manager PRIVATE Threads::Threads)

install(TARGETS kiosk-manager RUNTIME DESTINATION bin)
