cmake_minimum_required(VERSION 3.25)
project(runtime_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue CONFIG REQUIRED)

add_executable(host main.cpp)
target_link_libraries(host PRIVATE rex::core)
rexglue_configure_target(host)
rexglue_get_runtime_files(host runtime_files)
file(
  GENERATE
  OUTPUT ${CMAKE_BINARY_DIR}/runtime_files.txt
  CONTENT "$<JOIN:${runtime_files},\n>\n"
)
