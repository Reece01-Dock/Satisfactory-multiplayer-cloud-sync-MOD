# Native build of SharedWorldCore + unit tests. The same sources are compiled
# by Unreal as the "SharedWorldCore" module; this build proves they are
# UE-independent and lets the core be tested without the game.
cmake_minimum_required(VERSION 3.20)
project(SharedWorldCoreTests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SW_SANITIZE "address|undefined|thread|" "")

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../SharedWorld/Source/SharedWorldCore)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${CORE_DIR}/Private/*.cpp)
# The Unreal module entry point is the only UE-dependent file.
list(FILTER CORE_SOURCES EXCLUDE REGEX "SharedWorldCoreModule\\.cpp$")

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# Match Unreal: no exceptions, no RTTI, strict warnings.
set(SW_FLAGS -fno-exceptions -fno-rtti -Wall -Wextra -Wshadow -Werror -Wno-missing-field-initializers)
if(SW_SANITIZE)
  list(APPEND SW_FLAGS -fsanitize=${SW_SANITIZE} -fno-omit-frame-pointer -g)
  set(SW_LINK -fsanitize=${SW_SANITIZE})
endif()

add_library(SharedWorldCore STATIC ${CORE_SOURCES})
target_include_directories(SharedWorldCore PUBLIC ${CORE_DIR}/Public)
target_compile_options(SharedWorldCore PRIVATE ${SW_FLAGS})
target_link_libraries(SharedWorldCore PUBLIC ZLIB::ZLIB Threads::Threads ${SW_LINK})

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(sw_tests ${TEST_SOURCES})
target_compile_options(sw_tests PRIVATE ${SW_FLAGS})
target_link_libraries(sw_tests PRIVATE SharedWorldCore ${SW_LINK})

enable_testing()
add_test(NAME sw_tests COMMAND sw_tests)
