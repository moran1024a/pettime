cmake_minimum_required(VERSION 3.21)
project(Pettime VERSION 0.3.1 LANGUAGES CXX)

include(cmake/SourceFingerprint.cmake)
pettime_source_fingerprint("${CMAKE_CURRENT_SOURCE_DIR}" PETTIME_SOURCE_FINGERPRINT)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/SourceFingerprint.h"
    CONTENT "#pragma once\n#define PETTIME_SOURCE_FINGERPRINT \"${PETTIME_SOURCE_FINGERPRINT}\"\n")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)

add_library(pettime_lib STATIC
    src/PetModel.cpp src/SwarmController.cpp src/AnimationLibrary.cpp
    src/PetWindow.cpp src/CrackWindow.cpp src/SettingsStore.cpp
    src/SingleInstance.cpp src/ApplicationController.cpp
    src/NetworkService.cpp src/NetworkPanel.cpp src/DispatchController.cpp
)
target_include_directories(pettime_lib PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(pettime_lib PUBLIC PETTIME_VERSION="${PROJECT_VERSION}")
target_link_libraries(pettime_lib PUBLIC Qt6::Widgets Qt6::Network)
if(MSVC)
    target_compile_options(pettime_lib PUBLIC /utf-8)
    target_compile_options(pettime_lib PRIVATE /W4)
else()
    target_compile_options(pettime_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

qt_add_executable(pettime WIN32 src/main.cpp resources.qrc)
target_link_libraries(pettime PRIVATE pettime_lib)
include(GNUInstallDirs)
install(TARGETS pettime RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/pettime)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME fingerprint_tests COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DWORK_ROOT=${CMAKE_CURRENT_BINARY_DIR}/fingerprint-test"
        "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TestFingerprint.cmake")
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    add_executable(pettime_tests tests/TestPettime.cpp resources.qrc)
    target_link_libraries(pettime_tests PRIVATE pettime_lib Qt6::Test)
    add_executable(instance_probe tests/InstanceProbe.cpp)
    target_link_libraries(instance_probe PRIVATE pettime_lib)
    add_dependencies(pettime_tests instance_probe)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(desktop_probe tests/DesktopProbe.cpp resources.qrc)
        target_link_libraries(desktop_probe PRIVATE pettime_lib Qt6::Test)
    endif()
    add_executable(dispatch_probe tests/DispatchProbe.cpp resources.qrc)
    target_link_libraries(dispatch_probe PRIVATE pettime_lib)
    add_executable(dispatch_tests tests/TestDispatch.cpp resources.qrc)
    target_link_libraries(dispatch_tests PRIVATE pettime_lib Qt6::Test)
    add_test(NAME dispatch_tests COMMAND dispatch_tests)
    set_tests_properties(dispatch_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(network_tests tests/TestNetwork.cpp)
    target_link_libraries(network_tests PRIVATE pettime_lib Qt6::Test)
    add_test(NAME network_tests COMMAND network_tests)
    set_tests_properties(network_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME pettime_tests COMMAND pettime_tests)
    set_tests_properties(pettime_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
