add_executable(api_server main.cpp)

target_link_libraries(api_server PRIVATE
    agent_core
    coro::coro
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

if(WIN32)
    target_link_libraries(api_server PRIVATE ws2_32 wsock32)
endif()

set_target_properties(api_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Defined in the root CMakeLists when libpq is available; no-op otherwise.
if(COMMAND stage_libpq_dlls)
    stage_libpq_dlls(api_server)
endif()
