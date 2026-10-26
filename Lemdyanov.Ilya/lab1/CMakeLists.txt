cmake_minimum_required(VERSION 3.16)
project(disk-monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(disk-monitor main.cpp
        Config.cpp
        Config.h
        Daemon.cpp
        Daemon.h
        DirectoryMonitor.cpp
        DirectoryMonitor.h)

target_compile_options(disk-monitor PRIVATE
        -Wall
        -Werror
        -pedantic
)