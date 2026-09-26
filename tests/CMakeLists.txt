file(GLOB FLUB_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(flub_tests ${FLUB_TEST_SOURCES})
target_link_libraries(flub_tests PRIVATE flub::core flub::compiler_settings)
target_include_directories(flub_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME flub_tests COMMAND flub_tests)
