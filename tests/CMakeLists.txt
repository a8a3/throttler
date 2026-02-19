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

include(GoogleTest)
gtest_discover_tests(throttler_test)
