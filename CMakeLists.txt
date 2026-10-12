cmake_minimum_required(VERSION 3.16)
project(zer0-kristal CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia)

add_executable(zer0-kristal WIN32 main.cpp)
target_link_libraries(zer0-kristal PRIVATE Qt6::Widgets Qt6::Multimedia)
