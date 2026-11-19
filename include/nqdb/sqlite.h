#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace nqdb::sqlite {

class error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

template <typename T>
inline constexpr bool is_optional = false;
template <typename T>
inline constexpr bool is_optional<std::optional<T>> = true;

template <typename T>
concept integer_wrapper = requires(const T& wrapper) {
  { wrapper.value() } -> std::integral;
};

class statement {
 public:
  template <typename T>
  statement& bind(int parameter, const T& value) {
    if constexpr (is_optional<T>) {
      if (value) return bind(parameter, *value);
      check(sqlite3_bind_null(handle_.get(), parameter));
    } else if constexpr (std::integral<T>) {
      check(sqlite3_bind_int64(handle_.get(), parameter, static_cast<std::int64_t>(value)));
    } else if constexpr (integer_wrapper<T>) {
      return bind(parameter, value.value());
    } else {
      const std::string_view text = value;
      check(sqlite3_bind_text64(handle_.get(), parameter, text.data(), text.size(),
                                SQLITE_TRANSIENT, SQLITE_UTF8));
    }
    return *this;
  }

  bool next_row() {
    const int result = sqlite3_step(handle_.get());
    if (result == SQLITE_ROW) return true;
    if (result != SQLITE_DONE) check(result);
    return false;
  }

  void run() { next_row(); }

  void rewind() noexcept { sqlite3_reset(handle_.get()); }

  bool is_null(int column) const noexcept {
    return sqlite3_column_type(handle_.get(), column) == SQLITE_NULL;
  }

  template <typename T>
  T get(int column) const {
    if constexpr (is_optional<T>) {
      return is_null(column) ? T{} : T{get<typename T::value_type>(column)};
    } else if constexpr (std::integral<T>) {
      return static_cast<T>(sqlite3_column_int64(handle_.get(), column));
    } else if constexpr (integer_wrapper<T>) {
      return T{get<std::int64_t>(column)};
    } else {
      const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(handle_.get(), column));
      const auto size = static_cast<std::size_t>(sqlite3_column_bytes(handle_.get(), column));
      return T{std::string_view{text, size}};
    }
  }

 private:
  friend class database;

  struct finalizer {
    void operator()(sqlite3_stmt* handle) const noexcept { sqlite3_finalize(handle); }
  };

  explicit statement(sqlite3_stmt* handle) noexcept : handle_(handle) {}

  void check(int result) const {
    if (result != SQLITE_OK) throw error(sqlite3_errmsg(sqlite3_db_handle(handle_.get())));
  }

  std::unique_ptr<sqlite3_stmt, finalizer> handle_;
};

class database {
 public:
  static database create(const std::filesystem::path& new_file) {
    if (std::filesystem::exists(new_file)) {
      throw error(new_file.string() + " already exists");
    }
    return database(new_file.c_str(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  }

  static database open(const std::filesystem::path& file) {
    return database(file.c_str(), SQLITE_OPEN_READWRITE);
  }

  static database open_read_only(const std::filesystem::path& file) {
    return database(file.c_str(), SQLITE_OPEN_READONLY);
  }

  static database in_memory() {
    return database(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  }

  void run_script(const std::string& sql) {
    check(sqlite3_exec(handle_.get(), sql.c_str(), nullptr, nullptr, nullptr));
  }

  statement prepare(std::string_view sql) {
    sqlite3_stmt* prepared = nullptr;
    check(sqlite3_prepare_v2(handle_.get(), sql.data(), static_cast<int>(sql.size()), &prepared,
                             nullptr));
    return statement(prepared);
  }

  std::int64_t last_inserted_id() const noexcept {
    return sqlite3_last_insert_rowid(handle_.get());
  }

  int changed_rows() const noexcept { return sqlite3_changes(handle_.get()); }

  void copy_to(const std::filesystem::path& new_file) {
    prepare("VACUUM INTO ?1").bind(1, new_file.native()).run();
  }

 private:
  friend class transaction;

  static constexpr int wait_for_other_writers_ms = 5000;

  struct closer {
    void operator()(sqlite3* handle) const noexcept { sqlite3_close_v2(handle); }
  };

  database(const char* file, int open_flags) {
    sqlite3* opened = nullptr;
    const int result = sqlite3_open_v2(file, &opened, open_flags, nullptr);
    handle_.reset(opened);
    check(result);
    sqlite3_busy_timeout(handle_.get(), wait_for_other_writers_ms);
    run_script("PRAGMA foreign_keys = ON");
  }

  void check(int result) const {
    if (result != SQLITE_OK) throw error(sqlite3_errmsg(handle_.get()));
  }

  std::unique_ptr<sqlite3, closer> handle_;
};

class transaction {
 public:
  explicit transaction(database& database) : database_(database) {
    database_.run_script("SAVEPOINT nqdb_transaction");
  }

  ~transaction() {
    if (open_) {
      sqlite3_exec(database_.handle_.get(),
                   "ROLLBACK TO nqdb_transaction; RELEASE nqdb_transaction", nullptr, nullptr,
                   nullptr);
    }
  }

  transaction(const transaction&) = delete;
  transaction& operator=(const transaction&) = delete;

  void commit() {
    database_.run_script("RELEASE nqdb_transaction");
    open_ = false;
  }

 private:
  database& database_;
  bool open_ = true;
};

}  // namespace nqdb::sqlite
