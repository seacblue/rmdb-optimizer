include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(analyze)
add_subdirectory(record)
add_subdirectory(index)
add_subdirectory(system)
add_subdirectory(execution)
add_subdirectory(parser)
add_subdirectory(optimizer)
add_subdirectory(storage)
add_subdirectory(common)
add_subdirectory(replacer)
add_subdirectory(transaction)
add_subdirectory(recovery)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()


target_link_libraries(parser execution pthread)

add_executable(rmdb rmdb.cpp)
target_link_libraries(rmdb parser execution pthread planner analyze common)

add_executable(unit_test unit_test.cpp)
target_include_directories(unit_test PRIVATE ${CMAKE_SOURCE_DIR}/deps/googletest/googletest/include ${CMAKE_SOURCE_DIR}/deps/googletest/googlemock/include)
target_link_libraries(unit_test gtest gtest_main parser execution pthread planner analyze common storage record index system transaction recovery lru_replacer)
