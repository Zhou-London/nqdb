#pragma once

#include <filesystem>
#include <string>

#include <stdlib.h>

namespace nqdb_test {

class temporary_directory {
 public:
  temporary_directory() {
    std::string name_pattern = std::filesystem::temp_directory_path() / "nqdb_XXXXXX";
    path_ = mkdtemp(name_pattern.data());
  }

  ~temporary_directory() { std::filesystem::remove_all(path_); }

  temporary_directory(const temporary_directory&) = delete;
  temporary_directory& operator=(const temporary_directory&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace nqdb_test
