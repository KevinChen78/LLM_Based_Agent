cmake_minimum_required(VERSION 3.25)
project(LLM_Based_Agent VERSION 0.1.0 LANGUAGES CXX)

# C++20 is required for coroutines
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler-specific options
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-stdlib=libc++)
        add_link_options(-stdlib=libc++)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/W4 /permissive-)
    add_compile_options(/await:strict)
    add_compile_options(/utf-8)  # Force UTF-8 source and execution character sets
endif()

# Enable coroutine support
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-fcoroutines)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fcoroutines-ts)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(FetchDependencies)

# Local coro library (header-only + a few .cpp files, relative path)
set(CORO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../coro)
if(NOT EXISTS ${CORO_ROOT}/include/coro/coro.hpp)
    message(FATAL_ERROR "coro library not found at ${CORO_ROOT}")
endif()

add_library(coro STATIC)
add_library(coro::coro ALIAS coro)

target_sources(coro PRIVATE
    ${CORO_ROOT}/src/scheduler.cpp
    ${CORO_ROOT}/src/thread_pool.cpp
)

target_include_directories(coro PUBLIC ${CORO_ROOT}/include)

target_compile_features(coro PUBLIC cxx_std_20)

if(WIN32)
    target_compile_options(coro PUBLIC /await:strict)
    target_link_libraries(coro PUBLIC ws2_32 wsock32)
endif()

# -----------------------------------------------------------------------------
# Generated protobuf/grpc sources (Phase 0: keep proto definitions, generation optional)
# -----------------------------------------------------------------------------
# find_package(Protobuf CONFIG QUIET)
# find_package(gRPC CONFIG QUIET)
# if(Protobuf_FOUND AND gRPC_FOUND)
#     add_subdirectory(proto)
# endif()

# -----------------------------------------------------------------------------
# Core library
# -----------------------------------------------------------------------------
file(GLOB_RECURSE AGENT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

add_library(agent_core STATIC ${AGENT_SOURCES})

target_include_directories(agent_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(agent_core PUBLIC
    coro::coro
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    httplib::httplib
    sqlite3
)

if(WIN32)
    target_link_libraries(agent_core PUBLIC ws2_32 wsock32 crypt32)
endif()

# -----------------------------------------------------------------------------
# Optional libpq (PostgreSQL-backed DealCatalog). Auto-degrades to JSON-only
# when libpq is not found — the C++ source guards on AGENT_HAVE_LIBPQ.
# -----------------------------------------------------------------------------
option(ENABLE_PG_CATALOG "PostgreSQL-backed DealCatalog via libpq" ON)
set(PG_ROOT "C:/Program Files/PostgreSQL/17" CACHE PATH "PostgreSQL install root")

if(ENABLE_PG_CATALOG)
    find_path(LIBPQ_INCLUDE_DIR libpq-fe.h HINTS "${PG_ROOT}/include")
    find_library(LIBPQ_LIBRARY NAMES pq libpq HINTS "${PG_ROOT}/lib")
    if(LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
        message(STATUS "libpq found: ${LIBPQ_LIBRARY}")
        set(AGENT_HAVE_LIBPQ ON)
    else()
        message(WARNING "libpq not found under PG_ROOT=${PG_ROOT}; building without PG catalog")
    endif()
endif()

if(AGENT_HAVE_LIBPQ)
    target_include_directories(agent_core PRIVATE "${LIBPQ_INCLUDE_DIR}")
    # PUBLIC so the link propagates to api_server and test_agent.
    target_link_libraries(agent_core PUBLIC "${LIBPQ_LIBRARY}")
    target_compile_definitions(agent_core PRIVATE AGENT_HAVE_LIBPQ=1)
endif()

# libpq runtime DLL staging helper — must be DEFINED before the subdirectories
# that call it (the call site must be in the same directory scope as the exe
# target for add_custom_command(TARGET) to work).
if(AGENT_HAVE_LIBPQ AND WIN32)
    # libpq.dll + its runtime deps must sit next to the executables.
    # (Verify with: dumpbin /dependents <PG_ROOT>/bin/libpq.dll)
    set(LIBPQ_RUNTIME_DLLS
        "${PG_ROOT}/bin/libpq.dll"
        "${PG_ROOT}/bin/libssl-3-x64.dll"
        "${PG_ROOT}/bin/libcrypto-3-x64.dll"
        "${PG_ROOT}/bin/libiconv-2.dll"
        "${PG_ROOT}/bin/libintl-9.dll"
        "${PG_ROOT}/bin/libwinpthread-1.dll")
    function(stage_libpq_dlls tgt)
        foreach(dll ${LIBPQ_RUNTIME_DLLS})
            if(EXISTS "${dll}")
                add_custom_command(TARGET ${tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${dll}" "$<TARGET_FILE_DIR:${tgt}>")
            endif()
        endforeach()
    endfunction()
endif()

# -----------------------------------------------------------------------------
# Subdirectories
# -----------------------------------------------------------------------------
add_subdirectory(apps/api_server)

# Tests
enable_testing()
add_subdirectory(tests)

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY configs/ DESTINATION etc/llm_agent)
