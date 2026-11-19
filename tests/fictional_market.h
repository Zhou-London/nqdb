#pragma once

#include <nq/db/secmaster/records.h>
#include <nq/db/sqlite.h>

#include "secmaster_test_database.h"

namespace nqdb_test {

inline constexpr nq::db::secmaster::instrument_id acme{1}, bolt_fund{2}, test_index{3},
    index_future{4}, acme_call{5}, foo_bar_pair{6};
inline constexpr nq::db::secmaster::listing_id acme_on_test_exchange{1}, bolt_on_test_exchange{2},
    acme_on_alternative{3}, future_on_test_exchange{4}, call_on_test_exchange{5},
    pair_on_test_exchange{6};

inline nq::db::sqlite::database fictional_market() {
  nq::db::sqlite::database secmaster = secmaster_with_venues();
  secmaster.run_script(R"sql(
INSERT INTO instrument (instrument_id, asset_class, name) VALUES
  (1, 'EQUITY', 'Acme'), (2, 'ETF', 'Bolt Fund'), (3, 'INDEX', 'Test 100'),
  (4, 'FUTURE', 'Test 100 Dec 2026'), (5, 'OPTION', 'Acme Call 50'), (6, 'CRYPTO', 'FOO/BAR');
INSERT INTO future_spec VALUES (4, 3, 'T1', '2026-12-18', 50000000000, 'CASH');
INSERT INTO option_spec VALUES (5, 1, '2026-12-18', 50000000000, 'CALL', 'AMERICAN', 100000000000);
INSERT INTO pair_spec VALUES (6, 'FOO', 'BAR');

INSERT INTO listing (listing_id, instrument_id, venue_id, currency, is_primary, listed_on,
                     delisted_on) VALUES
  (1, 1, 1, 'TST', 1, '2020-01-01', NULL),
  (2, 2, 1, 'TST', 1, '2020-01-01', '2025-01-01'),
  (3, 1, 2, 'ALT', 0, '2024-06-01', NULL),
  (4, 4, 1, 'TST', 1, '2020-01-01', NULL),
  (5, 5, 1, 'TST', 1, '2020-01-01', NULL),
  (6, 6, 1, 'BAR', 1, '2020-01-01', NULL);

INSERT INTO listing_symbol (listing_id, source, symbol, valid_from, valid_until) VALUES
  (1, 'VENUE', 'ACME', '2020-01-01', '2024-06-01'),
  (1, 'VENUE', 'ACMX', '2024-06-01', NULL),
  (1, 'VENDOR', 'ACME.T', '2020-01-01', NULL),
  (2, 'VENUE', 'BOLT', '2020-01-01', '2025-01-01'),
  (3, 'VENUE', 'ACMX', '2024-06-01', NULL),
  (4, 'VENUE', 'T1Z6', '2020-01-01', NULL),
  (5, 'VENUE', 'ACMX 261218C50', '2020-01-01', NULL),
  (6, 'VENUE', 'FOOBAR', '2020-01-01', NULL);

INSERT INTO listing_spec (listing_id, valid_from, valid_until, lot_size_nanos, min_qty_nanos,
                          tick_rule_id) VALUES
  (1, '2020-01-01', '2024-06-01', 100000000000, NULL, 1),
  (1, '2024-06-01', NULL, 1000000000, NULL, 2),
  (2, '2020-01-01', '2025-01-01', 1000000000, NULL, 1),
  (3, '2024-06-01', NULL, 1000000000, NULL, 1),
  (4, '2020-01-01', NULL, 1000000000, NULL, 1),
  (5, '2020-01-01', NULL, 1000000000, NULL, 1),
  (6, '2020-01-01', NULL, 1000, 5000, 1);
)sql");
  return secmaster;
}

}  // namespace nqdb_test
