cmake_minimum_required(VERSION 3.16)

project(DiskMonitor LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
            "Disk Monitor requires Linux (inotify, signalfd, and flock)."
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DISK_MONITOR_SOURCES
        main.cpp
        src/Config.cpp
        src/DirectoryMonitor.cpp
        src/Daemon.cpp
)

foreach(source IN LISTS DISK_MONITOR_SOURCES)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "Required source file is missing: ${source}")
    endif()
endforeach()

function(enable_strict_warnings target)
    target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
    )
endfunction()

# Main application
add_executable(disk-monitor ${DISK_MONITOR_SOURCES})
enable_strict_warnings(disk-monitor)

# Testing
include(CTest)

if(BUILD_TESTING)
    # Config unit tests
    add_executable(config_tests
            tests/unit/ConfigTests.cpp
            src/Config.cpp
    )
    enable_strict_warnings(config_tests)

    add_test(NAME config_unit_tests COMMAND config_tests)
    set_tests_properties(config_unit_tests PROPERTIES
            TIMEOUT 15
            LABELS "unit"
    )

    add_executable(directory_monitor_tests
            tests/unit/DirectoryMonitorTests.cpp
            src/DirectoryMonitor.cpp
    )
    enable_strict_warnings(directory_monitor_tests)

    add_test(
            NAME directory_monitor_unit_tests
            COMMAND directory_monitor_tests
    )
    set_tests_properties(directory_monitor_unit_tests PROPERTIES
            TIMEOUT 15
            LABELS "unit"
    )

    add_executable(daemon_tests
            tests/unit/DaemonTests.cpp
            src/Daemon.cpp
    )
    enable_strict_warnings(daemon_tests)

    add_test(NAME daemon_unit_tests COMMAND daemon_tests)
    set_tests_properties(daemon_unit_tests PROPERTIES
            TIMEOUT 15
            LABELS "unit"
    )

    find_program(DISK_MONITOR_BASH_EXECUTABLE NAMES bash)
    find_program(DISK_MONITOR_PYTHON3_EXECUTABLE NAMES python3)

    if(DISK_MONITOR_BASH_EXECUTABLE
            AND DISK_MONITOR_PYTHON3_EXECUTABLE
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh"
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon.conf.in")

        add_test(
                NAME daemon_smoke_tests
                COMMAND "${DISK_MONITOR_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh"
                --config-template
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon.conf.in"
                --binary "$<TARGET_FILE:disk-monitor>"
        )

        set_tests_properties(daemon_smoke_tests PROPERTIES
                TIMEOUT 90
                LABELS "integration"
        )
    else()
        message(STATUS
                "Smoke test not registered: bash, python3, or test files are missing"
        )
    endif()
endif()