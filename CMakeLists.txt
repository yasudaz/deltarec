cmake_minimum_required(VERSION 3.16)
project(deltarec LANGUAGES CXX VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(deltarec src/main.cpp)
target_include_directories(deltarec PRIVATE src third_party)

find_package(Threads REQUIRED)
target_link_libraries(deltarec PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(deltarec PRIVATE /W3 /O2 /fp:precise /utf-8)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    target_compile_options(deltarec PRIVATE /arch:AVX2)
  endif()
  target_compile_definitions(deltarec PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(deltarec PRIVATE -Wall -Wextra -O3)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    target_compile_options(deltarec PRIVATE -mavx2)
  endif()
endif()

install(TARGETS deltarec RUNTIME DESTINATION bin)
