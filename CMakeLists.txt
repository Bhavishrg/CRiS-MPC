cmake_minimum_required(VERSION 3.14)
project(SS_PSI CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# AES-NI + SSE4 required by emp-tool
add_compile_options(-maes -msse4.2 -msse2)

# ── emp-tool ──────────────────────────────────────────────────────────────────
# Look for cmake config files installed by emp-tool.
# Typical install prefix is /usr/local; adjust CMAKE_PREFIX_PATH if needed.
list(APPEND CMAKE_PREFIX_PATH "/usr/local" "/usr/local/cmake")

find_package(emp-tool REQUIRED)

# Make emp-tool headers available to all subdirectories.
include_directories(${EMP-TOOL_INCLUDE_DIRS})

# Create an imported target so subdirectories can link against emp-tool::emp-tool
# (the installed config file only sets variables, not targets).
if(NOT TARGET emp-tool::emp-tool)
  add_library(emp-tool::emp-tool INTERFACE IMPORTED)
  set_target_properties(emp-tool::emp-tool PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${EMP-TOOL_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES      "${EMP-TOOL_LIBRARIES}")
endif()

# ── PSI include root (so code can #include "3pc/...") ─────────────────────
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ── nlohmann/json (used by benchmark/utils.h) ────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(nlohmann_json)

add_subdirectory(3pc)
add_subdirectory(test)
add_subdirectory(benchmark)
