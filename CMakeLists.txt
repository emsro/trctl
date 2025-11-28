cmake_minimum_required(VERSION 3.19)

option(TRCTL_TESTS_ENABLED "Enable tests" OFF)

project(trctl)

set(CMAKE_CXX_STANDARD 23)


include(FetchContent)
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG v1.51.0
)
FetchContent_Declare(
  cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
)
FetchContent_Declare(
  npb
  GIT_REPOSITORY https://github.com/nanopb/nanopb.git
  GIT_TAG nanopb-0.4.9.1
)
FetchContent_Declare(
  spdlog 
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.3
)
FetchContent_Declare(
  ecor
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ecor
)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(libuv cli11 npb spdlog ecor googletest)
target_compile_definitions(spdlog PUBLIC -DSPDLOG_USE_STD_FORMAT=ON)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${nanopb_SOURCE_DIR}/extra)
find_package(Nanopb REQUIRED)

add_library(trctl_lib src/server.cpp src/client.cpp src/util.cpp src/iface.cpp)
target_compile_features(trctl_lib PUBLIC cxx_std_20)
nanopb_generate_cpp(TARGET trctl_lib_proto iface.proto)
target_include_directories(trctl_lib_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/)
target_compile_options(trctl_lib PUBLIC "-Wall" "-Wextra" "-ftemplate-backtrace-limit=0")

target_link_libraries(trctl_lib PUBLIC nanopb spdlog::spdlog uv ecor::ecor trctl_lib_proto)


add_executable(hub src/hub/main.cpp)
target_link_libraries(hub PRIVATE CLI11::CLI11 trctl_lib)

add_executable(unit src/unit/main.cpp)
target_link_libraries(unit PRIVATE CLI11::CLI11 trctl_lib)



if(TRCTL_TESTS_ENABLED)
  enable_testing()
  file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS src/test/*_utest.cpp src/unit/test/*_utest.cpp)

  add_executable(tests ${TEST_SOURCES})
  target_link_libraries(tests PRIVATE gtest_main trctl_lib)
  add_test(NAME trctl_tests COMMAND tests)
endif()
