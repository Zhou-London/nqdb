#pragma once

#include <nqdb/secmaster/schema.h>
#include <nqdb/sqlite.h>

namespace nqdb_test {

inline constexpr const char* two_venues_and_two_tick_rules = R"sql(
INSERT INTO venue (venue_id, code, mic, name, country, timezone) VALUES
  (1, 'XTST', 'XTST', 'Test Exchange', 'ZZ', 'Etc/UTC'),
  (2, 'XALT', NULL, 'Alternative Test Exchange', NULL, 'Etc/UTC');
INSERT INTO tick_rule (tick_rule_id, name) VALUES (1, 'CENT'), (2, 'BANDED');
INSERT INTO tick_band (tick_rule_id, price_from_nanos, tick_size_nanos) VALUES
  (1, 0, 10000000),
  (2, 0, 100000),
  (2, 1000000000, 10000000);
)sql";

inline nqdb::sqlite::database secmaster_with_venues() {
  nqdb::sqlite::database database = nqdb::sqlite::database::in_memory();
  nqdb::secmaster::create_schema(database);
  database.run_script(two_venues_and_two_tick_rules);
  return database;
}

}  // namespace nqdb_test
