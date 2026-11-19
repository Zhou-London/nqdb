#pragma once

#include <stdexcept>

namespace nqdb::secmaster {

class error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

}  // namespace nqdb::secmaster
