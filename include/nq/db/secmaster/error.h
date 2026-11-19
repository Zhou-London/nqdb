#pragma once

#include <stdexcept>

namespace nq::db::secmaster {

class error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

}  // namespace nq::db::secmaster
