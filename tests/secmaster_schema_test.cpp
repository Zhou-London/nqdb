#include <nqdb/secmaster/schema.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "secmaster_test_database.h"
#include "temporary_directory.h"

namespace {

using namespace std::chrono_literals;
using nqdb::sqlite::database;
using nqdb::sqlite::error;

database secmaster_with_two_listings_per_venue() {
  database secmaster = nqdb_test::secmaster_with_venues();
  secmaster.run_script(R"sql(
INSERT INTO instrument (instrument_id, asset_class, name) VALUES
  (1, 'EQUITY', 'Acme'), (2, 'EQUITY', 'Bolt');
INSERT INTO listing (listing_id, instrument_id, venue_id, currency, listed_on) VALUES
  (1, 1, 1, 'TST', '2020-01-01'),
  (2, 2, 1, 'TST', '2020-01-01'),
  (3, 1, 2, 'TST', '2020-01-01');
INSERT INTO listing_symbol (listing_id, source, symbol, valid_from) VALUES
  (1, 'VENUE', 'ACME', '2020-01-01');
)sql");
  return secmaster;
}

TEST(SecmasterSchema, CreateTablesStampsTheSchemaVersion) {
  database secmaster = nqdb_test::secmaster_with_venues();
  nqdb::sqlite::statement version = secmaster.prepare("PRAGMA user_version");
  ASSERT_TRUE(version.next_row());
  EXPECT_EQ(version.get<std::int64_t>(0), nqdb::secmaster::schema_version);
}

TEST(SecmasterSchema, AListingHasOneSymbolPerSourceAtATime) {
  database secmaster = secmaster_with_two_listings_per_venue();
  EXPECT_THROW(secmaster.run_script("INSERT INTO listing_symbol (listing_id, source, symbol, "
                                    "valid_from) VALUES (1, 'VENUE', 'ACMX', '2024-06-01')"),
               error);
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO listing_symbol (listing_id, source, symbol, "
                                       "valid_from) VALUES (1, 'VENDOR', 'ACME.T', '2024-06-01')"));
}

TEST(SecmasterSchema, AClosedSymbolCannotBeExtendedOverItsSuccessor) {
  database secmaster = secmaster_with_two_listings_per_venue();
  secmaster.run_script(R"sql(
UPDATE listing_symbol SET valid_until = '2024-06-01' WHERE listing_id = 1 AND symbol = 'ACME';
INSERT INTO listing_symbol (listing_id, source, symbol, valid_from) VALUES
  (1, 'VENUE', 'ACMX', '2024-06-01');
)sql");
  EXPECT_THROW(secmaster.run_script("UPDATE listing_symbol SET valid_until = '2024-06-02' "
                                    "WHERE listing_id = 1 AND symbol = 'ACME'"),
               error);
  EXPECT_THROW(secmaster.run_script("UPDATE listing_symbol SET valid_until = NULL "
                                    "WHERE listing_id = 1 AND symbol = 'ACME'"),
               error);
}

TEST(SecmasterSchema, ASymbolNamesOneListingPerVenueAndSourceAtATime) {
  database secmaster = secmaster_with_two_listings_per_venue();
  EXPECT_THROW(secmaster.run_script("INSERT INTO listing_symbol (listing_id, source, symbol, "
                                    "valid_from) VALUES (2, 'VENUE', 'ACME', '2023-01-01')"),
               error);
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO listing_symbol (listing_id, source, symbol, "
                                       "valid_from) VALUES (3, 'VENUE', 'ACME', '2023-01-01')"));
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO listing_symbol (listing_id, source, symbol, "
                                       "valid_from) VALUES (2, 'VENDOR', 'ACME', '2023-01-01')"));

  secmaster.run_script("UPDATE listing_symbol SET valid_until = '2024-06-01' WHERE listing_id = 1");
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO listing_symbol (listing_id, source, symbol, "
                                       "valid_from) VALUES (2, 'VENUE', 'ACME', '2024-06-01')"));
  EXPECT_THROW(secmaster.run_script("UPDATE listing_symbol SET valid_until = NULL "
                                    "WHERE listing_id = 1"),
               error);
}

TEST(SecmasterSchema, AListingHasOneSpecAtATime) {
  database secmaster = secmaster_with_two_listings_per_venue();
  secmaster.run_script("INSERT INTO listing_spec (listing_id, valid_from, valid_until, "
                       "lot_size_nanos, tick_rule_id) VALUES "
                       "(1, '2020-01-01', '2022-01-01', 1000000000, 1)");
  EXPECT_THROW(secmaster.run_script("INSERT INTO listing_spec (listing_id, valid_from, "
                                    "lot_size_nanos, tick_rule_id) VALUES "
                                    "(1, '2021-12-31', 1000000000, 2)"),
               error);
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO listing_spec (listing_id, valid_from, "
                                       "lot_size_nanos, tick_rule_id) VALUES "
                                       "(1, '2022-01-01', 1000000000, 2)"));
}

TEST(SecmasterSchema, AnInstrumentHasOneIdentifierPerSchemeAtATime) {
  database secmaster = secmaster_with_two_listings_per_venue();
  secmaster.run_script("INSERT INTO instrument_identifier (instrument_id, scheme, value, "
                       "valid_from) VALUES (1, 'ISIN', 'ZZ0000000001', '2020-01-01')");
  EXPECT_THROW(secmaster.run_script("INSERT INTO instrument_identifier (instrument_id, scheme, "
                                    "value, valid_from, valid_until) VALUES "
                                    "(1, 'ISIN', 'ZZ0000000002', '2019-01-01', '2020-01-02')"),
               error);
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO instrument_identifier (instrument_id, scheme, "
                                       "value, valid_from, valid_until) VALUES "
                                       "(1, 'ISIN', 'ZZ0000000002', '2019-01-01', '2020-01-01')"));
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO instrument_identifier (instrument_id, scheme, "
                                       "value, valid_from) VALUES "
                                       "(1, 'FIGI', 'ZZG000000001', '2020-01-01')"));
}

TEST(SecmasterSchema, AnIdentifierNamesOneInstrumentPerSchemeAtATime) {
  database secmaster = secmaster_with_two_listings_per_venue();
  secmaster.run_script("INSERT INTO instrument_identifier (instrument_id, scheme, value, "
                       "valid_from, valid_until) VALUES "
                       "(1, 'ISIN', 'ZZ0000000001', '2020-01-01', '2022-01-01')");
  EXPECT_THROW(secmaster.run_script("INSERT INTO instrument_identifier (instrument_id, scheme, "
                                    "value, valid_from) VALUES "
                                    "(2, 'ISIN', 'ZZ0000000001', '2021-01-01')"),
               error);
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO instrument_identifier (instrument_id, scheme, "
                                       "value, valid_from) VALUES "
                                       "(2, 'FIGI', 'ZZ0000000001', '2021-01-01')"));
  EXPECT_NO_THROW(secmaster.run_script("INSERT INTO instrument_identifier (instrument_id, scheme, "
                                       "value, valid_from) VALUES "
                                       "(2, 'ISIN', 'ZZ0000000001', '2022-01-01')"));
}

TEST(SecmasterSchema, AListingStaysOnItsVenue) {
  database secmaster = secmaster_with_two_listings_per_venue();
  EXPECT_THROW(secmaster.run_script("UPDATE listing SET venue_id = 2 WHERE listing_id = 2"),
               error);
  EXPECT_NO_THROW(secmaster.run_script("UPDATE listing SET venue_id = 1, currency = 'ALT' "
                                       "WHERE listing_id = 2"));
}

struct date_column {
  std::string table;
  std::string column;
};

database secmaster_with_a_listed_future_and_pair() {
  database secmaster = secmaster_with_two_listings_per_venue();
  secmaster.run_script(R"sql(
INSERT INTO issuer (name) VALUES ('Acme Holdings');
INSERT INTO instrument (instrument_id, asset_class, name) VALUES
  (3, 'FUTURE', 'Acme Future'), (4, 'CRYPTO', 'FOO/BAR');
INSERT INTO future_spec VALUES (3, 1, 'AC', '2026-12-18', 1000000000, 'CASH');
INSERT INTO pair_spec VALUES (4, 'FOO', 'BAR');
INSERT INTO instrument_identifier (instrument_id, scheme, value, valid_from)
  VALUES (1, 'ISIN', 'ZZ0000000001', '2020-01-01');
)sql");
  return secmaster;
}

TEST(SecmasterSchema, AListingIsDelistedOnce) {
  database secmaster = secmaster_with_two_listings_per_venue();
  EXPECT_NO_THROW(secmaster.run_script("UPDATE listing SET delisted_on = '2025-01-01' "
                                       "WHERE listing_id = 2"));
  EXPECT_THROW(secmaster.run_script("UPDATE listing SET delisted_on = '2026-01-01' "
                                    "WHERE listing_id = 2"),
               error);
  EXPECT_THROW(secmaster.run_script("UPDATE listing SET delisted_on = NULL WHERE listing_id = 2"),
               error);
}

struct validity_table {
  std::string table;
  std::string insert_columns;
  std::string insert_values;
};

TEST(SecmasterSchema, SymbolsAndSpecsAreValidOnlyWhileTheirListingIsListed) {
  const validity_table validity_tables[] = {
      {"listing_symbol", "listing_id, source, symbol", "2, 'VENUE', 'BOLT'"},
      {"listing_spec", "listing_id, lot_size_nanos, tick_rule_id", "2, 1000000000, 1"},
  };
  for (const auto& [table, insert_columns, insert_values] : validity_tables) {
    database secmaster = secmaster_with_two_listings_per_venue();
    const auto insert_valid = [&](const std::string& from, const std::string& until) {
      secmaster.run_script("INSERT INTO " + table + " (" + insert_columns +
                           ", valid_from, valid_until) VALUES (" + insert_values + ", " + from +
                           ", " + until + ")");
    };
    EXPECT_THROW(insert_valid("'2019-12-31'", "NULL"), error) << table;
    EXPECT_NO_THROW(insert_valid("'2020-01-01'", "'2021-01-01'")) << table;

    secmaster.run_script("UPDATE listing SET delisted_on = '2025-01-01' WHERE listing_id = 2");
    EXPECT_THROW(insert_valid("'2021-01-01'", "NULL"), error) << table;
    EXPECT_THROW(insert_valid("'2021-01-01'", "'2025-01-02'"), error) << table;
    EXPECT_NO_THROW(insert_valid("'2021-01-01'", "'2025-01-01'")) << table;
    EXPECT_THROW(secmaster.run_script("UPDATE " + table + " SET valid_until = NULL "
                                      "WHERE listing_id = 2 AND valid_from = '2021-01-01'"),
                 error)
        << table;
  }
}

TEST(SecmasterSchema, LookupKeysAndNamesAreNotEmpty) {
  database secmaster = secmaster_with_a_listed_future_and_pair();
  const date_column text_columns[] = {
      {"venue", "code"},           {"venue", "name"},
      {"venue", "timezone"},       {"issuer", "name"},
      {"instrument", "name"},      {"instrument_identifier", "scheme"},
      {"instrument_identifier", "value"}, {"future_spec", "root"},
      {"pair_spec", "base_currency"},     {"pair_spec", "quote_currency"},
      {"listing", "currency"},     {"listing_symbol", "source"},
      {"listing_symbol", "symbol"}, {"tick_rule", "name"},
  };
  for (const auto& [table, column] : text_columns) {
    EXPECT_THROW(secmaster.run_script("UPDATE " + table + " SET " + column + " = ''"), error)
        << table << "." << column;
  }
}

TEST(SecmasterSchema, ACorporateActionCarriesTheFieldsOfItsType) {
  database secmaster = secmaster_with_two_listings_per_venue();
  const auto record = [&](const std::string& type, const std::string& cash,
                          const std::string& currency, const std::string& new_shares,
                          const std::string& old_shares) {
    secmaster.run_script(
        "INSERT INTO corporate_action (instrument_id, action_type, ex_date, "
        "cash_per_share_nanos, currency, new_shares, old_shares) VALUES (1, '" + type +
        "', '2024-06-03', " + cash + ", " + currency + ", " + new_shares + ", " + old_shares + ")");
  };
  EXPECT_NO_THROW(record("SPLIT", "NULL", "NULL", "3", "2"));
  EXPECT_THROW(record("SPLIT", "NULL", "NULL", "NULL", "NULL"), error);
  EXPECT_THROW(record("STOCK_DIVIDEND", "NULL", "NULL", "NULL", "NULL"), error);
  EXPECT_THROW(record("SPLIT", "NULL", "NULL", "3", "NULL"), error);
  EXPECT_THROW(record("SPLIT", "NULL", "NULL", "NULL", "2"), error);
  EXPECT_THROW(record("SPLIT", "NULL", "NULL", "0", "2"), error);

  EXPECT_NO_THROW(record("CASH_DIVIDEND", "250000000", "'TST'", "NULL", "NULL"));
  EXPECT_THROW(record("CASH_DIVIDEND", "NULL", "NULL", "NULL", "NULL"), error);
  EXPECT_THROW(record("CASH_DIVIDEND", "250000000", "NULL", "NULL", "NULL"), error);
  EXPECT_THROW(record("CASH_DIVIDEND", "0", "'TST'", "NULL", "NULL"), error);
  EXPECT_THROW(record("MERGER", "NULL", "'TST'", "NULL", "NULL"), error);
  EXPECT_NO_THROW(record("MERGER", "NULL", "NULL", "NULL", "NULL"));
}

database secmaster_with_a_row_in_every_dated_table() {
  database secmaster = secmaster_with_two_listings_per_venue();
  secmaster.run_script(R"sql(
INSERT INTO instrument (instrument_id, asset_class, name) VALUES
  (3, 'FUTURE', 'Acme Future'), (4, 'OPTION', 'Acme Call');
INSERT INTO future_spec VALUES (3, 1, 'AC', '2026-12-18', 1000000000, 'CASH');
INSERT INTO option_spec VALUES (4, 1, '2026-12-18', 1000000000, 'CALL', 'AMERICAN', 1000000000);
INSERT INTO listing_spec (listing_id, valid_from, lot_size_nanos, tick_rule_id)
  VALUES (1, '2020-01-01', 1000000000, 1);
INSERT INTO instrument_identifier (instrument_id, scheme, value, valid_from)
  VALUES (1, 'ISIN', 'ZZ0000000001', '2020-01-01');
INSERT INTO corporate_action (instrument_id, action_type, ex_date, announce_date, record_date,
                              pay_date)
  VALUES (1, 'MERGER', '2024-06-03', '2024-05-01', '2024-06-04', '2024-06-05');
)sql");
  return secmaster;
}

TEST(SecmasterSchema, EveryDateColumnHoldsAnIsoCalendarDay) {
  database secmaster = secmaster_with_a_row_in_every_dated_table();
  const date_column date_columns[] = {
      {"instrument_identifier", "valid_from"}, {"instrument_identifier", "valid_until"},
      {"future_spec", "expiry"},               {"option_spec", "expiry"},
      {"listing", "listed_on"},                {"listing", "delisted_on"},
      {"listing_symbol", "valid_from"},        {"listing_symbol", "valid_until"},
      {"listing_spec", "valid_from"},          {"listing_spec", "valid_until"},
      {"corporate_action", "ex_date"},         {"corporate_action", "announce_date"},
      {"corporate_action", "record_date"},     {"corporate_action", "pay_date"},
  };
  for (const auto& [table, column] : date_columns) {
    const std::string set_date = "UPDATE " + table + " SET " + column + " = ";
    EXPECT_THROW(secmaster.run_script(set_date + "'2025-6-1'"), error) << table << "." << column;
    EXPECT_THROW(secmaster.run_script(set_date + "'2025-02-30'"), error) << table << "." << column;
    EXPECT_THROW(secmaster.run_script(set_date + "'2025-06-01 00:00:00'"), error)
        << table << "." << column;
    EXPECT_THROW(secmaster.run_script(set_date + "20250601"), error) << table << "." << column;
  }
}

struct validity_interval {
  std::string table;
  std::string from;
  std::string until;
};

TEST(SecmasterSchema, EveryValidityIntervalIsNotEmpty) {
  database secmaster = secmaster_with_a_row_in_every_dated_table();
  const validity_interval intervals[] = {
      {"instrument_identifier", "valid_from", "valid_until"},
      {"listing", "listed_on", "delisted_on"},
      {"listing_symbol", "valid_from", "valid_until"},
      {"listing_spec", "valid_from", "valid_until"},
  };
  for (const auto& [table, from, until] : intervals) {
    const std::string set_until = "UPDATE " + table + " SET " + until + " = ";
    EXPECT_THROW(secmaster.run_script(set_until + from), error) << table;
    EXPECT_THROW(secmaster.run_script(set_until + "date(" + from + ", '-1 day')"), error) << table;
    EXPECT_NO_THROW(secmaster.run_script(set_until + "date(" + from + ", '+1 day')")) << table;
  }
}

TEST(SecmasterSchema, RowsMustReferenceExistingParents) {
  database secmaster = secmaster_with_two_listings_per_venue();
  EXPECT_THROW(secmaster.run_script("INSERT INTO listing (instrument_id, venue_id, currency, "
                                    "listed_on) VALUES (99, 1, 'TST', '2020-01-01')"),
               error);
  EXPECT_THROW(secmaster.run_script("DELETE FROM instrument WHERE instrument_id = 1"), error);
}

struct permanent_id {
  std::string table;
  std::string id_column;
  std::string insert_one_row;
};

TEST(SecmasterSchema, PermanentIdsAreNeverReused) {
  database secmaster = secmaster_with_two_listings_per_venue();
  const permanent_id permanent_ids[] = {
      {"venue", "venue_id",
       "INSERT INTO venue (code, name, timezone) VALUES ('XNEW', 'New Exchange', 'Etc/UTC')"},
      {"issuer", "issuer_id", "INSERT INTO issuer (name) VALUES ('Acme Holdings')"},
      {"instrument", "instrument_id",
       "INSERT INTO instrument (asset_class, name) VALUES ('EQUITY', 'Cog')"},
      {"listing", "listing_id",
       "INSERT INTO listing (instrument_id, venue_id, currency, listed_on) "
       "VALUES (2, 2, 'TST', '2020-01-01')"},
      {"tick_rule", "tick_rule_id", "INSERT INTO tick_rule (name) VALUES ('NEW')"},
      {"corporate_action", "corporate_action_id",
       "INSERT INTO corporate_action (instrument_id, action_type, ex_date) "
       "VALUES (1, 'MERGER', '2024-06-03')"},
  };
  for (const auto& [table, id_column, insert_one_row] : permanent_ids) {
    const std::string newest_id = "SELECT max(" + id_column + ") FROM " + table;
    secmaster.run_script(insert_one_row);
    nqdb::sqlite::statement deleted = secmaster.prepare(newest_id);
    ASSERT_TRUE(deleted.next_row());
    const auto deleted_id = deleted.get<std::int64_t>(0);
    secmaster.run_script("DELETE FROM " + table + " WHERE " + id_column + " = " +
                         std::to_string(deleted_id));

    secmaster.run_script(insert_one_row);
    nqdb::sqlite::statement reinserted = secmaster.prepare(newest_id);
    ASSERT_TRUE(reinserted.next_row());
    EXPECT_GT(reinserted.get<std::int64_t>(0), deleted_id) << table;
  }
}

TEST(SecmasterSchema, ArchiveWritesACopyNamedByDate) {
  const nqdb_test::temporary_directory directory;
  database secmaster = secmaster_with_two_listings_per_venue();

  const std::filesystem::path dated_copy =
      nqdb::secmaster::archive(secmaster, directory.path(), std::chrono::sys_days{2026y / 11 / 19});

  EXPECT_EQ(dated_copy, directory.path() / "secmaster-2026-11-19.db");
  database reopened = database::open_read_only(dated_copy);
  nqdb::sqlite::statement row = reopened.prepare("SELECT count(*) FROM listing");
  ASSERT_TRUE(row.next_row());
  EXPECT_EQ(row.get<std::int64_t>(0), 3);
}

}  // namespace
