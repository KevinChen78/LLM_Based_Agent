include(FetchContent)

# Use ghproxy mirror for GitHub if direct access fails.
# Set USE_GITHUB_MIRROR=ON to enable, or set FETCHCONTENT_BASE_DIR to a local cache.
option(USE_GITHUB_MIRROR "Use ghproxy mirror for GitHub FetchContent" OFF)

function(make_github_url url out_var)
    if(USE_GITHUB_MIRROR)
        set(${out_var} "https://ghproxy.com/${url}" PARENT_SCOPE)
    else()
        set(${out_var} "${url}" PARENT_SCOPE)
    endif()
endfunction()

make_github_url("https://github.com/nlohmann/json.git" JSON_URL)
make_github_url("https://github.com/gabime/spdlog.git" SPDLOG_URL)
make_github_url("https://github.com/yhirose/cpp-httplib.git" HTTPLIB_URL)
make_github_url("https://github.com/google/googletest.git" GTEST_URL)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY ${JSON_URL}
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY ${SPDLOG_URL}
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# cpp-httplib (header-only HTTP client/server)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY ${HTTPLIB_URL}
    GIT_TAG v0.15.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(httplib)

# GoogleTest (for tests)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY ${GTEST_URL}
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

# --- SQLite (amalgamation) ---------------------------------------------------
# Fetched directly from sqlite.org rather than via a GitHub clone: github.com
# git-clone is intermittently unreachable in this environment, while sqlite.org
# (a separate host) is reachable. The amalgamation is two C files compiled into
# a static lib; we use the raw sqlite3 C API (no wrapper dependency).
set(SQLITE_AMALGAMATION_URL "https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip"
    CACHE STRING "SQLite amalgamation zip URL (v3.53.3)")
set(sqlite3_src_dir "${CMAKE_BINARY_DIR}/_deps/sqlite3-src")
file(GLOB _sqlite3_c "${sqlite3_src_dir}/sqlite-amalgamation-*/sqlite3.c")
if(NOT _sqlite3_c)
    set(sqlite3_zip "${CMAKE_BINARY_DIR}/_deps/sqlite3-amalgamation.zip")
    message(STATUS "Downloading SQLite amalgamation from ${SQLITE_AMALGAMATION_URL}")
    file(DOWNLOAD "${SQLITE_AMALGAMATION_URL}" "${sqlite3_zip}"
         STATUS _sqlite_dl_status SHOW_PROGRESS TIMEOUT 180)
    list(GET _sqlite_dl_status 0 _sqlite_dl_code)
    if(_sqlite_dl_code)
        message(FATAL_ERROR "SQLite amalgamation download failed: ${_sqlite_dl_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${sqlite3_zip}" DESTINATION "${sqlite3_src_dir}")
    file(GLOB _sqlite3_c "${sqlite3_src_dir}/sqlite-amalgamation-*/sqlite3.c")
endif()
get_filename_component(_sqlite3_inc_dir "${_sqlite3_c}" DIRECTORY)
add_library(sqlite3 STATIC EXCLUDE_FROM_ALL "${_sqlite3_c}")
target_include_directories(sqlite3 PUBLIC "${_sqlite3_inc_dir}")
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)  # amalgamation is very chatty under /W4
endif()
