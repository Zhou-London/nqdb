#include "cli.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include <nqdb/sqlite.h>

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
      nqdb::cli::run(static_cast<int>(arguments.size()), arguments.data(), out, err);
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
    nqdb::sqlite::database database = nqdb::sqlite::database::open_read_only(database_file());
    nqdb::sqlite::statement row = database.prepare("SELECT count(*) FROM " + table_and_filter);
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
  expect_fails({"list"});
  expect_fails({"venue", "add", "--code", "XTST", "--name", "Test Exchange", "--timezone",
                "Etc/UTC"});
  EXPECT_FALSE(std::filesystem::exists(database_file()));
}

TEST_F(Cli, CommandsRejectADatabaseOfAnotherSchemaVersion) {
  add_acme_on_the_test_exchange();
  {
    nqdb::sqlite::database database = nqdb::sqlite::database::open(database_file());
    database.run_script("PRAGMA user_version = 999");
  }
  expect_fails({"list"});
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
  expect_prints({"list", "--date", "2020-01-01"}, std::string(listing_columns) + acme_row);
  expect_prints({"list", "--date", "2019-12-31"}, listing_columns);
  expect_prints({"list", "--date", "2020-01-01", "--venue", "XTST"},
                std::string(listing_columns) + acme_row);
  expect_prints({"list", "--date", "2020-01-01", "--venue", "XALT"}, listing_columns);
  expect_prints({"list", "--date", "2020-01-01", "--asset-class", "EQUITY"},
                std::string(listing_columns) + acme_row);
  expect_prints({"list", "--date", "2020-01-01", "--asset-class", "ETF"}, listing_columns);
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
  expect_prints({"find", "--venue", "XTST", "--symbol", "ACME", "--date", "2024-05-31"}, "1\n");
  expect_prints({"find", "--venue", "XTST", "--symbol", "ACMX", "--date", "2024-06-01"}, "1\n");
  expect_fails({"find", "--venue", "XTST", "--symbol", "ACME", "--date", "2024-06-01"});
  expect_fails({"find", "--venue", "NONE", "--symbol", "ACMX", "--date", "2024-06-01"});
}

TEST_F(Cli, FindReadsTheSymbolOfTheSourceGiven) {
  add_acme_on_the_test_exchange();
  expect_prints({"listing", "assign-symbol", "--listing", "1", "--source", "VENDOR", "--symbol",
                 "ACME.T", "--from", "2020-01-01"},
                "");
  EXPECT_EQ(stored("listing_symbol WHERE source = 'VENDOR' AND symbol = 'ACME.T'"), 1);
  expect_prints({"find", "--venue", "XTST", "--source", "VENDOR", "--symbol", "ACME.T", "--date",
                 "2020-01-01"},
                "1\n");
  expect_fails({"find", "--venue", "XTST", "--symbol", "ACME.T", "--date", "2020-01-01"});
  expect_fails({"find", "--venue", "XTST", "--source", "VENDOR", "--symbol", "ACME", "--date",
                "2020-01-01"});
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
  expect_prints({"list", "--date", "2024-05-31"}, std::string(listing_columns) + acme_row);
  expect_prints({"list", "--date", "2024-06-01"},
                std::string(listing_columns) +
                    "1\t1\tXTST\tACME\tEQUITY\tTST\t0.5\tBANDED\tAcme\n");
}

TEST_F(Cli, DelistEndsTheListingOnTheDayGiven) {
  add_acme_on_the_test_exchange();
  expect_prints({"listing", "delist", "--listing", "1", "--delisted-on", "2025-01-01"}, "");
  EXPECT_EQ(stored("listing WHERE listing_id = 1 AND delisted_on = '2025-01-01'"), 1);
  expect_prints({"list", "--date", "2024-12-31"}, std::string(listing_columns) + acme_row);
  expect_prints({"list", "--date", "2025-01-01"}, listing_columns);
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
  expect_prints({"action", "record", "--instrument", "1", "--type", "SPLIT", "--ex-date",
                 "2024-06-03", "--new-shares", "3", "--old-shares", "2", "--announce-date",
                 "2024-05-01", "--record-date", "2024-06-04"},
                "1\n");
  EXPECT_EQ(stored("corporate_action WHERE corporate_action_id = 1 AND instrument_id = 1 "
                   "AND action_type = 'SPLIT' AND ex_date = '2024-06-03' AND new_shares = 3 "
                   "AND old_shares = 2 AND announce_date = '2024-05-01' "
                   "AND record_date = '2024-06-04' AND pay_date IS NULL "
                   "AND cash_per_share_nanos IS NULL AND related_instrument_id IS NULL"),
            1);
  expect_prints({"action", "record", "--instrument", "2", "--type", "SPINOFF", "--ex-date",
                 "2024-07-01", "--related-instrument", "1", "--cash-per-share", "0.25",
                 "--currency", "TST", "--pay-date", "2024-07-10"},
                "2\n");
  EXPECT_EQ(stored("corporate_action WHERE corporate_action_id = 2 AND instrument_id = 2 "
                   "AND action_type = 'SPINOFF' AND related_instrument_id = 1 "
                   "AND cash_per_share_nanos = 250000000 AND currency = 'TST' "
                   "AND pay_date = '2024-07-10' AND new_shares IS NULL"),
            1);
  expect_fails({"action", "record", "--instrument", "1", "--type", "GIFT", "--ex-date",
                "2024-06-03"});
  expect_fails({"action", "record", "--instrument", "1", "--type", "SPLIT", "--ex-date",
                "2024-06-03"});
  expect_fails({"action", "record", "--instrument", "1", "--type", "CASH_DIVIDEND", "--ex-date",
                "2024-06-03", "--cash-per-share", "0.25"});
}

TEST_F(Cli, RejectsMalformedDatesAndNumbers) {
  add_acme_on_the_test_exchange();
  expect_fails({"list", "--date", "2020-1-1"});
  expect_fails({"list", "--date", "2020-02-30"});
  expect_fails({"list", "--date", "yesterday"});
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
  const outcome listed = run_nqdb({"--db", archived.c_str(), "list", "--date", "2020-01-01"});
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
