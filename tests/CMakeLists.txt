add_executable(test_agent
    test_task_planner.cpp
    test_safety_guard.cpp
    test_sqlite_session.cpp
)

target_link_libraries(test_agent PRIVATE
    agent_core
    coro::coro
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    GTest::gtest_main
)

if(WIN32)
    target_link_libraries(test_agent PRIVATE ws2_32 wsock32)
endif()

set_target_properties(test_agent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

include(GoogleTest)
gtest_discover_tests(test_agent)
