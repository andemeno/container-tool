cmake_minimum_required(VERSION 3.16)

set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(container-tool VERSION 0.0.1 LANGUAGES C)

add_executable(container-tool)
target_sources(container-tool PRIVATE main.c)
