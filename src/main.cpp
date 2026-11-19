#include <iostream>

#include "cli.h"

int main(int argc, char** argv) { return nq::db::cli::run(argc, argv, std::cout, std::cerr); }
