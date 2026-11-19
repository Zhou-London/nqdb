#include <nq/db/secmaster/loader.h>
#include <nq/db/secmaster/schema.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fictional_market.h"
#include "secmaster_test_database.h"
#include "temporary_directory.h"

namespace {

using namespace std::chrono_literals;
using namespace nq::db::secmaster;
using namespace nqdb_test;
using nq::db::sqlite::database;
using std::chrono::sys_days;

constexpr sys_days rename_day{2024y / 6 / 1};

constexpr auto everything = [](const venue&, const instrument&) { return true; };

day_range one_day(sys_days day) { return {.from = day, .until = day + std::chrono::days{1}}; }

const instrument& instrument_of(const history& loaded, instrument_id id) {
  return loaded.instruments()[*loaded.instruments().find(id)];
}

std::vector<std::string> symbols(const history& loaded) {
  std::vector<std::string> symbols;
  for (const listing& each : loaded.listings()) {
    const listing_index index = *loaded.listings().find(each.id);
    symbols.push_back(loaded.symbol_on(venue_symbol_source, index, rename_day)->symbol);
  }
  return symbols;
}

TEST(SecmasterLoader, VenuesKeepTheirOptionalFields) {
  database secmaster = fictional_market();
  const history loaded = load_history(secmaster, one_day(rename_day), everything);

  const venue& test_exchange = loaded.venues()[*loaded.find_venue("XTST")];
  EXPECT_EQ(test_exchange.id, venue_id{1});
  EXPECT_EQ(test_exchange.mic, "XTST");
  EXPECT_EQ(test_exchange.country, "ZZ");
  EXPECT_EQ(test_exchange.timezone, "Etc/UTC");

  const venue& alternative = loaded.venues()[*loaded.find_venue("XALT")];
  EXPECT_EQ(alternative.mic, std::nullopt);
  EXPECT_EQ(alternative.country, std::nullopt);
}

TEST(SecmasterLoader, InstrumentsCarryTheSpecOfTheirAssetClass) {
  database secmaster = fictional_market();
  const history loaded = load_history(secmaster, one_day(rename_day), everything);

  EXPECT_EQ(instrument_of(loaded, acme).asset_class, asset_class::equity);
  EXPECT_TRUE(std::holds_alternative<no_spec>(instrument_of(loaded, acme).spec));

  const future_spec& future = std::get<future_spec>(instrument_of(loaded, index_future).spec);
  EXPECT_EQ(future.underlying, test_index);
  EXPECT_EQ(future.root, "T1");
  EXPECT_EQ(future.expiry, sys_days{2026y / 12 / 18});
  EXPECT_EQ(future.multiplier, multiplier_nanos{50 * nanos_per_unit});
  EXPECT_EQ(future.settlement, settlement_method::cash);
  EXPECT_EQ(loaded.instruments().find(future.underlying), std::nullopt);

  const option_spec& option = std::get<option_spec>(instrument_of(loaded, acme_call).spec);
  EXPECT_EQ(option.underlying, acme);
  EXPECT_EQ(option.expiry, sys_days{2026y / 12 / 18});
  EXPECT_EQ(option.strike, price_nanos{50 * nanos_per_unit});
  EXPECT_EQ(option.right, option_right::call);
  EXPECT_EQ(option.style, exercise_style::american);
  EXPECT_EQ(option.multiplier, multiplier_nanos{100 * nanos_per_unit});

  const pair_spec& pair = std::get<pair_spec>(instrument_of(loaded, foo_bar_pair).spec);
  EXPECT_EQ(pair.base_currency, "FOO");
  EXPECT_EQ(pair.quote_currency, "BAR");
}

TEST(SecmasterLoader, ReadsEveryNameTheSchemaAllows) {
  database secmaster = nqdb_test::secmaster_with_venues();
  secmaster.run_script(R"sql(
INSERT INTO instrument (instrument_id, asset_class, name) VALUES
  (1, 'EQUITY', 'Acme'), (2, 'ETF', 'Bolt Fund'), (3, 'FUTURE', 'Cash Future'),
  (4, 'OPTION', 'Acme Put'), (5, 'FX', 'FOO/BAR'), (6, 'CRYPTO', 'BAZ/BAR'),
  (7, 'BOND', 'Acme 2030'), (8, 'INDEX', 'Test 100'), (9, 'FUTURE', 'Physical Future'),
  (10, 'OPTION', 'Acme Call');
INSERT INTO future_spec VALUES
  (3, 8, 'T1', '2026-12-18', 1000000000, 'CASH'),
  (9, 8, 'T2', '2026-12-18', 1000000000, 'PHYSICAL');
INSERT INTO option_spec VALUES
  (4, 1, '2026-12-18', 1000000000, 'PUT', 'EUROPEAN', 1000000000),
  (10, 1, '2026-12-18', 1000000000, 'CALL', 'AMERICAN', 1000000000);
INSERT INTO pair_spec VALUES (5, 'FOO', 'BAR'), (6, 'BAZ', 'BAR');
INSERT INTO listing (listing_id, instrument_id, venue_id, currency, listed_on)
  SELECT instrument_id, instrument_id, 1, 'TST', '2020-01-01' FROM instrument;
INSERT INTO listing_symbol (listing_id, source, symbol, valid_from)
  SELECT listing_id, 'VENUE', 'SYMBOL' || listing_id, '2020-01-01' FROM listing;
INSERT INTO listing_spec (listing_id, valid_from, lot_size_nanos, tick_rule_id)
  SELECT listing_id, '2020-01-01', 1000000000, 1 FROM listing;
)sql");
  const history loaded = load_history(secmaster, one_day(rename_day), everything);
  const auto asset_class_of = [&](std::int64_t id) {
    return instrument_of(loaded, instrument_id{id}).asset_class;
  };
  const auto spec_of = [&](std::int64_t id) {
    return instrument_of(loaded, instrument_id{id}).spec;
  };

  EXPECT_EQ(asset_class_of(1), asset_class::equity);
  EXPECT_EQ(asset_class_of(2), asset_class::etf);
  EXPECT_EQ(asset_class_of(3), asset_class::future);
  EXPECT_EQ(asset_class_of(4), asset_class::option);
  EXPECT_EQ(asset_class_of(5), asset_class::fx);
  EXPECT_EQ(asset_class_of(6), asset_class::crypto);
  EXPECT_EQ(asset_class_of(7), asset_class::bond);
  EXPECT_EQ(asset_class_of(8), asset_class::index);

  EXPECT_EQ(std::get<future_spec>(spec_of(3)).settlement, settlement_method::cash);
  EXPECT_EQ(std::get<future_spec>(spec_of(9)).settlement, settlement_method::physical);
  EXPECT_EQ(std::get<option_spec>(spec_of(4)).right, option_right::put);
  EXPECT_EQ(std::get<option_spec>(spec_of(4)).style, exercise_style::european);
  EXPECT_EQ(std::get<option_spec>(spec_of(10)).right, option_right::call);
  EXPECT_EQ(std::get<option_spec>(spec_of(10)).style, exercise_style::american);
  EXPECT_EQ(std::get<pair_spec>(spec_of(5)).base_currency, "FOO");
  EXPECT_EQ(std::get<pair_spec>(spec_of(6)).base_currency, "BAZ");
  EXPECT_TRUE(std::holds_alternative<no_spec>(spec_of(2)));
  EXPECT_TRUE(std::holds_alternative<no_spec>(spec_of(7)));
  EXPECT_TRUE(std::holds_alternative<no_spec>(spec_of(8)));
}

TEST(SecmasterLoader, TickSizeFollowsThePriceBand) {
  database secmaster = fictional_market();
  const history loaded = load_history(secmaster, one_day(rename_day), everything);
  const listing_index acme_listing = *loaded.listings().find(acme_on_test_exchange);
  const tick_rule& banded =
      loaded.tick_rules()[loaded.spec_on(acme_listing, rename_day)->tick_rule];

  EXPECT_EQ(banded.tick_size_at(price_nanos{-1}), price_nanos{100'000});
  EXPECT_EQ(banded.tick_size_at(price_nanos{0}), price_nanos{100'000});
  EXPECT_EQ(banded.tick_size_at(price_nanos{nanos_per_unit - 1}), price_nanos{100'000});
  EXPECT_EQ(banded.tick_size_at(price_nanos{nanos_per_unit}), price_nanos{10'000'000});
  EXPECT_EQ(banded.tick_size_at(price_nanos{500 * nanos_per_unit}), price_nanos{10'000'000});
}

TEST(SecmasterLoader, RejectsAnIncompleteListingOnlyInsideTheUniverse) {
  database secmaster = fictional_market();
  secmaster.run_script("DELETE FROM listing_spec WHERE listing_id = 3;"
                       "DELETE FROM listing_symbol WHERE listing_id = 4;");
  const auto on_venue = [](std::string_view code, asset_class wanted) {
    return [=](const venue& venue, const instrument& instrument) {
      return venue.code == code && instrument.asset_class == wanted;
    };
  };

  const auto load = [&](auto in_universe) {
    return load_history(secmaster, one_day(rename_day), in_universe);
  };
  EXPECT_NO_THROW(load(on_venue("XTST", asset_class::equity)));
  EXPECT_THROW(load(on_venue("XALT", asset_class::equity)), error);
  EXPECT_THROW(load(on_venue("XTST", asset_class::future)), error);
}

TEST(SecmasterLoader, RejectsADerivativeWithoutItsSpec) {
  database secmaster = fictional_market();
  secmaster.run_script("DELETE FROM option_spec");
  EXPECT_THROW(load_history(secmaster, one_day(rename_day), everything), error);
}

TEST(SecmasterLoader, RejectsATickRuleWithoutBands) {
  database secmaster = fictional_market();
  secmaster.run_script("INSERT INTO tick_rule (tick_rule_id, name) VALUES (3, 'EMPTY')");
  EXPECT_THROW(load_history(secmaster, one_day(rename_day), everything), error);
}

TEST(SecmasterLoader, LoadsFromAnArchivedCopy) {
  const nqdb_test::temporary_directory directory;
  database secmaster = fictional_market();
  database archived = database::open_read_only(archive(secmaster, directory.path(), rename_day));

  EXPECT_EQ(symbols(load_history(archived, one_day(rename_day), everything)),
            symbols(load_history(secmaster, one_day(rename_day), everything)));
}

}  // namespace
