add_executable(test_agent
    test_task_planner.cpp
    test_safety_guard.cpp
    test_sqlite_session.cpp
    test_streaming.cpp
    test_deals_tools.cpp
    test_deal_catalog_pg.cpp
    test_composer.cpp
    test_kb.cpp
    test_observability.cpp
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

# Defined in the root CMakeLists when libpq is available; no-op otherwise.
if(COMMAND stage_libpq_dlls)
    stage_libpq_dlls(test_agent)
endif()

include(GoogleTest)
# Several tests read data/deals.json by relative path — pin the working
# directory to the project root so `RUN_TESTS` / ctest behave the same as
# running bin/Release/test_agent.exe from the root.
gtest_discover_tests(test_agent WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
