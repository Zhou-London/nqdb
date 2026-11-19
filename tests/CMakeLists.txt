FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

function(nqdb_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE nqdb::nqdb GTest::gtest_main ${ARGN})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    gtest_discover_tests(${name})
endfunction()

nqdb_add_test(sqlite_test)
nqdb_add_test(secmaster_schema_test)
nqdb_add_test(secmaster_loader_test)
nqdb_add_test(secmaster_decimal_test)
nqdb_add_test(secmaster_writer_test)
if(NQDB_BUILD_CLI)
    nqdb_add_test(cli_test nqdb_cli)
endif()
