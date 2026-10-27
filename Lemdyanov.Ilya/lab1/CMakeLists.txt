cmake_minimum_required(VERSION 3.16)

project(DiskMonitor LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Disk Monitor requires Linux (inotify, signalfd, and flock).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DISK_MONITOR_SOURCES
        main.cpp
        DirectoryMonitor.cpp
        Daemon.cpp
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Config.cpp")
    list(APPEND DISK_MONITOR_SOURCES Config.cpp)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.cpp")
    list(APPEND DISK_MONITOR_SOURCES config.cpp)
else()
    message(FATAL_ERROR
            "Cannot find Config.cpp or config.cpp in ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

foreach(required_source IN ITEMS main.cpp DirectoryMonitor.cpp Daemon.cpp)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${required_source}")
        message(FATAL_ERROR "Required source file is missing: ${required_source}")
    endif()
endforeach()

add_executable(disk-monitor ${DISK_MONITOR_SOURCES})
target_include_directories(disk-monitor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_options(disk-monitor PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
)