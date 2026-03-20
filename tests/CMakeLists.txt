include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(throttler_test throttler_test.cpp)
target_link_libraries(throttler_test GTest::gtest_main)

add_executable(async_throttler_test async_throttler_test.cpp)
target_include_directories(async_throttler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include/async)
target_link_libraries(async_throttler_test GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(throttler_test)
gtest_discover_tests(async_throttler_test)
