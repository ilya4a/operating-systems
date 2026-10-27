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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Config.cpp")
    set(DISK_MONITOR_CONFIG_SOURCE Config.cpp)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.cpp")
    set(DISK_MONITOR_CONFIG_SOURCE config.cpp)
else()
    message(FATAL_ERROR
            "Cannot find Config.cpp or config.cpp in ${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()

set(DISK_MONITOR_SOURCES
        main.cpp
        DirectoryMonitor.cpp
        Daemon.cpp
        ${DISK_MONITOR_CONFIG_SOURCE}
)

foreach(required_source IN ITEMS main.cpp DirectoryMonitor.cpp Daemon.cpp)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${required_source}")
        message(FATAL_ERROR
                "Required source file is missing: ${required_source}"
        )
    endif()
endforeach()

function(enable_strict_warnings target)
    target_include_directories(
            ${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
    )
endfunction()

add_executable(disk-monitor ${DISK_MONITOR_SOURCES})
enable_strict_warnings(disk-monitor)

include(CTest)

if(BUILD_TESTING)
    add_executable(config_tests
            tests/unit/ConfigTests.cpp
            ${DISK_MONITOR_CONFIG_SOURCE}
    )
    enable_strict_warnings(config_tests)
    add_test(NAME config_unit_tests COMMAND config_tests)
    set_tests_properties(config_unit_tests PROPERTIES TIMEOUT 15 LABELS "unit")

    add_executable(directory_monitor_tests
            tests/unit/DirectoryMonitorTests.cpp
            DirectoryMonitor.cpp
    )
    enable_strict_warnings(directory_monitor_tests)
    add_test(NAME directory_monitor_unit_tests COMMAND directory_monitor_tests)
    set_tests_properties(directory_monitor_unit_tests PROPERTIES TIMEOUT 15 LABELS "unit")

    add_executable(daemon_tests
            tests/unit/DaemonTests.cpp
            Daemon.cpp
    )
    enable_strict_warnings(daemon_tests)
    add_test(NAME daemon_unit_tests COMMAND daemon_tests)
    set_tests_properties(daemon_unit_tests PROPERTIES TIMEOUT 15 LABELS "unit")

    find_program(DISK_MONITOR_BASH_EXECUTABLE NAMES bash)
    find_program(DISK_MONITOR_PYTHON3_EXECUTABLE NAMES python3)
    if(DISK_MONITOR_BASH_EXECUTABLE AND
            DISK_MONITOR_PYTHON3_EXECUTABLE AND
            EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh" AND
            EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon.conf.in")
        add_test(
                NAME daemon_smoke_tests
                COMMAND "${DISK_MONITOR_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh"
                --config-template "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon.conf.in"
                --binary "$<TARGET_FILE:disk-monitor>"
        )
        set_tests_properties(daemon_smoke_tests PROPERTIES
                TIMEOUT 90
                LABELS "integration"
        )
    else()
        message(STATUS
                "Smoke test not registered: requires bash, python3, tests/smoke_test.sh, and tests/daemon.conf.in"
        )
    endif()
endif()