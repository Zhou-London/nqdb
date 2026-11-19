#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nq/db/secmaster/text.h>
#include <nq/db/secmaster/error.h>
#include <nq/db/sqlite.h>

namespace nq::db::secmaster {

inline constexpr std::int64_t schema_version = 2;

inline constexpr std::array validity_tables_of_a_listing{std::string_view{"listing_symbol"},
                                                         std::string_view{"listing_spec"}};

namespace detail {

inline constexpr const char* tables = R"sql(
CREATE TABLE venue (
  venue_id  INTEGER PRIMARY KEY AUTOINCREMENT,
  code      TEXT NOT NULL UNIQUE CHECK (code <> ''),
  mic       TEXT UNIQUE CHECK (length(mic) = 4),
  name      TEXT NOT NULL CHECK (name <> ''),
  country   TEXT CHECK (length(country) = 2),
  timezone  TEXT NOT NULL CHECK (timezone <> '')
) STRICT;

CREATE TABLE issuer (
  issuer_id  INTEGER PRIMARY KEY AUTOINCREMENT,
  name       TEXT NOT NULL CHECK (name <> ''),
  lei        TEXT UNIQUE CHECK (length(lei) = 20),
  country    TEXT CHECK (length(country) = 2)
) STRICT;

CREATE TABLE instrument (
  instrument_id  INTEGER PRIMARY KEY AUTOINCREMENT,
  asset_class    TEXT NOT NULL CHECK (asset_class IN
                 ('EQUITY', 'ETF', 'FUTURE', 'OPTION', 'FX', 'CRYPTO', 'BOND', 'INDEX')),
  issuer_id      INTEGER REFERENCES issuer,
  cfi            TEXT CHECK (length(cfi) = 6),
  name           TEXT NOT NULL CHECK (name <> '')
) STRICT;

CREATE TABLE instrument_identifier (
  instrument_id  INTEGER NOT NULL REFERENCES instrument,
  scheme         TEXT NOT NULL CHECK (scheme <> ''),
  value          TEXT NOT NULL CHECK (value <> ''),
  valid_from     TEXT NOT NULL CHECK (valid_from IS date(valid_from)),
  valid_until    TEXT CHECK (valid_until IS date(valid_until)),
  PRIMARY KEY (instrument_id, scheme, valid_from),
  CONSTRAINT "an identifier ends after it starts"
    CHECK (valid_until IS NULL OR valid_from < valid_until)
) STRICT;
CREATE INDEX instrument_identifier_by_scheme_value ON instrument_identifier (scheme, value);

CREATE TABLE future_spec (
  instrument_id     INTEGER PRIMARY KEY REFERENCES instrument,
  underlying_id     INTEGER NOT NULL REFERENCES instrument,
  root              TEXT NOT NULL CHECK (root <> ''),
  expiry            TEXT NOT NULL CHECK (expiry IS date(expiry)),
  multiplier_nanos  INTEGER NOT NULL CHECK (multiplier_nanos > 0),
  settlement        TEXT NOT NULL CHECK (settlement IN ('CASH', 'PHYSICAL'))
) STRICT;

CREATE TABLE option_spec (
  instrument_id     INTEGER PRIMARY KEY REFERENCES instrument,
  underlying_id     INTEGER NOT NULL REFERENCES instrument,
  expiry            TEXT NOT NULL CHECK (expiry IS date(expiry)),
  strike_nanos      INTEGER NOT NULL,
  option_right      TEXT NOT NULL CHECK (option_right IN ('PUT', 'CALL')),
  exercise_style    TEXT NOT NULL CHECK (exercise_style IN ('AMERICAN', 'EUROPEAN')),
  multiplier_nanos  INTEGER NOT NULL CHECK (multiplier_nanos > 0)
) STRICT;

CREATE TABLE pair_spec (
  instrument_id   INTEGER PRIMARY KEY REFERENCES instrument,
  base_currency   TEXT NOT NULL CHECK (base_currency <> ''),
  quote_currency  TEXT NOT NULL CHECK (quote_currency <> '')
) STRICT;

CREATE TABLE listing (
  listing_id     INTEGER PRIMARY KEY AUTOINCREMENT,
  instrument_id  INTEGER NOT NULL REFERENCES instrument,
  venue_id       INTEGER NOT NULL REFERENCES venue,
  currency       TEXT NOT NULL CHECK (currency <> ''),
  is_primary     INTEGER NOT NULL DEFAULT 0 CHECK (is_primary IN (0, 1)),
  listed_on      TEXT NOT NULL CHECK (listed_on IS date(listed_on)),
  delisted_on    TEXT CHECK (delisted_on IS date(delisted_on)),
  UNIQUE (instrument_id, venue_id, currency),
  CONSTRAINT "a listing is delisted after it is listed"
    CHECK (delisted_on IS NULL OR listed_on < delisted_on)
) STRICT;

CREATE TRIGGER listing_venue_is_permanent
BEFORE UPDATE OF venue_id ON listing
WHEN NEW.venue_id IS NOT OLD.venue_id
BEGIN
  SELECT RAISE(ABORT, 'a listing stays on its venue');
END;

CREATE TABLE listing_symbol (
  listing_id   INTEGER NOT NULL REFERENCES listing,
  source       TEXT NOT NULL CHECK (source <> ''),
  symbol       TEXT NOT NULL CHECK (symbol <> ''),
  valid_from   TEXT NOT NULL CHECK (valid_from IS date(valid_from)),
  valid_until  TEXT CHECK (valid_until IS date(valid_until)),
  PRIMARY KEY (listing_id, source, valid_from),
  CONSTRAINT "a symbol ends after it starts"
    CHECK (valid_until IS NULL OR valid_from < valid_until)
) STRICT;
CREATE INDEX listing_symbol_by_source_symbol ON listing_symbol (source, symbol);

CREATE TABLE tick_rule (
  tick_rule_id  INTEGER PRIMARY KEY AUTOINCREMENT,
  name          TEXT NOT NULL UNIQUE CHECK (name <> '')
) STRICT;

CREATE TABLE tick_band (
  tick_rule_id      INTEGER NOT NULL REFERENCES tick_rule,
  price_from_nanos  INTEGER NOT NULL,
  tick_size_nanos   INTEGER NOT NULL CHECK (tick_size_nanos > 0),
  PRIMARY KEY (tick_rule_id, price_from_nanos)
) STRICT;

CREATE TABLE listing_spec (
  listing_id      INTEGER NOT NULL REFERENCES listing,
  valid_from      TEXT NOT NULL CHECK (valid_from IS date(valid_from)),
  valid_until     TEXT CHECK (valid_until IS date(valid_until)),
  lot_size_nanos  INTEGER NOT NULL CHECK (lot_size_nanos > 0),
  min_qty_nanos   INTEGER CHECK (min_qty_nanos > 0),
  tick_rule_id    INTEGER NOT NULL REFERENCES tick_rule,
  PRIMARY KEY (listing_id, valid_from),
  CONSTRAINT "a listing spec ends after it starts"
    CHECK (valid_until IS NULL OR valid_from < valid_until)
) STRICT;

CREATE TABLE corporate_action (
  corporate_action_id    INTEGER PRIMARY KEY AUTOINCREMENT,
  instrument_id          INTEGER NOT NULL REFERENCES instrument,
  action_type            TEXT NOT NULL CHECK (action_type IN
                         ('CASH_DIVIDEND', 'STOCK_DIVIDEND', 'SPLIT', 'SPINOFF',
                          'MERGER', 'RIGHTS', 'DELISTING')),
  ex_date                TEXT NOT NULL CHECK (ex_date IS date(ex_date)),
  announce_date          TEXT CHECK (announce_date IS date(announce_date)),
  record_date            TEXT CHECK (record_date IS date(record_date)),
  pay_date               TEXT CHECK (pay_date IS date(pay_date)),
  cash_per_share_nanos   INTEGER CHECK (cash_per_share_nanos > 0),
  currency               TEXT,
  new_shares             INTEGER CHECK (new_shares > 0),
  old_shares             INTEGER CHECK (old_shares > 0),
  related_instrument_id  INTEGER REFERENCES instrument,
  recorded_at            TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  CONSTRAINT "cash comes with its currency"
    CHECK ((cash_per_share_nanos IS NULL) = (currency IS NULL)),
  CONSTRAINT "new and old shares come together"
    CHECK ((new_shares IS NULL) = (old_shares IS NULL)),
  CONSTRAINT "a split or stock dividend has a share ratio"
    CHECK (action_type NOT IN ('SPLIT', 'STOCK_DIVIDEND') OR new_shares IS NOT NULL),
  CONSTRAINT "a cash dividend has cash"
    CHECK (action_type <> 'CASH_DIVIDEND' OR cash_per_share_nanos IS NOT NULL)
) STRICT;

CREATE TABLE weekly_session (
  venue_id       INTEGER NOT NULL REFERENCES venue,
  weekday        INTEGER NOT NULL CHECK (weekday BETWEEN 1 AND 7),
  opens_minute   INTEGER NOT NULL CHECK (opens_minute BETWEEN -1440 AND 1439),
  closes_minute  INTEGER NOT NULL CHECK (closes_minute <= 2880),
  valid_from     TEXT NOT NULL CHECK (valid_from IS date(valid_from)),
  valid_until    TEXT CHECK (valid_until IS date(valid_until)),
  PRIMARY KEY (venue_id, weekday, opens_minute, valid_from),
  CONSTRAINT "a session closes after it opens" CHECK (opens_minute < closes_minute),
  CONSTRAINT "a weekly session ends after it starts"
    CHECK (valid_until IS NULL OR valid_from < valid_until)
) STRICT;

CREATE TABLE closed_day (
  venue_id  INTEGER NOT NULL REFERENCES venue,
  day       TEXT NOT NULL CHECK (day IS date(day)),
  PRIMARY KEY (venue_id, day)
) STRICT;

CREATE TABLE special_session (
  venue_id       INTEGER NOT NULL REFERENCES venue,
  day            TEXT NOT NULL CHECK (day IS date(day)),
  opens_minute   INTEGER NOT NULL CHECK (opens_minute BETWEEN -1440 AND 1439),
  closes_minute  INTEGER NOT NULL CHECK (closes_minute <= 2880),
  PRIMARY KEY (venue_id, day, opens_minute),
  CONSTRAINT "a session closes after it opens" CHECK (opens_minute < closes_minute)
) STRICT;

CREATE TRIGGER closed_day_has_no_special_session
BEFORE INSERT ON closed_day
WHEN EXISTS (SELECT 1 FROM special_session WHERE venue_id = NEW.venue_id AND day = NEW.day)
BEGIN
  SELECT RAISE(ABORT, 'a day is closed or has special sessions, not both');
END;

CREATE TRIGGER special_session_is_on_an_open_day
BEFORE INSERT ON special_session
WHEN EXISTS (SELECT 1 FROM closed_day WHERE venue_id = NEW.venue_id AND day = NEW.day)
BEGIN
  SELECT RAISE(ABORT, 'a day is closed or has special sessions, not both');
END;

CREATE TABLE change_log (
  change_id   INTEGER PRIMARY KEY AUTOINCREMENT,
  changed_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  table_name  TEXT NOT NULL CHECK (table_name <> ''),
  operation   TEXT NOT NULL CHECK (operation IN ('INSERT', 'UPDATE', 'DELETE')),
  old_row     TEXT CHECK (json_valid(old_row)),
  new_row     TEXT CHECK (json_valid(new_row)),
  CONSTRAINT "a change has the rows of its operation"
    CHECK ((old_row IS NULL) = (operation = 'INSERT')
       AND (new_row IS NULL) = (operation = 'DELETE'))
) STRICT;
)sql";

inline constexpr std::string_view change_log_table = "change_log";

struct exclusive_validity {
  std::string_view table;
  std::string_view rule;
  std::string_view same_key;
  std::string_view violation;
};

inline constexpr std::array exclusive_validities{
    exclusive_validity{
        .table = "instrument_identifier",
        .rule = "one_per_scheme",
        .same_key = "other.instrument_id = NEW.instrument_id AND other.scheme = NEW.scheme",
        .violation = "an instrument has one identifier per scheme at a time"},
    exclusive_validity{
        .table = "instrument_identifier",
        .rule = "unique_in_scheme",
        .same_key = "other.scheme = NEW.scheme AND other.value = NEW.value",
        .violation = "an identifier names one instrument per scheme at a time"},
    exclusive_validity{
        .table = "listing_symbol",
        .rule = "one_per_source",
        .same_key = "other.listing_id = NEW.listing_id AND other.source = NEW.source",
        .violation = "a listing has one symbol per source at a time"},
    exclusive_validity{
        .table = "listing_symbol",
        .rule = "unique_on_venue",
        .same_key = "other.source = NEW.source AND other.symbol = NEW.symbol "
                    "AND (SELECT venue_id FROM listing WHERE listing_id = other.listing_id) = "
                    "(SELECT venue_id FROM listing WHERE listing_id = NEW.listing_id)",
        .violation = "a symbol names one listing per venue and source at a time"},
    exclusive_validity{
        .table = "listing_spec",
        .rule = "one_per_listing",
        .same_key = "other.listing_id = NEW.listing_id",
        .violation = "a listing has one spec at a time"},
};

inline std::string exclusive_validity_trigger(const exclusive_validity& exclusive,
                                              std::string_view write) {
  return std::format(
      "CREATE TRIGGER {0}_{1}_after_{2} AFTER {2} ON {0} "
      "WHEN EXISTS ("
      "  SELECT 1 FROM {0} other "
      "  WHERE other.rowid <> NEW.rowid AND {3} "
      "    AND (NEW.valid_until IS NULL OR other.valid_from < NEW.valid_until) "
      "    AND (other.valid_until IS NULL OR NEW.valid_from < other.valid_until)) "
      "BEGIN SELECT RAISE(ABORT, '{4}'); END;",
      exclusive.table, exclusive.rule, write, exclusive.same_key, exclusive.violation);
}

inline std::string within_listing_lifetime_trigger(std::string_view validity_table,
                                                   std::string_view write) {
  return std::format(
      "CREATE TRIGGER {0}_within_listing_lifetime_after_{1} AFTER {1} ON {0} "
      "WHEN EXISTS ("
      "  SELECT 1 FROM listing "
      "  WHERE listing_id = NEW.listing_id "
      "    AND (NEW.valid_from < listed_on "
      "         OR (delisted_on IS NOT NULL "
      "             AND (NEW.valid_until IS NULL OR delisted_on < NEW.valid_until)))) "
      "BEGIN SELECT RAISE(ABORT, 'a {0} row is valid only while its listing is listed'); END;",
      validity_table, write);
}

inline std::vector<std::string> first_column_of_each_row(sqlite::statement row) {
  std::vector<std::string> values;
  while (row.next_row()) values.push_back(row.get<std::string>(0));
  return values;
}

inline std::vector<std::string> logged_tables(sqlite::database& database) {
  sqlite::statement table = database.prepare(
      "SELECT name FROM sqlite_schema "
      "WHERE type = 'table' AND name NOT LIKE 'sqlite~_%' ESCAPE '~' AND name <> ?1 "
      "ORDER BY name");
  table.bind(1, change_log_table);
  return first_column_of_each_row(std::move(table));
}

inline std::string json_of_row(std::string_view old_or_new,
                               const std::vector<std::string>& columns) {
  std::string json = "json_object(";
  for (const std::string& column : columns) {
    json += std::format("{}'{}', {}.{}", json.ends_with('(') ? "" : ", ", column, old_or_new,
                        column);
  }
  return json + ")";
}

inline void log_every_write(sqlite::database& database, const std::string& table) {
  sqlite::statement column = database.prepare("SELECT name FROM pragma_table_info(?1)");
  column.bind(1, table);
  const std::vector<std::string> columns = first_column_of_each_row(std::move(column));
  const std::string old_row = json_of_row("OLD", columns);
  const std::string new_row = json_of_row("NEW", columns);
  const auto log_after = [&](std::string_view write, std::string_view operation,
                             std::string_view when, std::string_view old_json,
                             std::string_view new_json) {
    database.run_script(std::format(
        "CREATE TRIGGER {0}_is_logged_after_{1} AFTER {1} ON {0} {2} "
        "BEGIN INSERT INTO {3} (table_name, operation, old_row, new_row) "
        "VALUES ('{0}', '{4}', {5}, {6}); END;",
        table, write, when, change_log_table, operation, old_json, new_json));
  };
  log_after("insert", "INSERT", "", "NULL", new_row);
  log_after("update", "UPDATE", std::format("WHEN {} <> {}", old_row, new_row), old_row, new_row);
  log_after("delete", "DELETE", "", old_row, "NULL");
}

}  // namespace detail

inline void create_schema(sqlite::database& empty_database) {
  sqlite::transaction tables_with_their_triggers_and_version(empty_database);
  empty_database.run_script(detail::tables);
  for (const std::string_view write : {"insert", "update"}) {
    for (const detail::exclusive_validity& exclusive : detail::exclusive_validities) {
      empty_database.run_script(detail::exclusive_validity_trigger(exclusive, write));
    }
    for (const std::string_view validity_table : validity_tables_of_a_listing) {
      empty_database.run_script(detail::within_listing_lifetime_trigger(validity_table, write));
    }
  }
  for (const std::string& table : detail::logged_tables(empty_database)) {
    detail::log_every_write(empty_database, table);
  }
  empty_database.run_script(std::format("PRAGMA user_version = {}", schema_version));
  tables_with_their_triggers_and_version.commit();
}

inline void require_schema_version(sqlite::database& database) {
  sqlite::statement row = database.prepare("PRAGMA user_version");
  row.next_row();
  const auto version = row.get<std::int64_t>(0);
  if (version != schema_version) {
    throw error(std::format("database has schema version {}, this build uses version {}",
                            version, schema_version));
  }
}

inline sqlite::database open(const std::filesystem::path& file) {
  sqlite::database database = sqlite::database::open(file);
  require_schema_version(database);
  return database;
}

inline sqlite::database open_read_only(const std::filesystem::path& file) {
  sqlite::database database = sqlite::database::open_read_only(file);
  require_schema_version(database);
  return database;
}

inline std::filesystem::path archive(sqlite::database& database,
                                     const std::filesystem::path& directory,
                                     std::chrono::sys_days as_of) {
  std::filesystem::path dated_copy = directory / std::format("secmaster-{}.db", iso_date(as_of));
  database.copy_to(dated_copy);
  return dated_copy;
}

}  // namespace nq::db::secmaster
