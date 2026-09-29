# Native build of SharedWorldCore + unit tests. The same sources are compiled
# by Unreal as the "SharedWorldCore" module; this build proves they are
# UE-independent and lets the core be tested without the game.
cmake_minimum_required(VERSION 3.20)
project(SharedWorldCoreTests CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SW_SANITIZE "address|undefined|thread|" "")
option(SW_CHAOS_FULL "Build the large overnight chaos suite" OFF)

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../SharedWorld/Source/SharedWorldCore)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${CORE_DIR}/Private/*.cpp)
# The Unreal module entry point is the only UE-dependent file.
list(FILTER CORE_SOURCES EXCLUDE REGEX "SharedWorldCoreModule\\.cpp$")
# ZstdBundle.cpp is for the Unreal module only (vendored .c unity build).
list(FILTER CORE_SOURCES EXCLUDE REGEX "ZstdBundle\\.cpp$")

set(SW_ZLIB_FETCHED FALSE)
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  include(FetchContent)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
  )
  FetchContent_MakeAvailable(zlib)
  set(SW_ZLIB_FETCHED TRUE)
  # Prefer the static library so sw_tests.exe does not need zlibd.dll beside it.
  if(TARGET zlibstatic)
    if(NOT TARGET ZLIB::ZLIB)
      add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
  elseif(TARGET zlib)
    if(NOT TARGET ZLIB::ZLIB)
      add_library(ZLIB::ZLIB ALIAS zlib)
    endif()
  endif()
endif()
find_package(Threads REQUIRED)

# Vendored zstd (lib only — common/compress/decompress).
set(ZSTD_LIB_DIR ${CORE_DIR}/ThirdParty/zstd/lib)
file(GLOB ZSTD_SOURCES CONFIGURE_DEPENDS
  ${ZSTD_LIB_DIR}/common/*.c
  ${ZSTD_LIB_DIR}/compress/*.c
  ${ZSTD_LIB_DIR}/decompress/*.c
)
add_library(sw_zstd STATIC ${ZSTD_SOURCES})
target_include_directories(sw_zstd PUBLIC ${ZSTD_LIB_DIR})
target_compile_definitions(sw_zstd PRIVATE ZSTD_MULTITHREAD=0 XXH_NAMESPACE=ZSTD_ ZSTD_DISABLE_ASM)
if(MSVC)
  target_compile_options(sw_zstd PRIVATE /W3 /wd4996)
else()
  target_compile_options(sw_zstd PRIVATE -w)
endif()

# Match Unreal: no exceptions, no RTTI, strict warnings (shadowing is an
# error in Unreal builds too).
if(MSVC)
  # C4100 unreferenced parameter and C4127 constant conditional are noise here.
  set(SW_FLAGS /W4 /WX /GR- /EHs-c- /D_HAS_EXCEPTIONS=0 /utf-8 /permissive- /wd4100 /wd4127
      /w14456 /w14457 /w14458 /w14459 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX)
else()
  set(SW_FLAGS -fno-exceptions -fno-rtti -Wall -Wextra -Wshadow -Werror -Wno-missing-field-initializers)
endif()
if(SW_SANITIZE)
  list(APPEND SW_FLAGS -fsanitize=${SW_SANITIZE} -fno-omit-frame-pointer -g)
  set(SW_LINK -fsanitize=${SW_SANITIZE})
endif()

add_library(SharedWorldCore STATIC ${CORE_SOURCES})
target_include_directories(SharedWorldCore PUBLIC ${CORE_DIR}/Public ${ZSTD_LIB_DIR})
if(SW_ZLIB_FETCHED)
  target_include_directories(SharedWorldCore PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
endif()
target_compile_options(SharedWorldCore PRIVATE ${SW_FLAGS})
target_link_libraries(SharedWorldCore PUBLIC ZLIB::ZLIB sw_zstd Threads::Threads ${SW_LINK})

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(sw_tests ${TEST_SOURCES})
target_compile_options(sw_tests PRIVATE ${SW_FLAGS})
target_link_libraries(sw_tests PRIVATE SharedWorldCore ${SW_LINK})
target_compile_definitions(sw_tests PRIVATE SW_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
if(SW_CHAOS_FULL)
  target_compile_definitions(sw_tests PRIVATE SW_CHAOS_FULL=1)
endif()
# When zlib is a shared DLL (FetchContent default on Windows), place it next to sw_tests.
if(WIN32 AND TARGET zlib)
  add_custom_command(TARGET sw_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:zlib> $<TARGET_FILE_DIR:sw_tests>)
endif()

add_executable(sw_compress_bench ${CMAKE_CURRENT_SOURCE_DIR}/tools/compress_bench.cpp)
target_compile_options(sw_compress_bench PRIVATE ${SW_FLAGS})
target_link_libraries(sw_compress_bench PRIVATE SharedWorldCore ${SW_LINK})
target_compile_definitions(sw_compress_bench PRIVATE SW_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
if(WIN32 AND TARGET zlib)
  add_custom_command(TARGET sw_compress_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:zlib> $<TARGET_FILE_DIR:sw_compress_bench>)
endif()

enable_testing()
add_test(NAME sw_tests COMMAND sw_tests)
add_test(NAME sw_tests_host_migration COMMAND sw_tests HostElection Migration Chaos_Small)
add_test(NAME sw_compress_tests COMMAND sw_tests Compression)
