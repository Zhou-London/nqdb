#include <iostream>

#include "cli.h"

int main(int argc, char** argv) { return nqdb::cli::run(argc, argv, std::cout, std::cerr); }
