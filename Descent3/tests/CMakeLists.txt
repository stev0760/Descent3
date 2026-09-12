set(CMAKE_FOLDER "tests")

add_executable(
  porting-tests
  porting-tests.cpp
)

target_link_libraries(
  porting-tests
  module
  GTest::gtest_main
)

gtest_discover_tests(porting-tests)

add_executable(
  gamespyutils_tests
  gamespyutils_tests.cpp
  ../gamespyutils.cpp
)
target_link_libraries(
  gamespyutils_tests
  GTest::gtest_main
)
gtest_discover_tests(gamespyutils_tests)

# The isolated navigation harness compiles production functions with synthetic geometry stubs.
if(UNIX AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME D3.BotSkelChain
      COMMAND ${CMAKE_COMMAND} -E env "CXX=${CMAKE_CXX_COMPILER}"
        ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tools/test_bot_skel_chain.py)
    add_test(NAME D3.BotLogAnalysis
      COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tools/test_analyze_bot_log.py)
  endif()
endif()
