#include "cli.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include <nq/db/sqlite.h>

#include "temporary_directory.h"

namespace {

struct outcome {
  int exit_code;
  std::string printed;
  std::string complaint;
};

outcome run_nqdb(std::vector<const char*> arguments) {
  arguments.insert(arguments.begin(), "nqdb");
  std::ostringstream out;
  std::ostringstream err;
  const int exit_code =
      nq::db::cli::run(static_cast<int>(arguments.size()), arguments.data(), out, err);
  return {exit_code, out.str(), err.str()};
}

class Cli : public testing::Test {
 protected:
  std::filesystem::path database_file() const { return directory_.path() / "secmaster.db"; }

  outcome nqdb(std::initializer_list<const char*> arguments) const {
    const std::string file = database_file().string();
    std::vector<const char*> with_database{"--db", file.c_str()};
    with_database.insert(with_database.end(), arguments);
    return run_nqdb(with_database);
  }

  void expect_prints(std::initializer_list<const char*> arguments,
                     const std::string& printed) const {
    const outcome outcome = nqdb(arguments);
    EXPECT_EQ(outcome.exit_code, 0) << outcome.complaint;
    EXPECT_EQ(outcome.printed, printed);
    EXPECT_EQ(outcome.complaint, "");
  }

  void expect_fails(std::initializer_list<const char*> arguments) const {
    const outcome outcome = nqdb(arguments);
    EXPECT_NE(outcome.exit_code, 0);
    EXPECT_EQ(outcome.printed, "");
    EXPECT_NE(outcome.complaint, "");
  }

  std::int64_t stored(const std::string& table_and_filter) const {
    nq::db::sqlite::database database = nq::db::sqlite::database::open_read_only(database_file());
    nq::db::sqlite::statement row = database.prepare("SELECT count(*) FROM " + table_and_filter);
    row.next_row();
    return row.get<std::int64_t>(0);
  }

  void add_acme_on_the_test_exchange() const {
    nqdb({"init"});
    expect_prints({"venue", "add", "--code", "XTST", "--name", "Test Exchange", "--timezone",
                   "Etc/UTC", "--mic", "XTST", "--country", "ZZ"},
                  "1\n");
    expect_prints({"tick-rule", "add", "--name", "BANDED", "--band", "0:0.0001", "--band",
                   "1:0.01"},
                  "1\n");
    expect_prints({"instrument", "add", "--asset-class", "EQUITY", "--name", "Acme"}, "1\n");
    expect_prints({"listing", "add", "--instrument", "1", "--venue", "XTST", "--currency", "TST",
                   "--symbol", "ACME", "--listed-on", "2020-01-01", "--lot-size", "100",
                   "--min-qty", "200", "--tick-rule", "BANDED", "--primary"},
                  "1\n");
  }

  nqdb_test::temporary_directory directory_;
};

constexpr const char* listing_columns =
    "listing_id\tinstrument_id\tvenue\tsymbol\tasset_class\tcurrency\tlot_size\ttick_rule\tname\n";
constexpr const char* acme_row = "1\t1\tXTST\tACME\tEQUITY\tTST\t100\tBANDED\tAcme\n";

TEST_F(Cli, InitCreatesTheDatabaseOnce) {
  expect_prints({"init"}, database_file().string() + "\n");
  EXPECT_TRUE(std::filesystem::exists(database_file()));
  expect_fails({"init"});
}

TEST_F(Cli, InitCreatesMissingDirectoriesOfTheDatabaseFile) {
  const std::string nested = (directory_.path() / "a" / "b" / "secmaster.db").string();
  EXPECT_EQ(run_nqdb({"--db", nested.c_str(), "init"}).exit_code, 0);
  EXPECT_TRUE(std::filesystem::exists(nested));
}

class CliInWorkingDirectory : public Cli {
 protected:
  CliInWorkingDirectory() : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(directory_.path());
  }
  ~CliInWorkingDirectory() override { std::filesystem::current_path(previous_); }

  std::filesystem::path previous_;
};

TEST_F(CliInWorkingDirectory, ABareFileNameIsInTheWorkingDirectory) {
  const outcome outcome = run_nqdb({"--db", "secmaster.db", "init"});
  EXPECT_EQ(outcome.exit_code, 0) << outcome.complaint;
  EXPECT_TRUE(std::filesystem::exists(database_file()));
  EXPECT_EQ(run_nqdb({"--db", "secmaster.db", "archive", "--date", "2026-11-19"}).exit_code, 0);
  EXPECT_TRUE(std::filesystem::exists(directory_.path() / "archive" / "secmaster-2026-11-19.db"));
}

class CliWithEnvironment : public Cli {
 protected:
  CliWithEnvironment() : previous_home_(std::getenv("HOME")) {
    setenv("HOME", directory_.path().c_str(), 1);
  }
  ~CliWithEnvironment() override {
    setenv("HOME", previous_home_.c_str(), 1);
    unsetenv("NQDB_PATH");
  }

  std::string previous_home_;
};

TEST_F(CliWithEnvironment, TheDatabaseDefaultsToAFileUnderHome) {
  EXPECT_EQ(run_nqdb({"init"}).exit_code, 0);
  EXPECT_TRUE(std::filesystem::exists(directory_.path() / ".nqdb" / "secmaster.db"));
}

TEST_F(CliWithEnvironment, NqdbPathNamesTheDatabaseAndTheOptionWinsOverIt) {
  const std::string from_environment = (directory_.path() / "environment.db").string();
  setenv("NQDB_PATH", from_environment.c_str(), 1);
  EXPECT_EQ(run_nqdb({"init"}).exit_code, 0);
  EXPECT_TRUE(std::filesystem::exists(from_environment));

  expect_prints({"init"}, database_file().string() + "\n");
  EXPECT_FALSE(std::filesystem::exists(directory_.path() / ".nqdb"));
}

TEST_F(Cli, CommandsFailWithoutADatabase) {
  expect_fails({"listing", "list"});
  expect_fails({"venue", "add", "--code", "XTST", "--name", "Test Exchange", "--timezone",
                "Etc/UTC"});
  EXPECT_FALSE(std::filesystem::exists(database_file()));
}

TEST_F(Cli, CommandsRejectADatabaseOfAnotherSchemaVersion) {
  add_acme_on_the_test_exchange();
  {
    nq::db::sqlite::database database = nq::db::sqlite::database::open(database_file());
    database.run_script("PRAGMA user_version = 999");
  }
  expect_fails({"listing", "list"});
  expect_fails({"instrument", "add", "--asset-class", "EQUITY", "--name", "Bolt"});
}

TEST_F(Cli, AddCommandsStoreEveryOption) {
  add_acme_on_the_test_exchange();
  EXPECT_EQ(stored("venue WHERE code = 'XTST' AND name = 'Test Exchange' AND mic = 'XTST' "
                   "AND timezone = 'Etc/UTC' AND country = 'ZZ'"),
            1);
  EXPECT_EQ(stored("tick_band WHERE tick_rule_id = 1 AND "
                   "((price_from_nanos = 0 AND tick_size_nanos = 100000) OR "
                   " (price_from_nanos = 1000000000 AND tick_size_nanos = 10000000))"),
            2);
  EXPECT_EQ(stored("listing WHERE instrument_id = 1 AND venue_id = 1 AND currency = 'TST' "
                   "AND is_primary = 1 AND listed_on = '2020-01-01' AND delisted_on IS NULL"),
            1);
  EXPECT_EQ(stored("listing_symbol WHERE listing_id = 1 AND source = 'VENUE' AND symbol = 'ACME' "
                   "AND valid_from = '2020-01-01' AND valid_until IS NULL"),
            1);
  EXPECT_EQ(stored("listing_spec WHERE listing_id = 1 AND valid_from = '2020-01-01' "
                   "AND lot_size_nanos = 100000000000 AND min_qty_nanos = 200000000000 "
                   "AND tick_rule_id = 1"),
            1);

  expect_prints({"instrument", "add", "--asset-class", "ETF", "--name", "Bolt Fund"}, "2\n");
  expect_prints({"listing", "add", "--instrument", "2", "--venue", "XTST", "--currency", "ALT",
                 "--symbol", "BOLT", "--listed-on", "2021-02-03", "--lot-size", "1",
                 "--tick-rule", "BANDED"},
                "2\n");
  EXPECT_EQ(stored("listing WHERE listing_id = 2 AND is_primary = 0 AND currency = 'ALT'"), 1);
  EXPECT_EQ(stored("listing_spec WHERE listing_id = 2 AND min_qty_nanos IS NULL"), 1);
}

TEST_F(Cli, ListPrintsTheListingsOfTheDay) {
  add_acme_on_the_test_exchange();
  expect_prints({"listing", "list", "--date", "2020-01-01"},
                 std::string(listing_columns) + acme_row);
  expect_prints({"listing", "list", "--date", "2019-12-31"}, listing_columns);
  expect_prints({"listing", "list", "--date", "2020-01-01", "--venue", "XTST"},
                std::string(listing_columns) + acme_row);
  expect_prints({"listing", "list", "--date", "2020-01-01", "--venue", "XALT"}, listing_columns);
  expect_prints({"listing", "list", "--date", "2020-01-01", "--asset-class", "EQUITY"},
                std::string(listing_columns) + acme_row);
  expect_prints({"listing", "list", "--date", "2020-01-01", "--asset-class", "ETF"},
                 listing_columns);
}

TEST_F(Cli, CheckCountsWhatLoadsOnTheDayGiven) {
  add_acme_on_the_test_exchange();
  expect_prints({"check", "--date", "2020-01-01"},
                "1 listings of 1 instruments load on 2020-01-01\n");
  expect_prints({"check", "--date", "2019-12-31"},
                "0 listings of 0 instruments load on 2019-12-31\n");
  expect_prints({"check", "--date", "2020-01-01", "--asset-class", "ETF"},
                "0 listings of 0 instruments load on 2020-01-01\n");
}

TEST_F(Cli, FindFollowsARename) {
  add_acme_on_the_test_exchange();
  expect_prints({"listing", "assign-symbol", "--listing", "1", "--symbol", "ACMX", "--from",
                 "2024-06-01"},
                "");
  expect_prints({"listing", "find", "--venue", "XTST", "--symbol", "ACME", "--date",
                 "2024-05-31"}, "1\n");
  expect_prints({"listing", "find", "--venue", "XTST", "--symbol", "ACMX", "--date",
                 "2024-06-01"}, "1\n");
  expect_fails({"listing", "find", "--venue", "XTST", "--symbol", "ACME", "--date", "2024-06-01"});
  expect_fails({"listing", "find", "--venue", "NONE", "--symbol", "ACMX", "--date", "2024-06-01"});
}

TEST_F(Cli, FindReadsTheSymbolOfTheSourceGiven) {
  add_acme_on_the_test_exchange();
  expect_prints({"listing", "assign-symbol", "--listing", "1", "--source", "VENDOR", "--symbol",
                 "ACME.T", "--from", "2020-01-01"},
                "");
  EXPECT_EQ(stored("listing_symbol WHERE source = 'VENDOR' AND symbol = 'ACME.T'"), 1);
  expect_prints({"listing", "find", "--venue", "XTST", "--source", "VENDOR", "--symbol", "ACME.T",
                 "--date",
                 "2020-01-01"},
                "1\n");
  expect_fails({"listing", "find", "--venue", "XTST", "--symbol", "ACME.T", "--date",
                "2020-01-01"});
  expect_fails({"listing", "find", "--venue", "XTST", "--source", "VENDOR", "--symbol", "ACME",
                "--date",
                "2020-01-01"});
}

constexpr const char* period_columns =
    "listing_id\tvenue\tkind\tsource\tvalid_from\tvalid_until\tsymbol\tlot_size\tmin_qty\t"
    "tick_rule\n";

class CliWithARenamedAndDelistedListing : public Cli {
 protected:
  CliWithARenamedAndDelistedListing() {
    add_acme_on_the_test_exchange();
    nqdb({"listing", "assign-symbol", "--listing", "1", "--source", "VENDOR", "--symbol", "ACME.T",
          "--from", "2020-01-01"});
    nqdb({"listing", "assign-symbol", "--listing", "1", "--symbol", "ACMX", "--from",
          "2024-06-01"});
    nqdb({"listing", "assign-spec", "--listing", "1", "--from", "2024-06-01", "--lot-size", "1",
          "--tick-rule", "BANDED"});
    nqdb({"listing", "delist", "--listing", "1", "--delisted-on", "2025-01-01"});
  }
};

TEST_F(CliWithARenamedAndDelistedListing, HistoryPrintsEveryPeriodOfEveryListing) {
  expect_prints({"listing", "history"},
                std::string(period_columns) +
                    "1\tXTST\tLISTING\t\t2020-01-01\t2025-01-01\t\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENDOR\t2020-01-01\t2025-01-01\tACME.T\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENUE\t2020-01-01\t2024-06-01\tACME\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENUE\t2024-06-01\t2025-01-01\tACMX\t\t\t\n"
                    "1\tXTST\tSPEC\t\t2020-01-01\t2024-06-01\t\t100\t200\tBANDED\n"
                    "1\tXTST\tSPEC\t\t2024-06-01\t2025-01-01\t\t1\t\tBANDED\n");
}

TEST_F(CliWithARenamedAndDelistedListing, HistoryReadsTheDaysAndTheUniverseGiven) {
  expect_prints({"listing", "history", "--until", "2024-06-01", "--venue", "XTST",
                 "--asset-class", "EQUITY"},
                std::string(period_columns) +
                    "1\tXTST\tLISTING\t\t2020-01-01\t2025-01-01\t\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENDOR\t2020-01-01\t2025-01-01\tACME.T\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENUE\t2020-01-01\t2024-06-01\tACME\t\t\t\n"
                    "1\tXTST\tSPEC\t\t2020-01-01\t2024-06-01\t\t100\t200\tBANDED\n");
  expect_prints({"listing", "history", "--from", "2024-06-01", "--until", "2024-06-02"},
                std::string(period_columns) +
                    "1\tXTST\tLISTING\t\t2020-01-01\t2025-01-01\t\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENDOR\t2020-01-01\t2025-01-01\tACME.T\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENUE\t2024-06-01\t2025-01-01\tACMX\t\t\t\n"
                    "1\tXTST\tSPEC\t\t2024-06-01\t2025-01-01\t\t1\t\tBANDED\n");
  expect_prints({"listing", "history", "--from", "2025-01-01"}, period_columns);
  expect_prints({"listing", "history", "--until", "2020-01-01"}, period_columns);
  expect_prints({"listing", "history", "--venue", "XALT"}, period_columns);
  expect_prints({"listing", "history", "--asset-class", "ETF"}, period_columns);
}

TEST_F(CliWithARenamedAndDelistedListing, HistoryRejectsDaysThatAreNotARange) {
  expect_fails({"listing", "history", "--from", "2024-06-01", "--until", "2024-06-01"});
  expect_fails({"listing", "history", "--from", "2024-06-02", "--until", "2024-06-01"});
  expect_fails({"listing", "history", "--from", "June"});
}

TEST_F(Cli, AmendCorrectsOnlyTheOptionsGiven) {
  add_acme_on_the_test_exchange();
  expect_prints({"venue", "amend", "--venue", "XTST", "--name", "Testing Exchange"}, "");
  EXPECT_EQ(stored("venue WHERE code = 'XTST' AND name = 'Testing Exchange' AND mic = 'XTST' "
                   "AND timezone = 'Etc/UTC' AND country = 'ZZ'"),
            1);
  expect_prints({"venue", "amend", "--venue", "XTST", "--code", "XNEW", "--mic", "XNEW",
                 "--country", "YY", "--timezone", "Etc/GMT"},
                "");
  EXPECT_EQ(stored("venue WHERE code = 'XNEW' AND name = 'Testing Exchange' AND mic = 'XNEW' "
                   "AND timezone = 'Etc/GMT' AND country = 'YY'"),
            1);
  expect_fails({"venue", "amend", "--venue", "XTST", "--name", "Test Exchange"});

  expect_prints({"issuer", "add", "--name", "Acme Holdngs", "--country", "ZZ"}, "1\n");
  expect_prints({"issuer", "amend", "--issuer", "1", "--name", "Acme Holdings", "--lei",
                 "ABCDEFGHIJ0123456789"},
                "");
  EXPECT_EQ(stored("issuer WHERE name = 'Acme Holdings' AND lei = 'ABCDEFGHIJ0123456789' "
                   "AND country = 'ZZ'"),
            1);
  expect_fails({"issuer", "amend", "--issuer", "9", "--name", "None"});

  expect_prints({"instrument", "amend", "--instrument", "1", "--name", "Acme Fund",
                 "--asset-class", "ETF", "--issuer", "1", "--cfi", "CEOGMS"},
                "");
  EXPECT_EQ(stored("instrument WHERE instrument_id = 1 AND name = 'Acme Fund' "
                   "AND asset_class = 'ETF' AND issuer_id = 1 AND cfi = 'CEOGMS'"),
            1);
  expect_fails({"instrument", "amend", "--instrument", "1", "--asset-class", "FUTURE"});
  expect_fails({"instrument", "amend", "--instrument", "9", "--name", "None"});
  EXPECT_EQ(stored("instrument WHERE asset_class = 'ETF'"), 1);
}

TEST_F(Cli, TickPrintsTheTickSizeOfASymbolAtAPrice) {
  add_acme_on_the_test_exchange();
  expect_prints({"tick-rule", "add", "--name", "CENT", "--band", "0:0.01"}, "2\n");
  expect_prints({"tick-rule", "list"},
                "tick_rule_id\tname\tprice_from\ttick_size\n"
                "1\tBANDED\t0\t0.0001\n"
                "1\tBANDED\t1\t0.01\n"
                "2\tCENT\t0\t0.01\n");

  expect_prints({"listing", "tick-size", "--venue", "XTST", "--symbol", "ACME", "--price", "0.5",
                 "--date",
                 "2020-01-01"},
                "0.0001\n");
  expect_prints({"listing", "tick-size", "--venue", "XTST", "--symbol", "ACME", "--price", "1",
                 "--date",
                 "2020-01-01"},
                "0.01\n");
  expect_prints({"listing", "tick-size", "--venue", "XTST", "--symbol", "ACME", "--price",
                 "187.5"}, "0.01\n");

  nqdb({"listing", "assign-spec", "--listing", "1", "--from", "2024-06-01", "--lot-size", "1",
        "--tick-rule", "CENT"});
  nqdb({"listing", "assign-symbol", "--listing", "1", "--source", "VENDOR", "--symbol", "ACME.T",
        "--from", "2020-01-01"});
  expect_prints({"listing", "tick-size", "--venue", "XTST", "--symbol", "ACME", "--price", "0.5",
                 "--date",
                 "2024-05-31"},
                "0.0001\n");
  expect_prints({"listing", "tick-size", "--venue", "XTST", "--symbol", "ACME.T", "--source",
                 "VENDOR", "--price",
                 "0.5", "--date", "2024-06-01"},
                "0.01\n");

  expect_fails({"listing", "tick-size", "--venue", "XTST", "--symbol", "NONE", "--price", "1"});
  expect_fails({"listing", "tick-size", "--venue", "NONE", "--symbol", "ACME", "--price", "1"});
  expect_fails({"listing", "tick-size", "--venue", "XTST", "--symbol", "ACME", "--price", "one"});
  expect_fails({"listing", "tick-size", "--venue", "XTST", "--symbol", "ACME", "--price", "1",
                "--date",
                "2019-12-31"});
  expect_fails({"listing", "tick-size", "--venue", "XTST", "--symbol", "ACME"});
}

TEST_F(Cli, TickRuleAmendRenamesTheRuleAndReplacesEveryBand) {
  add_acme_on_the_test_exchange();
  expect_prints({"tick-rule", "amend", "--tick-rule", "BANDED", "--name", "CENT"}, "");
  EXPECT_EQ(stored("tick_rule WHERE name = 'CENT'"), 1);
  EXPECT_EQ(stored("tick_band WHERE tick_rule_id = 1"), 2);

  expect_prints({"tick-rule", "amend", "--tick-rule", "CENT", "--band", "0:0.01"}, "");
  EXPECT_EQ(stored("tick_band WHERE tick_rule_id = 1"), 1);
  EXPECT_EQ(stored("tick_band WHERE price_from_nanos = 0 AND tick_size_nanos = 10000000"), 1);
  expect_fails({"tick-rule", "amend", "--tick-rule", "BANDED", "--name", "NONE"});
  expect_fails({"tick-rule", "amend", "--tick-rule", "CENT", "--band", "0.01"});
  EXPECT_EQ(stored("tick_band WHERE tick_rule_id = 1"), 1);
}

TEST_F(Cli, AmendReplacesTheSpecOfAFutureAnOptionAndAPair) {
  add_acme_on_the_test_exchange();
  nqdb({"instrument", "add-future", "--name", "Acme Future", "--underlying", "1", "--root", "AC",
        "--expiry", "2026-12-18", "--multiplier", "50", "--settlement", "PHYSICAL"});
  nqdb({"instrument", "add-option", "--name", "Acme Put", "--underlying", "1", "--expiry",
        "2026-11-20", "--strike", "12.5", "--right", "PUT", "--style", "EUROPEAN",
        "--multiplier", "100"});
  nqdb({"instrument", "add-pair", "--asset-class", "CRYPTO", "--name", "FOO/BAR", "--base",
        "FOO", "--quote", "BAR"});

  expect_prints({"instrument", "amend-future", "--instrument", "2", "--underlying", "1",
                 "--root", "AD", "--expiry", "2026-11-20", "--multiplier", "25", "--settlement",
                 "CASH"},
                "");
  EXPECT_EQ(stored("future_spec WHERE instrument_id = 2 AND root = 'AD' "
                   "AND expiry = '2026-11-20' AND multiplier_nanos = 25000000000 "
                   "AND settlement = 'CASH'"),
            1);

  expect_prints({"instrument", "amend-option", "--instrument", "3", "--underlying", "1",
                 "--expiry", "2026-12-18", "--strike", "15", "--right", "CALL", "--style",
                 "AMERICAN", "--multiplier", "10"},
                "");
  EXPECT_EQ(stored("option_spec WHERE instrument_id = 3 AND expiry = '2026-12-18' "
                   "AND strike_nanos = 15000000000 AND option_right = 'CALL' "
                   "AND exercise_style = 'AMERICAN' AND multiplier_nanos = 10000000000"),
            1);

  expect_prints({"instrument", "amend-pair", "--instrument", "4", "--base", "BAZ", "--quote",
                 "QUX"},
                "");
  EXPECT_EQ(stored("pair_spec WHERE instrument_id = 4 AND base_currency = 'BAZ' "
                   "AND quote_currency = 'QUX'"),
            1);

  expect_fails({"instrument", "amend-pair", "--instrument", "2", "--base", "BAZ", "--quote",
                "QUX"});
  expect_fails({"instrument", "amend-future", "--instrument", "2", "--root", "AE"});
  EXPECT_EQ(stored("future_spec WHERE root = 'AD'"), 1);
  EXPECT_EQ(stored("pair_spec"), 1);
}

TEST_F(CliWithARenamedAndDelistedListing, ListingAmendMovesItsDaysWithItsSymbolsAndSpecs) {
  expect_prints({"listing", "amend", "--listing", "1", "--listed-on", "2019-06-01",
                 "--delisted-on", "2025-03-01", "--currency", "ALT", "--not-primary"},
                "");
  EXPECT_EQ(stored("listing WHERE currency = 'ALT' AND is_primary = 0"), 1);
  expect_prints({"listing", "history"},
                std::string(period_columns) +
                    "1\tXTST\tLISTING\t\t2019-06-01\t2025-03-01\t\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENDOR\t2019-06-01\t2025-03-01\tACME.T\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENUE\t2019-06-01\t2024-06-01\tACME\t\t\t\n"
                    "1\tXTST\tSYMBOL\tVENUE\t2024-06-01\t2025-03-01\tACMX\t\t\t\n"
                    "1\tXTST\tSPEC\t\t2019-06-01\t2024-06-01\t\t100\t200\tBANDED\n"
                    "1\tXTST\tSPEC\t\t2024-06-01\t2025-03-01\t\t1\t\tBANDED\n");

  expect_prints({"listing", "amend", "--listing", "1", "--primary"}, "");
  EXPECT_EQ(stored("listing WHERE currency = 'ALT' AND is_primary = 1"), 1);
  expect_fails({"listing", "amend", "--listing", "1", "--primary", "--not-primary"});
  expect_fails({"listing", "amend", "--listing", "1", "--listed-on", "2024-07-01"});
  expect_fails({"listing", "amend", "--listing", "9", "--currency", "TST"});
  EXPECT_EQ(stored("listing WHERE listed_on = '2019-06-01' AND delisted_on = '2025-03-01'"), 1);
}

TEST_F(CliWithARenamedAndDelistedListing, UndelistListsTheListingAgain) {
  expect_fails({"listing", "list", "--date", "2025-01-01", "--venue", "NONE", "--asset-class",
                "NONE"});
  expect_prints({"listing", "undelist", "--listing", "1"}, "");
  expect_prints({"listing", "find", "--venue", "XTST", "--symbol", "ACMX", "--date",
                 "2030-01-01"}, "1\n");
  EXPECT_EQ(stored("listing_symbol WHERE valid_until IS NULL"), 2);
  EXPECT_EQ(stored("listing_spec WHERE valid_until IS NULL"), 1);
  expect_fails({"listing", "undelist", "--listing", "1"});
  expect_fails({"listing", "amend", "--listing", "1", "--delisted-on", "2025-03-01"});
}

TEST_F(Cli, SetSymbolsReplacesEveryPeriodOfTheSource) {
  add_acme_on_the_test_exchange();
  nqdb({"listing", "assign-symbol", "--listing", "1", "--source", "VENDOR", "--symbol", "ACME.T",
        "--from", "2020-01-01"});

  expect_prints({"listing", "set-symbols", "--listing", "1", "--period",
                 "2020-01-01:2022-01-01:ACM", "--period", "2022-01-01::AC:ME"},
                "");
  EXPECT_EQ(stored("listing_symbol WHERE source = 'VENUE'"), 2);
  EXPECT_EQ(stored("listing_symbol WHERE source = 'VENUE' AND symbol = 'ACM' "
                   "AND valid_from = '2020-01-01' AND valid_until = '2022-01-01'"),
            1);
  EXPECT_EQ(stored("listing_symbol WHERE source = 'VENUE' AND symbol = 'AC:ME' "
                   "AND valid_from = '2022-01-01' AND valid_until IS NULL"),
            1);
  EXPECT_EQ(stored("listing_symbol WHERE source = 'VENDOR' AND symbol = 'ACME.T'"), 1);

  expect_fails({"listing", "set-symbols", "--listing", "1", "--period", "2020-01-01::ACME",
                "--period", "2021-01-01::ACMX"});
  expect_fails({"listing", "set-symbols", "--listing", "1", "--period", "2020-01-01:ACME"});
  expect_fails({"listing", "set-symbols", "--listing", "1", "--period", "January::ACME"});
  expect_fails({"listing", "set-symbols", "--listing", "9", "--period", "2020-01-01::ACME"});
  EXPECT_EQ(stored("listing_symbol WHERE source = 'VENUE' AND symbol IN ('ACM', 'AC:ME')"), 2);

  expect_prints({"listing", "set-symbols", "--listing", "1", "--source", "VENDOR"}, "");
  EXPECT_EQ(stored("listing_symbol WHERE source = 'VENDOR'"), 0);
}

TEST_F(Cli, SetSpecsReplacesEveryPeriod) {
  add_acme_on_the_test_exchange();
  expect_prints({"listing", "set-specs", "--listing", "1", "--period",
                 "2020-01-01:2022-01-01:10:20:BANDED", "--period", "2022-01-01::1::BANDED"},
                "");
  EXPECT_EQ(stored("listing_spec"), 2);
  EXPECT_EQ(stored("listing_spec WHERE valid_from = '2020-01-01' AND valid_until = '2022-01-01' "
                   "AND lot_size_nanos = 10000000000 AND min_qty_nanos = 20000000000"),
            1);
  EXPECT_EQ(stored("listing_spec WHERE valid_from = '2022-01-01' AND valid_until IS NULL "
                   "AND lot_size_nanos = 1000000000 AND min_qty_nanos IS NULL"),
            1);

  expect_fails({"listing", "set-specs", "--listing", "1", "--period", "2020-01-01::1::NONE"});
  expect_fails({"listing", "set-specs", "--listing", "1", "--period", "2020-01-01::1:BANDED"});
  expect_fails({"listing", "set-specs", "--listing", "1", "--period", "2020-01-01::lot::BANDED"});
  EXPECT_EQ(stored("listing_spec"), 2);
}

TEST_F(Cli, SetIdentifiersReplacesEveryPeriodOfTheScheme) {
  add_acme_on_the_test_exchange();
  nqdb({"instrument", "assign-identifier", "--instrument", "1", "--scheme", "ISIN", "--value",
        "ZZ0000000001", "--from", "2020-01-01"});
  nqdb({"instrument", "assign-identifier", "--instrument", "1", "--scheme", "CUSIP", "--value",
        "000000001", "--from", "2020-01-01"});

  expect_prints({"instrument", "set-identifiers", "--instrument", "1", "--scheme", "ISIN",
                 "--period", "2020-01-01:2024-06-01:ZZ0000000010", "--period",
                 "2024-06-01::ZZ0000000020"},
                "");
  EXPECT_EQ(stored("instrument_identifier WHERE scheme = 'ISIN'"), 2);
  EXPECT_EQ(stored("instrument_identifier WHERE value = 'ZZ0000000010' "
                   "AND valid_from = '2020-01-01' AND valid_until = '2024-06-01'"),
            1);
  EXPECT_EQ(stored("instrument_identifier WHERE value = 'ZZ0000000020' "
                   "AND valid_until IS NULL"),
            1);
  EXPECT_EQ(stored("instrument_identifier WHERE scheme = 'CUSIP'"), 1);
  expect_fails({"instrument", "set-identifiers", "--instrument", "9", "--scheme", "ISIN"});
}

TEST_F(Cli, ActionAmendReplacesEveryFieldAndRemoveDeletesTheAction) {
  add_acme_on_the_test_exchange();
  nqdb({"action", "add", "--instrument", "1", "--type", "SPLIT", "--ex-date", "2024-06-03",
        "--new-shares", "3", "--old-shares", "2", "--announce-date", "2024-05-01"});

  expect_prints({"action", "amend", "--action", "1", "--instrument", "1", "--type",
                 "CASH_DIVIDEND", "--ex-date", "2024-06-04", "--cash-per-share", "0.25",
                 "--currency", "TST"},
                "");
  EXPECT_EQ(stored("corporate_action WHERE corporate_action_id = 1 "
                   "AND action_type = 'CASH_DIVIDEND' AND ex_date = '2024-06-04' "
                   "AND cash_per_share_nanos = 250000000 AND currency = 'TST' "
                   "AND new_shares IS NULL AND old_shares IS NULL AND announce_date IS NULL"),
            1);
  expect_fails({"action", "amend", "--action", "1", "--instrument", "1", "--type",
                "CASH_DIVIDEND", "--ex-date", "2024-06-04"});
  expect_fails({"action", "amend", "--action", "9", "--instrument", "1", "--type", "MERGER",
                "--ex-date", "2024-06-04"});

  expect_prints({"action", "remove", "--action", "1"}, "");
  EXPECT_EQ(stored("corporate_action"), 0);
  expect_fails({"action", "remove", "--action", "1"});
}

class CliWithSplitsAndADividend : public Cli {
 protected:
  CliWithSplitsAndADividend() {
    add_acme_on_the_test_exchange();
    nqdb({"instrument", "add", "--asset-class", "EQUITY", "--name", "Bolt"});
    nqdb({"action", "add", "--instrument", "1", "--type", "SPLIT", "--ex-date", "2024-09-02",
          "--new-shares", "4", "--old-shares", "1"});
    nqdb({"action", "add", "--instrument", "1", "--type", "CASH_DIVIDEND", "--ex-date",
          "2023-06-01", "--cash-per-share", "1", "--currency", "TST", "--announce-date",
          "2023-05-02", "--record-date", "2023-06-02", "--pay-date", "2023-06-15"});
    nqdb({"action", "add", "--instrument", "1", "--type", "SPLIT", "--ex-date", "2022-03-01",
          "--new-shares", "2", "--old-shares", "1"});
    nqdb({"action", "add", "--instrument", "2", "--type", "SPINOFF", "--ex-date", "2022-03-01",
          "--related-instrument", "1"});
  }
};

constexpr const char* action_columns =
    "action_id\tinstrument_id\ttype\tex_date\tannounce_date\trecord_date\tpay_date\t"
    "cash_per_share\tcurrency\tnew_shares\told_shares\trelated_instrument_id\n";
constexpr const char* factor_columns =
    "ex_date\tprice_factor\tvolume_factor\tcumulative_price_factor\tcumulative_volume_factor\n";

TEST_F(CliWithSplitsAndADividend, ActionListPrintsTheActionsByInstrumentThenExDate) {
  const std::string actions_of_acme =
      "3\t1\tSPLIT\t2022-03-01\t\t\t\t\t\t2\t1\t\n"
      "2\t1\tCASH_DIVIDEND\t2023-06-01\t2023-05-02\t2023-06-02\t2023-06-15\t1\tTST\t\t\t\n"
      "1\t1\tSPLIT\t2024-09-02\t\t\t\t\t\t4\t1\t\n";
  expect_prints({"action", "list"},
                action_columns + actions_of_acme + "4\t2\tSPINOFF\t2022-03-01\t\t\t\t\t\t\t\t1\n");
  expect_prints({"action", "list", "--instrument", "1"}, action_columns + actions_of_acme);
  expect_prints({"action", "list", "--instrument", "9"}, action_columns);
}

TEST_F(CliWithSplitsAndADividend, ActionFactorsAdjustForSplits) {
  expect_prints({"action", "factors", "--instrument", "1"},
                std::string(factor_columns) +
                    "2022-03-01\t0.5\t2\t0.125\t8\n"
                    "2024-09-02\t0.25\t4\t0.25\t4\n");
  expect_prints({"action", "factors", "--instrument", "1", "--as-of", "2024-09-01"},
                std::string(factor_columns) + "2022-03-01\t0.5\t2\t0.5\t2\n");
  expect_prints({"action", "factors", "--instrument", "1", "--as-of", "2022-02-28"},
                factor_columns);
  expect_prints({"action", "factors", "--instrument", "2"}, factor_columns);
}

TEST_F(CliWithSplitsAndADividend, ActionFactorsAdjustForCashDividendsWithTheCloseGiven) {
  expect_prints({"action", "factors", "--instrument", "1", "--currency", "TST", "--close",
                 "2023-06-01:100"},
                std::string(factor_columns) +
                    "2022-03-01\t0.5\t2\t0.12375\t8\n"
                    "2023-06-01\t0.99\t1\t0.2475\t4\n"
                    "2024-09-02\t0.25\t4\t0.25\t4\n");

  expect_fails({"action", "factors", "--instrument", "1", "--currency", "TST"});
  expect_fails({"action", "factors", "--instrument", "1", "--currency", "ALT", "--close",
                "2023-06-01:100"});
  expect_fails({"action", "factors", "--instrument", "1", "--currency", "TST", "--close",
                "2023-06-01:1"});
  expect_fails({"action", "factors", "--instrument", "1", "--currency", "TST", "--close",
                "2023-06-01"});
  expect_fails({"action", "factors", "--instrument", "1", "--close", "2023-06-01:100"});
}

TEST_F(Cli, LogPrintsEveryChangeWithTheRowBeforeAndAfter) {
  add_acme_on_the_test_exchange();
  nqdb({"venue", "amend", "--venue", "XTST", "--name", "Testing Exchange"});
  const auto lines_of = [](const std::string& printed) {
    std::vector<std::string> lines;
    std::istringstream text(printed);
    for (std::string line; std::getline(text, line);) lines.push_back(line);
    return lines;
  };

  const std::vector<std::string> venue_changes =
      lines_of(nqdb({"log", "--table", "venue"}).printed);
  ASSERT_EQ(venue_changes.size(), 3u);
  EXPECT_EQ(venue_changes[0], "change_id\tchanged_at\ttable\toperation\told_row\tnew_row");
  EXPECT_NE(venue_changes[1].find("\tvenue\tINSERT\t\t{"), std::string::npos);
  EXPECT_NE(venue_changes[1].find(R"("name":"Test Exchange")"), std::string::npos);
  EXPECT_NE(venue_changes[2].find("\tvenue\tUPDATE\t{"), std::string::npos);
  EXPECT_NE(venue_changes[2].find(R"("name":"Test Exchange")"), std::string::npos);
  EXPECT_NE(venue_changes[2].find(R"("name":"Testing Exchange")"), std::string::npos);

  EXPECT_EQ(lines_of(nqdb({"log"}).printed).size(), 10u);
  EXPECT_EQ(lines_of(nqdb({"log", "--table", "issuer"}).printed).size(), 1u);
}

constexpr const char* session_columns =
    "trading_day\tweekday\topens\tcloses\topens_utc\tcloses_utc\n";

class CliWithAWeekOfSessions : public Cli {
 protected:
  CliWithAWeekOfSessions() {
    add_acme_on_the_test_exchange();
    nqdb({"calendar", "set-week", "--venue", "XTST", "--from", "2020-01-01", "--hours",
          "MON-FRI:09:30-16:00"});
  }
};

TEST_F(CliWithAWeekOfSessions, CalendarListPrintsTheSessionsOfEachTradingDay) {
  expect_prints({"calendar", "list", "--venue", "XTST", "--from", "2024-01-05", "--until",
                 "2024-01-10"},
                std::string(session_columns) +
                    "2024-01-05\tFRI\t09:30\t16:00\t"
                    "2024-01-05T09:30:00Z\t2024-01-05T16:00:00Z\n"
                    "2024-01-08\tMON\t09:30\t16:00\t"
                    "2024-01-08T09:30:00Z\t2024-01-08T16:00:00Z\n"
                    "2024-01-09\tTUE\t09:30\t16:00\t"
                    "2024-01-09T09:30:00Z\t2024-01-09T16:00:00Z\n");
  expect_prints({"calendar", "list", "--venue", "XTST", "--from", "2019-12-01", "--until",
                 "2020-01-01"},
                session_columns);
  expect_fails({"calendar", "list", "--venue", "NONE", "--from", "2024-01-05", "--until",
                "2024-01-10"});
  expect_fails({"calendar", "list", "--venue", "XTST", "--from", "2024-01-05", "--until",
                "2024-01-05"});
  expect_fails({"calendar", "list", "--venue", "XTST", "--from", "2024-01-05"});
}

TEST_F(CliWithAWeekOfSessions, CloseSetDayAndResetDayChangeSingleDays) {
  expect_prints({"calendar", "close-day", "--venue", "XTST", "--date", "2024-01-08", "--date",
                 "2024-01-09"},
                "");
  expect_prints({"calendar", "set-day", "--venue", "XTST", "--date", "2024-01-05", "--session",
                 "09:30-13:00"},
                "");
  expect_prints({"calendar", "set-day", "--venue", "XTST", "--date", "2024-01-06", "--session",
                 "13:00-15:00", "--session", "09:00-11:30"},
                "");
  expect_prints({"calendar", "list", "--venue", "XTST", "--from", "2024-01-05", "--until",
                 "2024-01-11"},
                std::string(session_columns) +
                    "2024-01-05\tFRI\t09:30\t13:00\t"
                    "2024-01-05T09:30:00Z\t2024-01-05T13:00:00Z\n"
                    "2024-01-06\tSAT\t09:00\t11:30\t"
                    "2024-01-06T09:00:00Z\t2024-01-06T11:30:00Z\n"
                    "2024-01-06\tSAT\t13:00\t15:00\t"
                    "2024-01-06T13:00:00Z\t2024-01-06T15:00:00Z\n"
                    "2024-01-10\tWED\t09:30\t16:00\t"
                    "2024-01-10T09:30:00Z\t2024-01-10T16:00:00Z\n");
  expect_prints({"calendar", "rules", "--venue", "XTST"},
                "kind\tday\tuntil\tweekday\topens\tcloses\n"
                "WEEK\t2020-01-01\t\tMON\t09:30\t16:00\n"
                "WEEK\t2020-01-01\t\tTUE\t09:30\t16:00\n"
                "WEEK\t2020-01-01\t\tWED\t09:30\t16:00\n"
                "WEEK\t2020-01-01\t\tTHU\t09:30\t16:00\n"
                "WEEK\t2020-01-01\t\tFRI\t09:30\t16:00\n"
                "CLOSED\t2024-01-08\t\tMON\t\t\n"
                "CLOSED\t2024-01-09\t\tTUE\t\t\n"
                "SPECIAL\t2024-01-05\t\tFRI\t09:30\t13:00\n"
                "SPECIAL\t2024-01-06\t\tSAT\t09:00\t11:30\n"
                "SPECIAL\t2024-01-06\t\tSAT\t13:00\t15:00\n");

  expect_prints({"calendar", "reset-day", "--venue", "XTST", "--date", "2024-01-05", "--date",
                 "2024-01-06", "--date", "2024-01-08"},
                "");
  expect_prints({"calendar", "list", "--venue", "XTST", "--from", "2024-01-05", "--until",
                 "2024-01-10"},
                std::string(session_columns) +
                    "2024-01-05\tFRI\t09:30\t16:00\t"
                    "2024-01-05T09:30:00Z\t2024-01-05T16:00:00Z\n"
                    "2024-01-08\tMON\t09:30\t16:00\t"
                    "2024-01-08T09:30:00Z\t2024-01-08T16:00:00Z\n");
}

TEST_F(CliWithAWeekOfSessions, SetWeekReplacesTheWeekFromTheDayGiven) {
  expect_prints({"calendar", "set-week", "--venue", "XTST", "--from", "2024-01-08", "--hours",
                 "MON-THU:09:00-11:30", "--hours", "MON-THU:13:00-15:00", "--hours",
                 "SUN:-06:00-17:00", "--hours", "FRI:20:00-26:00"},
                "");
  expect_prints({"calendar", "list", "--venue", "XTST", "--from", "2024-01-05", "--until",
                 "2024-01-09"},
                std::string(session_columns) +
                    "2024-01-05\tFRI\t09:30\t16:00\t"
                    "2024-01-05T09:30:00Z\t2024-01-05T16:00:00Z\n"
                    "2024-01-08\tMON\t09:00\t11:30\t"
                    "2024-01-08T09:00:00Z\t2024-01-08T11:30:00Z\n"
                    "2024-01-08\tMON\t13:00\t15:00\t"
                    "2024-01-08T13:00:00Z\t2024-01-08T15:00:00Z\n");
  expect_prints({"calendar", "list", "--venue", "XTST", "--from", "2024-01-12", "--until",
                 "2024-01-15"},
                std::string(session_columns) +
                    "2024-01-12\tFRI\t20:00\t26:00\t"
                    "2024-01-12T20:00:00Z\t2024-01-13T02:00:00Z\n"
                    "2024-01-14\tSUN\t-06:00\t17:00\t"
                    "2024-01-13T18:00:00Z\t2024-01-14T17:00:00Z\n");
  EXPECT_EQ(stored("weekly_session WHERE valid_from = '2020-01-01' "
                   "AND valid_until = '2024-01-08'"),
            5);

  expect_prints({"calendar", "set-week", "--venue", "XTST", "--from", "2024-01-08"}, "");
  EXPECT_EQ(stored("weekly_session WHERE valid_from = '2024-01-08'"), 0);
}

TEST_F(CliWithAWeekOfSessions, CalendarListAndAtUseTheTimeZoneOfTheVenue) {
  expect_prints({"venue", "amend", "--venue", "XTST", "--timezone", "America/New_York"}, "");
  expect_prints({"calendar", "list", "--venue", "XTST", "--from", "2025-03-07", "--until",
                 "2025-03-11"},
                std::string(session_columns) +
                    "2025-03-07\tFRI\t09:30\t16:00\t"
                    "2025-03-07T14:30:00Z\t2025-03-07T21:00:00Z\n"
                    "2025-03-10\tMON\t09:30\t16:00\t"
                    "2025-03-10T13:30:00Z\t2025-03-10T20:00:00Z\n");

  expect_prints({"calendar", "at", "--venue", "XTST", "--time", "2025-03-10T13:30:00Z"},
                "2025-03-10\n");
  expect_prints({"calendar", "at", "--venue", "XTST", "--time", "2025-03-10T19:59:59Z"},
                "2025-03-10\n");
  expect_fails({"calendar", "at", "--venue", "XTST", "--time", "2025-03-10T13:29:59Z"});
  expect_fails({"calendar", "at", "--venue", "XTST", "--time", "2025-03-10T20:00:00Z"});
  expect_fails({"calendar", "at", "--venue", "XTST", "--time", "2025-03-09T15:00:00Z"});

  nqdb({"calendar", "set-week", "--venue", "XTST", "--from", "2025-06-01", "--hours",
        "MON-FRI:-06:00-17:00"});
  expect_prints({"calendar", "at", "--venue", "XTST", "--time", "2025-06-08T23:00:00Z"},
                "2025-06-09\n");
  expect_fails({"calendar", "at", "--venue", "XTST", "--time", "2025-06-09T21:30:00Z"});
}

TEST_F(CliWithAWeekOfSessions, CalendarAtRejectsATimeThatIsNotAUtcTimestamp) {
  for (const char* malformed : {"2025-03-10", "2025-03-10T13:30:00", "2025-03-10 13:30:00Z",
                                "2025-03-10T13:30Z", "2025-03-10T24:00:00Z",
                                "2025-03-10T13:30:60Z", "2025-13-10T13:30:00Z",
                                "2025-03-10T13:30:0xZ", "2025-03-10T-3:30:00Z"}) {
    expect_fails({"calendar", "at", "--venue", "XTST", "--time", malformed});
  }
  expect_fails({"calendar", "at", "--venue", "NONE", "--time", "2025-03-10T13:30:00Z"});
}

TEST_F(Cli, AVenueTakesOnlyATimeZoneOfTheTimeZoneDatabase) {
  nqdb({"init"});
  expect_fails({"venue", "add", "--code", "XTST", "--name", "Test Exchange", "--timezone",
                "New York"});
  EXPECT_EQ(stored("venue"), 0);
  expect_prints({"venue", "add", "--code", "XTST", "--name", "Test Exchange", "--timezone",
                 "Asia/Tokyo"},
                "1\n");
  expect_fails({"venue", "amend", "--venue", "XTST", "--timezone", "Mars/Olympus"});
  EXPECT_EQ(stored("venue WHERE timezone = 'Asia/Tokyo'"), 1);
}

TEST_F(CliWithAWeekOfSessions, CalendarCommandsRejectMalformedHoursAndDays) {
  const auto set_week_with = [&](const char* hours) {
    expect_fails({"calendar", "set-week", "--venue", "XTST", "--from", "2024-01-08", "--hours",
                  hours});
  };
  set_week_with("MON-FRI");
  set_week_with("MON-FRI:09:30");
  set_week_with("FRI-MON:09:30-16:00");
  set_week_with("MONDAY:09:30-16:00");
  set_week_with("MON:9:30-16:00");
  set_week_with("MON:16:00-09:30");
  expect_fails({"calendar", "set-week", "--venue", "XTST", "--from", "2024-01-08", "--hours",
                "MON:09:00-12:00", "--hours", "MON:11:00-13:00"});
  expect_fails({"calendar", "set-week", "--venue", "NONE", "--from", "2024-01-08", "--hours",
                "MON:09:30-16:00"});
  expect_fails({"calendar", "close-day", "--venue", "XTST", "--date", "2024-01-08", "--date",
                "Tuesday"});
  expect_fails({"calendar", "set-day", "--venue", "XTST", "--date", "2024-01-08"});
  expect_fails({"calendar", "set-day", "--venue", "XTST", "--date", "2024-01-08", "--session",
                "09:30"});

  EXPECT_EQ(stored("weekly_session WHERE valid_from = '2020-01-01' AND valid_until IS NULL"), 5);
  EXPECT_EQ(stored("closed_day"), 0);
  EXPECT_EQ(stored("special_session"), 0);
}

TEST_F(Cli, AssignSpecChangesTheSpecFromTheDayGiven) {
  add_acme_on_the_test_exchange();
  expect_prints({"listing", "assign-spec", "--listing", "1", "--from", "2024-06-01", "--lot-size",
                 "0.5", "--tick-rule", "BANDED", "--min-qty", "2"},
                "");
  EXPECT_EQ(stored("listing_spec WHERE listing_id = 1 AND valid_from = '2020-01-01' "
                   "AND valid_until = '2024-06-01'"),
            1);
  EXPECT_EQ(stored("listing_spec WHERE listing_id = 1 AND valid_from = '2024-06-01' "
                   "AND lot_size_nanos = 500000000 AND min_qty_nanos = 2000000000"),
            1);
  expect_prints({"listing", "list", "--date", "2024-05-31"},
                 std::string(listing_columns) + acme_row);
  expect_prints({"listing", "list", "--date", "2024-06-01"},
                std::string(listing_columns) +
                    "1\t1\tXTST\tACME\tEQUITY\tTST\t0.5\tBANDED\tAcme\n");
}

TEST_F(Cli, DelistEndsTheListingOnTheDayGiven) {
  add_acme_on_the_test_exchange();
  expect_prints({"listing", "delist", "--listing", "1", "--delisted-on", "2025-01-01"}, "");
  EXPECT_EQ(stored("listing WHERE listing_id = 1 AND delisted_on = '2025-01-01'"), 1);
  expect_prints({"listing", "list", "--date", "2024-12-31"},
                 std::string(listing_columns) + acme_row);
  expect_prints({"listing", "list", "--date", "2025-01-01"}, listing_columns);
  expect_fails({"listing", "delist", "--listing", "1", "--delisted-on", "2025-02-01"});
  expect_fails({"listing", "delist", "--listing", "99", "--delisted-on", "2025-01-01"});
  expect_fails({"listing", "assign-symbol", "--listing", "1", "--symbol", "ACMX", "--from",
                "2025-01-02"});
}

TEST_F(Cli, AddsAFutureAnOptionAndAPairWithTheirSpecs) {
  add_acme_on_the_test_exchange();
  expect_prints({"instrument", "add-future", "--name", "Acme Future", "--underlying", "1",
                 "--root", "AC", "--expiry", "2026-12-18", "--multiplier", "50", "--settlement",
                 "PHYSICAL"},
                "2\n");
  EXPECT_EQ(stored("instrument WHERE instrument_id = 2 AND asset_class = 'FUTURE' "
                   "AND name = 'Acme Future'"),
            1);
  EXPECT_EQ(stored("future_spec WHERE instrument_id = 2 AND underlying_id = 1 AND root = 'AC' "
                   "AND expiry = '2026-12-18' AND multiplier_nanos = 50000000000 "
                   "AND settlement = 'PHYSICAL'"),
            1);

  expect_prints({"instrument", "add-option", "--name", "Acme Put", "--underlying", "1",
                 "--expiry", "2026-11-20", "--strike", "12.5", "--right", "PUT", "--style",
                 "EUROPEAN", "--multiplier", "100"},
                "3\n");
  EXPECT_EQ(stored("option_spec WHERE instrument_id = 3 AND underlying_id = 1 "
                   "AND expiry = '2026-11-20' AND strike_nanos = 12500000000 "
                   "AND option_right = 'PUT' AND exercise_style = 'EUROPEAN' "
                   "AND multiplier_nanos = 100000000000"),
            1);
  EXPECT_EQ(stored("instrument WHERE instrument_id = 3 AND asset_class = 'OPTION'"), 1);

  expect_prints({"instrument", "add-pair", "--asset-class", "CRYPTO", "--name", "FOO/BAR",
                 "--base", "FOO", "--quote", "BAR"},
                "4\n");
  EXPECT_EQ(stored("pair_spec WHERE instrument_id = 4 AND base_currency = 'FOO' "
                   "AND quote_currency = 'BAR'"),
            1);
  EXPECT_EQ(stored("instrument WHERE instrument_id = 4 AND asset_class = 'CRYPTO'"), 1);

  expect_prints({"instrument", "list"},
                "instrument_id\tasset_class\tname\n"
                "1\tEQUITY\tAcme\n"
                "2\tFUTURE\tAcme Future\n"
                "3\tOPTION\tAcme Put\n"
                "4\tCRYPTO\tFOO/BAR\n");
}

TEST_F(Cli, AnInstrumentCommandTakesOnlyTheOptionsOfItsKind) {
  add_acme_on_the_test_exchange();
  expect_fails({"instrument", "add", "--asset-class", "FUTURE", "--name", "No Spec"});
  expect_fails({"instrument", "add", "--asset-class", "EQUITY", "--name", "Stray", "--strike",
                "5"});
  expect_fails({"instrument", "add", "--asset-class", "SHARE", "--name", "Bad Class"});
  expect_fails({"instrument", "add-future", "--name", "No Root", "--underlying", "1", "--expiry",
                "2026-12-18", "--multiplier", "50", "--settlement", "CASH"});
  expect_fails({"instrument", "add-option", "--name", "Bad Right", "--underlying", "1",
                "--expiry", "2026-12-18", "--strike", "50", "--right", "MAYBE", "--style",
                "AMERICAN", "--multiplier", "100"});
  expect_fails({"instrument", "add-pair", "--asset-class", "EQUITY", "--name", "Not A Pair",
                "--base", "FOO", "--quote", "BAR"});
  EXPECT_EQ(stored("instrument"), 1);
}

TEST_F(Cli, AddsIssuersAndPrintsThem) {
  add_acme_on_the_test_exchange();
  expect_prints({"issuer", "add", "--name", "Acme Holdings", "--lei", "ZZZZ0000000000000001",
                 "--country", "ZZ"},
                "1\n");
  expect_prints({"issuer", "add", "--name", "Bolt Holdings"}, "2\n");
  expect_prints({"issuer", "list"}, "issuer_id\tname\tlei\tcountry\n"
                                    "1\tAcme Holdings\tZZZZ0000000000000001\tZZ\n"
                                    "2\tBolt Holdings\t\t\n");
  expect_prints({"instrument", "add", "--asset-class", "EQUITY", "--name", "Acme Parts",
                 "--issuer", "1", "--cfi", "ESVUFR"},
                "2\n");
  EXPECT_EQ(stored("instrument WHERE instrument_id = 2 AND issuer_id = 1 AND cfi = 'ESVUFR'"), 1);
}

TEST_F(Cli, AssignIdentifierEndsThePreviousOneOnTheDayGiven) {
  add_acme_on_the_test_exchange();
  expect_prints({"instrument", "add", "--asset-class", "EQUITY", "--name", "Bolt"}, "2\n");
  expect_prints({"instrument", "assign-identifier", "--instrument", "1", "--scheme", "ISIN",
                 "--value", "ZZ0000000001", "--from", "2020-01-01"},
                "");
  expect_prints({"instrument", "assign-identifier", "--instrument", "1", "--scheme", "ISIN",
                 "--value", "ZZ0000000002", "--from", "2024-06-01"},
                "");
  EXPECT_EQ(stored("instrument_identifier WHERE instrument_id = 1 AND scheme = 'ISIN' "
                   "AND value = 'ZZ0000000001' AND valid_from = '2020-01-01' "
                   "AND valid_until = '2024-06-01'"),
            1);
  EXPECT_EQ(stored("instrument_identifier WHERE instrument_id = 1 AND value = 'ZZ0000000002' "
                   "AND valid_from = '2024-06-01' AND valid_until IS NULL"),
            1);
  expect_fails({"instrument", "assign-identifier", "--instrument", "2", "--scheme", "ISIN",
                "--value", "ZZ0000000002", "--from", "2024-07-01"});
}

TEST_F(Cli, ActionRecordStoresEveryOption) {
  add_acme_on_the_test_exchange();
  expect_prints({"instrument", "add", "--asset-class", "EQUITY", "--name", "Acme Parts"}, "2\n");
  expect_prints({"action", "add", "--instrument", "1", "--type", "SPLIT", "--ex-date",
                 "2024-06-03", "--new-shares", "3", "--old-shares", "2", "--announce-date",
                 "2024-05-01", "--record-date", "2024-06-04"},
                "1\n");
  EXPECT_EQ(stored("corporate_action WHERE corporate_action_id = 1 AND instrument_id = 1 "
                   "AND action_type = 'SPLIT' AND ex_date = '2024-06-03' AND new_shares = 3 "
                   "AND old_shares = 2 AND announce_date = '2024-05-01' "
                   "AND record_date = '2024-06-04' AND pay_date IS NULL "
                   "AND cash_per_share_nanos IS NULL AND related_instrument_id IS NULL"),
            1);
  expect_prints({"action", "add", "--instrument", "2", "--type", "SPINOFF", "--ex-date",
                 "2024-07-01", "--related-instrument", "1", "--cash-per-share", "0.25",
                 "--currency", "TST", "--pay-date", "2024-07-10"},
                "2\n");
  EXPECT_EQ(stored("corporate_action WHERE corporate_action_id = 2 AND instrument_id = 2 "
                   "AND action_type = 'SPINOFF' AND related_instrument_id = 1 "
                   "AND cash_per_share_nanos = 250000000 AND currency = 'TST' "
                   "AND pay_date = '2024-07-10' AND new_shares IS NULL"),
            1);
  expect_fails({"action", "add", "--instrument", "1", "--type", "GIFT", "--ex-date",
                "2024-06-03"});
  expect_fails({"action", "add", "--instrument", "1", "--type", "SPLIT", "--ex-date",
                "2024-06-03"});
  expect_fails({"action", "add", "--instrument", "1", "--type", "CASH_DIVIDEND", "--ex-date",
                "2024-06-03", "--cash-per-share", "0.25"});
}

TEST_F(Cli, RejectsMalformedDatesAndNumbers) {
  add_acme_on_the_test_exchange();
  expect_fails({"listing", "list", "--date", "2020-1-1"});
  expect_fails({"listing", "list", "--date", "2020-02-30"});
  expect_fails({"listing", "list", "--date", "yesterday"});
  expect_fails({"listing", "assign-symbol", "--listing", "1", "--symbol", "ACMX", "--from",
                "June"});
  expect_fails({"listing", "assign-spec", "--listing", "1", "--from", "2024-06-01", "--lot-size",
                "1e2", "--tick-rule", "BANDED"});
  expect_fails({"tick-rule", "add", "--name", "BAD", "--band", "0.01"});
}

TEST_F(Cli, RejectsNamesThatAreNotInTheDatabase) {
  add_acme_on_the_test_exchange();
  expect_fails({"listing", "assign-spec", "--listing", "1", "--from", "2024-06-01", "--lot-size",
                "1", "--tick-rule", "NONE"});
  expect_fails({"listing", "add", "--instrument", "1", "--venue", "NONE", "--currency", "TST",
                "--symbol", "ACMY", "--listed-on", "2020-01-01", "--lot-size", "1", "--tick-rule",
                "BANDED"});
  expect_fails({"listing", "add", "--instrument", "99", "--venue", "XTST", "--currency", "TST",
                "--symbol", "ACMY", "--listed-on", "2020-01-01", "--lot-size", "1", "--tick-rule",
                "BANDED"});
  EXPECT_EQ(stored("listing"), 1);
}

TEST_F(Cli, ArchiveWritesADatedCopyBesideTheDatabase) {
  add_acme_on_the_test_exchange();
  const std::filesystem::path copy = directory_.path() / "archive" / "secmaster-2026-11-19.db";
  expect_prints({"archive", "--date", "2026-11-19"}, copy.string() + "\n");
  EXPECT_TRUE(std::filesystem::exists(copy));
  expect_fails({"archive", "--date", "2026-11-19"});

  const std::string archived = copy.string();
  const outcome listed = run_nqdb({"--db", archived.c_str(), "listing", "list", "--date",
                                   "2020-01-01"});
  EXPECT_EQ(listed.printed, std::string(listing_columns) + acme_row);
}

TEST_F(Cli, ArchiveWritesIntoTheDirectoryGiven) {
  add_acme_on_the_test_exchange();
  const std::string elsewhere = (directory_.path() / "elsewhere").string();
  const std::filesystem::path copy = directory_.path() / "elsewhere" / "secmaster-2026-11-19.db";
  expect_prints({"archive", "--date", "2026-11-19", "--dir", elsewhere.c_str()},
                copy.string() + "\n");
  EXPECT_TRUE(std::filesystem::exists(copy));
  EXPECT_FALSE(std::filesystem::exists(directory_.path() / "archive"));
}

TEST_F(Cli, HelpAndUnknownCommandsDoNotCreateTheDatabase) {
  EXPECT_EQ(nqdb({"--help"}).exit_code, 0);
  EXPECT_NE(nqdb({"frobnicate"}).exit_code, 0);
  EXPECT_NE(nqdb({}).exit_code, 0);
  EXPECT_FALSE(std::filesystem::exists(database_file()));
}

}  // namespace
