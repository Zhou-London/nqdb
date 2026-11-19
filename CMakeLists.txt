cmake_minimum_required(VERSION 4.3)

project(nqdb
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(nlib
    GIT_REPOSITORY https://github.com/Zhou-London/nlib.git
    GIT_TAG v0.7.0
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(nlib)

set(BUILD_TOOLS OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_TESTING OFF)
FetchContent_Declare(cctz
    GIT_REPOSITORY https://github.com/google/cctz.git
    GIT_TAG v2.5
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(cctz)

find_package(SQLite3 3.38 REQUIRED)

add_library(nqdb INTERFACE)
add_library(nq::db ALIAS nqdb)
target_include_directories(nqdb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(nqdb INTERFACE cxx_std_23)
target_link_libraries(nqdb INTERFACE nlib::nlib SQLite3::SQLite3 cctz::cctz)

option(NQDB_BUILD_CLI "Build the nqdb command line program" ${PROJECT_IS_TOP_LEVEL})
if(NQDB_BUILD_CLI)
    FetchContent_Declare(cli11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG v2.5.0
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(cli11)

    add_library(nqdb_cli INTERFACE)
    target_include_directories(nqdb_cli INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(nqdb_cli INTERFACE nq::db CLI11::CLI11)

    add_executable(nqdb_program src/main.cpp)
    set_target_properties(nqdb_program PROPERTIES OUTPUT_NAME nqdb)
    target_link_libraries(nqdb_program PRIVATE nqdb_cli)
    target_compile_options(nqdb_program PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(NQDB_BUILD_TESTS "Build the nqdb tests" ${PROJECT_IS_TOP_LEVEL})
if(NQDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
