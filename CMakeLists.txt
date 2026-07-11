cmake_minimum_required(VERSION 3.21)

project(rufus-qt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets LinguistTools)
qt_standard_project_setup()

add_subdirectory(src)
