#include <nq/db/secmaster/writer.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <nq/db/secmaster/loader.h>
#include <nq/db/secmaster/schema.h>

#include "day_view.h"

namespace {

using namespace std::chrono_literals;
using namespace nq::db::secmaster;
using nqdb_test::day_view;
using nqdb_test::listing_on_day;
using nq::db::sqlite::database;
using std::chrono::sys_days;

constexpr sys_days listing_day{2020y / 1 / 1};
constexpr sys_days rename_day{2024y / 6 / 1};
constexpr sys_days delisting_day{2025y / 1 / 1};
constexpr day_range every_day{.from = sys_days{2000y / 1 / 1}, .until = sys_days{2100y / 1 / 1}};
constexpr validity always_listed{.from = listing_day, .until = std::nullopt};

constexpr auto everything = [](const venue&, const instrument&) { return true; };

struct market {
  database secmaster;
  venue_id test_exchange;
  tick_rule_id cent;
  tick_rule_id milli;
  instrument_id acme;
  listing_id acme_listing;
  listing_id bolt_listing;
};

instrument_id add_equity(database& secmaster, const std::string& name) {
  return add_instrument(secmaster, {.asset_class = asset_class::equity, .name = name,
                                    .issuer = {}, .cfi = {}, .spec = no_spec{}});
}

listing_id list_on_test_exchange(market& market, instrument_id instrument,
                                 const std::string& symbol, sys_days listed_on) {
  return add_listing(market.secmaster,
                     {.instrument = instrument,
                      .venue = market.test_exchange,
                      .currency = "TST",
                      .is_primary = true,
                      .listed_on = listed_on,
                      .symbol = symbol,
                      .spec = {.lot_size = quantity_nanos{nanos_per_unit}, .min_qty = {},
                               .tick_rule = market.cent}});
}

market market_with_acme_and_bolt_listed() {
  market market{.secmaster = database::in_memory(), .test_exchange = {}, .cent = {}, .milli = {},
                .acme = {}, .acme_listing = {}, .bolt_listing = {}};
  create_schema(market.secmaster);
  market.test_exchange =
      add_venue(market.secmaster, {.code = "XTST", .mic = "XTST", .name = "Test Exchange",
                                   .country = "ZZ", .timezone = "Etc/UTC"});
  market.cent = add_tick_rule(market.secmaster, "CENT",
                              std::array{tick_band{price_nanos{0}, price_nanos{10'000'000}}});
  market.milli = add_tick_rule(market.secmaster, "MILLI",
                               std::array{tick_band{price_nanos{0}, price_nanos{1'000'000}}});
  market.acme = add_equity(market.secmaster, "Acme");
  market.acme_listing = list_on_test_exchange(market, market.acme, "ACME", listing_day);
  market.bolt_listing =
      list_on_test_exchange(market, add_equity(market.secmaster, "Bolt"), "BOLT", listing_day);
  return market;
}

std::int64_t count(database& secmaster, const std::string& table_and_filter) {
  nq::db::sqlite::statement row = secmaster.prepare("SELECT count(*) FROM " + table_and_filter);
  row.next_row();
  return row.get<std::int64_t>(0);
}

bool acme_is_listed_on(market& market, sys_days day) {
  return day_view(market.secmaster, day, everything)
      .listings()
      .find(market.acme_listing)
      .has_value();
}

std::vector<symbol_period> symbols_of(market& market, listing_id listing,
                                      std::string_view symbol_source = "VENUE") {
  const history loaded = load_history(market.secmaster, every_day, everything);
  const std::span<const symbol_period> periods =
      loaded.symbols_of(symbol_source, *loaded.listings().find(listing));
  return {periods.begin(), periods.end()};
}

std::vector<validity> spec_validities_of(market& market, listing_id listing) {
  const history loaded = load_history(market.secmaster, every_day, everything);
  std::vector<validity> validities;
  for (const spec_period& period : loaded.specs_of(*loaded.listings().find(listing))) {
    validities.push_back(period.valid);
  }
  return validities;
}

TEST(SecmasterAmend, AmendVenueChangesOnlyTheFieldsGiven) {
  market market = market_with_acme_and_bolt_listed();
  amend_venue(market.secmaster, market.test_exchange,
              {.code = {}, .mic = {}, .name = "Testing Exchange", .country = {}, .timezone = {}});

  const day_view loaded = day_view(market.secmaster, listing_day, everything);
  const venue& amended = loaded.venues()[*loaded.find_venue("XTST")];
  EXPECT_EQ(amended.name, "Testing Exchange");
  EXPECT_EQ(amended.mic, "XTST");
  EXPECT_EQ(amended.country, "ZZ");
  EXPECT_EQ(amended.timezone, "Etc/UTC");

  amend_venue(market.secmaster, market.test_exchange,
              {.code = "XNEW", .mic = "XNEW", .name = {}, .country = "YY", .timezone = "Etc/GMT"});
  EXPECT_EQ(count(market.secmaster, "venue WHERE code = 'XNEW' AND mic = 'XNEW' AND country = 'YY'"
                                    " AND timezone = 'Etc/GMT' AND name = 'Testing Exchange'"),
            1);
  EXPECT_THROW(amend_venue(market.secmaster, venue_id{99}, {.code = {}, .mic = {}, .name = "None",
                                                            .country = {}, .timezone = {}}),
               error);
}

TEST(SecmasterAmend, AmendIssuerChangesOnlyTheFieldsGiven) {
  market market = market_with_acme_and_bolt_listed();
  const issuer_id holdings =
      add_issuer(market.secmaster, {.name = "Acme Holdngs", .lei = {}, .country = "ZZ"});

  amend_issuer(market.secmaster, holdings,
               {.name = "Acme Holdings", .lei = "ABCDEFGHIJ0123456789", .country = {}});

  const issuer amended = read_issuers(market.secmaster).front();
  EXPECT_EQ(amended.name, "Acme Holdings");
  EXPECT_EQ(amended.lei, "ABCDEFGHIJ0123456789");
  EXPECT_EQ(amended.country, "ZZ");
  EXPECT_THROW(
      amend_issuer(market.secmaster, issuer_id{99}, {.name = "None", .lei = {}, .country = {}}),
      error);
}

TEST(SecmasterAmend, AmendTickRuleReplacesEveryBand) {
  market market = market_with_acme_and_bolt_listed();
  const auto cent = [&] {
    const day_view loaded = day_view(market.secmaster, listing_day, everything);
    return loaded.tick_rules()[*loaded.tick_rules().find(market.cent)];
  };

  amend_tick_rule(market.secmaster, market.cent,
                  {.name = {},
                   .bands = std::vector{tick_band{price_nanos{0}, price_nanos{100'000}},
                                        tick_band{price_nanos{nanos_per_unit},
                                                  price_nanos{10'000'000}}}});
  EXPECT_EQ(cent().name, "CENT");
  EXPECT_EQ(cent().tick_size_at(price_nanos{nanos_per_unit / 2}), price_nanos{100'000});
  EXPECT_EQ(cent().tick_size_at(price_nanos{2 * nanos_per_unit}), price_nanos{10'000'000});

  amend_tick_rule(market.secmaster, market.cent, {.name = "BANDED", .bands = {}});
  EXPECT_EQ(cent().name, "BANDED");
  EXPECT_EQ(cent().bands_by_ascending_price.size(), 2u);

  EXPECT_THROW(amend_tick_rule(market.secmaster, market.cent,
                               {.name = "EMPTY", .bands = std::vector<tick_band>{}}),
               error);
  EXPECT_THROW(amend_tick_rule(market.secmaster, tick_rule_id{99}, {.name = "NONE", .bands = {}}),
               error);
  EXPECT_EQ(cent().name, "BANDED");
  EXPECT_EQ(cent().bands_by_ascending_price.size(), 2u);
}

TEST(SecmasterAmend, AmendInstrumentChangesItsNameAndItsAssetClass) {
  market market = market_with_acme_and_bolt_listed();
  const issuer_id holdings =
      add_issuer(market.secmaster, {.name = "Acme Holdings", .lei = {}, .country = {}});

  amend_instrument(market.secmaster, market.acme,
                   {.asset_class = asset_class::etf, .name = "Acme Fund", .issuer = holdings,
                    .cfi = "CEOGMS", .spec = {}});
  EXPECT_EQ(count(market.secmaster, "instrument WHERE instrument_id = 1 AND asset_class = 'ETF' "
                                    "AND name = 'Acme Fund' AND issuer_id = 1 AND cfi = 'CEOGMS'"),
            1);

  amend_instrument(market.secmaster, market.acme,
                   {.asset_class = {}, .name = "Acme ETF", .issuer = {}, .cfi = {}, .spec = {}});
  EXPECT_EQ(count(market.secmaster, "instrument WHERE instrument_id = 1 AND asset_class = 'ETF' "
                                    "AND name = 'Acme ETF' AND issuer_id = 1 AND cfi = 'CEOGMS'"),
            1);

  EXPECT_THROW(amend_instrument(market.secmaster, market.acme,
                                {.asset_class = asset_class::future, .name = "Acme Future",
                                 .issuer = {}, .cfi = {}, .spec = {}}),
               error);
  EXPECT_THROW(amend_instrument(market.secmaster, market.acme,
                                {.asset_class = {}, .name = "Acme Pair", .issuer = {}, .cfi = {},
                                 .spec = pair_spec{"FOO", "BAR"}}),
               error);
  EXPECT_THROW(amend_instrument(market.secmaster, instrument_id{99},
                                {.asset_class = {}, .name = "None", .issuer = {}, .cfi = {},
                                 .spec = {}}),
               error);
  EXPECT_EQ(count(market.secmaster, "instrument WHERE name = 'Acme ETF'"), 1);
  EXPECT_EQ(count(market.secmaster, "pair_spec"), 0);
}

TEST(SecmasterAmend, AmendInstrumentReplacesItsSpec) {
  market market = market_with_acme_and_bolt_listed();
  const future_spec december{.underlying = market.acme,
                             .root = "AC",
                             .expiry = sys_days{2026y / 12 / 18},
                             .multiplier = multiplier_nanos{100 * nanos_per_unit},
                             .settlement = settlement_method::cash};
  const instrument_id contract =
      add_instrument(market.secmaster, {.asset_class = asset_class::future, .name = "Acme Dec",
                                        .issuer = {}, .cfi = {}, .spec = december});
  const auto stored_spec = [&] { return read_instruments(market.secmaster).back().spec; };

  future_spec november = december;
  november.expiry = sys_days{2026y / 11 / 20};
  amend_instrument(market.secmaster, contract,
                   {.asset_class = {}, .name = {}, .issuer = {}, .cfi = {}, .spec = november});
  EXPECT_EQ(std::get<future_spec>(stored_spec()).expiry, november.expiry);
  EXPECT_EQ(std::get<future_spec>(stored_spec()).root, "AC");

  const option_spec call{.underlying = market.acme,
                         .expiry = november.expiry,
                         .strike = price_nanos{50 * nanos_per_unit},
                         .right = option_right::call,
                         .style = exercise_style::european,
                         .multiplier = multiplier_nanos{100 * nanos_per_unit}};
  EXPECT_THROW(amend_instrument(market.secmaster, contract,
                                {.asset_class = {}, .name = {}, .issuer = {}, .cfi = {},
                                 .spec = call}),
               error);
  EXPECT_EQ(std::get<future_spec>(stored_spec()).expiry, november.expiry);

  amend_instrument(market.secmaster, contract,
                   {.asset_class = asset_class::option, .name = "Acme Call", .issuer = {},
                    .cfi = {}, .spec = call});
  EXPECT_EQ(std::get<option_spec>(stored_spec()).strike, call.strike);
  EXPECT_EQ(count(market.secmaster, "future_spec"), 0);
}

TEST(SecmasterAmend, AmendListingChangesItsCurrencyAndWhetherItIsPrimary) {
  market market = market_with_acme_and_bolt_listed();
  amend_listing(market.secmaster, market.acme_listing,
                {.currency = "ALT", .is_primary = false, .listed_on = {}, .delisted_on = {}});
  EXPECT_EQ(count(market.secmaster, "listing WHERE listing_id = 1 AND currency = 'ALT' "
                                    "AND is_primary = 0 AND listed_on = '2020-01-01'"),
            1);

  amend_listing(market.secmaster, market.acme_listing,
                {.currency = {}, .is_primary = true, .listed_on = {}, .delisted_on = {}});
  EXPECT_EQ(count(market.secmaster, "listing WHERE listing_id = 1 AND currency = 'ALT' "
                                    "AND is_primary = 1"),
            1);
  EXPECT_THROW(amend_listing(market.secmaster, listing_id{99},
                             {.currency = "ALT", .is_primary = {}, .listed_on = {},
                              .delisted_on = {}}),
               error);
}

TEST(SecmasterAmend, AmendListingMovesTheFirstDayWithItsSymbolAndSpec) {
  market market = market_with_acme_and_bolt_listed();
  const sys_days earlier{2019y / 6 / 1};
  const sys_days later{2020y / 2 / 1};

  amend_listing(market.secmaster, market.acme_listing,
                {.currency = {}, .is_primary = {}, .listed_on = earlier, .delisted_on = {}});
  EXPECT_TRUE(acme_is_listed_on(market, earlier));
  EXPECT_EQ(symbols_of(market, market.acme_listing).front().valid.from, earlier);
  EXPECT_EQ(spec_validities_of(market, market.acme_listing).front().from, earlier);

  amend_listing(market.secmaster, market.acme_listing,
                {.currency = {}, .is_primary = {}, .listed_on = later, .delisted_on = {}});
  EXPECT_FALSE(acme_is_listed_on(market, listing_day));
  EXPECT_TRUE(acme_is_listed_on(market, later));
  EXPECT_EQ(symbols_of(market, market.bolt_listing).front().valid.from, listing_day);
}

TEST(SecmasterAmend, AmendListingRejectsAFirstDayThatLeavesAPeriodOutsideTheLifetime) {
  market market = market_with_acme_and_bolt_listed();
  assign_symbol(market.secmaster, market.acme_listing, "VENUE", "ACMX", rename_day);
  assign_symbol(market.secmaster, market.acme_listing, "VENDOR", "ACME.T", sys_days{2021y / 1 / 1});
  assign_symbol(market.secmaster, market.acme_listing, "VENDOR", "ACMX.T", sys_days{2022y / 1 / 1});
  const std::vector<symbol_period> venue_symbols = symbols_of(market, market.acme_listing);
  const auto amend_first_day = [&](sys_days listed_on) {
    amend_listing(market.secmaster, market.acme_listing,
                  {.currency = "ALT", .is_primary = {}, .listed_on = listed_on,
                   .delisted_on = {}});
  };

  EXPECT_THROW(amend_first_day(sys_days{2024y / 7 / 1}), std::runtime_error);
  EXPECT_THROW(amend_first_day(sys_days{2021y / 6 / 1}), std::runtime_error);
  EXPECT_EQ(symbols_of(market, market.acme_listing), venue_symbols);
  EXPECT_EQ(count(market.secmaster, "listing WHERE listing_id = 1 AND currency = 'TST' "
                                    "AND listed_on = '2020-01-01'"),
            1);
  EXPECT_NO_THROW(amend_first_day(sys_days{2020y / 12 / 1}));
}

TEST(SecmasterAmend, AmendListingMovesTheDelistingDayWithTheEndOfItsSymbolAndSpec) {
  market market = market_with_acme_and_bolt_listed();
  const sys_days later{2025y / 3 / 1};
  const auto amend_delisting_day = [&](listing_id listing, sys_days delisted_on) {
    amend_listing(market.secmaster, listing,
                  {.currency = {}, .is_primary = {}, .listed_on = {}, .delisted_on = delisted_on});
  };
  delist(market.secmaster, market.acme_listing, delisting_day);

  amend_delisting_day(market.acme_listing, later);
  EXPECT_TRUE(acme_is_listed_on(market, delisting_day));
  EXPECT_FALSE(acme_is_listed_on(market, later));
  EXPECT_EQ(symbols_of(market, market.acme_listing).back().valid.until, later);
  EXPECT_EQ(spec_validities_of(market, market.acme_listing).back().until, later);

  amend_delisting_day(market.acme_listing, rename_day);
  EXPECT_FALSE(acme_is_listed_on(market, rename_day));
  EXPECT_EQ(symbols_of(market, market.acme_listing).back().valid.until, rename_day);

  EXPECT_THROW(amend_delisting_day(market.bolt_listing, later), error);
  EXPECT_EQ(count(market.secmaster, "listing WHERE listing_id = 2 AND delisted_on IS NULL"), 1);
  EXPECT_THROW(amend_delisting_day(market.acme_listing, listing_day), std::runtime_error);
  EXPECT_EQ(symbols_of(market, market.acme_listing).back().valid.until, rename_day);
}

TEST(SecmasterAmend, UndoDelistingReopensTheListingWithItsSymbolAndSpec) {
  market market = market_with_acme_and_bolt_listed();
  assign_symbol(market.secmaster, market.acme_listing, "VENUE", "ACMX", rename_day);
  delist(market.secmaster, market.acme_listing, delisting_day);

  undelist(market.secmaster, market.acme_listing);
  EXPECT_TRUE(acme_is_listed_on(market, sys_days{2030y / 1 / 1}));
  EXPECT_EQ(symbols_of(market, market.acme_listing),
            (std::vector{symbol_period{{.from = listing_day, .until = rename_day}, "ACME"},
                         symbol_period{{.from = rename_day, .until = std::nullopt}, "ACMX"}}));
  EXPECT_EQ(spec_validities_of(market, market.acme_listing), (std::vector{always_listed}));

  EXPECT_THROW(undelist(market.secmaster, market.acme_listing), error);
  EXPECT_THROW(undelist(market.secmaster, listing_id{99}), error);
}

TEST(SecmasterAmend, UndoDelistingFailsWhileAnotherListingHasTheSymbol) {
  market market = market_with_acme_and_bolt_listed();
  delist(market.secmaster, market.acme_listing, delisting_day);
  list_on_test_exchange(market, add_equity(market.secmaster, "Acme Two"), "ACME", delisting_day);

  EXPECT_THROW(undelist(market.secmaster, market.acme_listing), std::runtime_error);
  EXPECT_FALSE(acme_is_listed_on(market, delisting_day));
  EXPECT_EQ(symbols_of(market, market.acme_listing).back().valid.until, delisting_day);
}

TEST(SecmasterAmend, ReplaceSymbolsRewritesEveryPeriodOfOneSource) {
  market market = market_with_acme_and_bolt_listed();
  assign_symbol(market.secmaster, market.acme_listing, "VENDOR", "ACME.T", listing_day);
  const sys_days first_rename{2022y / 1 / 1};
  const std::vector corrected{
      symbol_period{{.from = listing_day, .until = first_rename}, "ACM"},
      symbol_period{{.from = first_rename, .until = rename_day}, "ACME"},
      symbol_period{{.from = rename_day, .until = std::nullopt}, "ACMX"}};

  set_symbols(market.secmaster, market.acme_listing, "VENUE", corrected);
  EXPECT_EQ(symbols_of(market, market.acme_listing), corrected);
  EXPECT_EQ(symbols_of(market, market.acme_listing, "VENDOR").size(), 1u);
  EXPECT_EQ(symbols_of(market, market.bolt_listing).front().symbol, "BOLT");

  set_symbols(market.secmaster, market.acme_listing, "VENDOR", {});
  EXPECT_EQ(count(market.secmaster, "listing_symbol WHERE source = 'VENDOR'"), 0);
}

TEST(SecmasterAmend, ReplaceSymbolsKeepsThePeriodsItHadWhenTheNewOnesBreakARule) {
  market market = market_with_acme_and_bolt_listed();
  const std::vector<symbol_period> stored = symbols_of(market, market.acme_listing);
  const auto replace_with = [&](std::vector<symbol_period> periods) {
    set_symbols(market.secmaster, market.acme_listing, "VENUE", periods);
  };

  EXPECT_THROW(replace_with({{{.from = listing_day, .until = std::nullopt}, "ACME"},
                             {{.from = rename_day, .until = std::nullopt}, "ACMX"}}),
               std::runtime_error);
  EXPECT_THROW(replace_with({{{.from = sys_days{2019y / 1 / 1}, .until = std::nullopt}, "ACME"}}),
               std::runtime_error);
  EXPECT_THROW(replace_with({{{.from = listing_day, .until = std::nullopt}, "BOLT"}}),
               std::runtime_error);
  EXPECT_THROW(replace_with({{{.from = rename_day, .until = listing_day}, "ACME"}}),
               std::runtime_error);
  EXPECT_THROW(set_symbols(market.secmaster, listing_id{99}, "VENUE", {}), error);
  EXPECT_EQ(symbols_of(market, market.acme_listing), stored);
}

TEST(SecmasterAmend, ReplaceListingSpecsRewritesEveryPeriod) {
  market market = market_with_acme_and_bolt_listed();
  const listing_spec lots_of_hundred{.lot_size = quantity_nanos{100 * nanos_per_unit},
                                     .min_qty = quantity_nanos{200 * nanos_per_unit},
                                     .tick_rule = market.cent};
  const listing_spec single_shares{.lot_size = quantity_nanos{nanos_per_unit}, .min_qty = {},
                                   .tick_rule = market.milli};

  set_listing_specs(
      market.secmaster, market.acme_listing,
      std::array{listing_spec_period{{.from = listing_day, .until = rename_day}, lots_of_hundred},
                 listing_spec_period{{.from = rename_day, .until = std::nullopt}, single_shares}});

  const history loaded = load_history(market.secmaster, every_day, everything);
  const std::span<const spec_period> specs =
      loaded.specs_of(*loaded.listings().find(market.acme_listing));
  ASSERT_EQ(specs.size(), 2u);
  EXPECT_EQ(specs[0].valid, (validity{.from = listing_day, .until = rename_day}));
  EXPECT_EQ(specs[0].lot_size, lots_of_hundred.lot_size);
  EXPECT_EQ(specs[0].min_qty, lots_of_hundred.min_qty);
  EXPECT_EQ(loaded.tick_rules()[specs[0].tick_rule].name, "CENT");
  EXPECT_EQ(specs[1].valid, (validity{.from = rename_day, .until = std::nullopt}));
  EXPECT_EQ(loaded.tick_rules()[specs[1].tick_rule].name, "MILLI");
  EXPECT_EQ(spec_validities_of(market, market.bolt_listing), (std::vector{always_listed}));

  EXPECT_THROW(set_listing_specs(
                   market.secmaster, market.acme_listing,
                   std::array{listing_spec_period{always_listed, lots_of_hundred},
                              listing_spec_period{{.from = rename_day, .until = std::nullopt},
                                                  single_shares}}),
               std::runtime_error);
  EXPECT_THROW(set_listing_specs(market.secmaster, listing_id{99}, {}), error);
  EXPECT_EQ(spec_validities_of(market, market.acme_listing).size(), 2u);
}

TEST(SecmasterAmend, ReplaceIdentifiersRewritesEveryPeriodOfOneScheme) {
  market market = market_with_acme_and_bolt_listed();
  assign_identifier(market.secmaster, market.acme, "ISIN", "ZZ0000000001", listing_day);
  assign_identifier(market.secmaster, market.acme, "CUSIP", "000000001", listing_day);

  set_identifiers(
      market.secmaster, market.acme, "ISIN",
      std::array{identifier_period{{.from = listing_day, .until = rename_day}, "ZZ0000000010"},
                 identifier_period{{.from = rename_day, .until = std::nullopt}, "ZZ0000000020"}});
  EXPECT_EQ(count(market.secmaster, "instrument_identifier WHERE scheme = 'ISIN'"), 2);
  EXPECT_EQ(count(market.secmaster, "instrument_identifier WHERE value = 'ZZ0000000010' "
                                    "AND valid_from = '2020-01-01' AND valid_until = '2024-06-01'"),
            1);
  EXPECT_EQ(count(market.secmaster, "instrument_identifier WHERE value = 'ZZ0000000020' "
                                    "AND valid_from = '2024-06-01' AND valid_until IS NULL"),
            1);
  EXPECT_EQ(count(market.secmaster, "instrument_identifier WHERE scheme = 'CUSIP'"), 1);

  EXPECT_THROW(set_identifiers(market.secmaster, instrument_id{99}, "ISIN", {}), error);
  set_identifiers(market.secmaster, market.acme, "ISIN", {});
  EXPECT_EQ(count(market.secmaster, "instrument_identifier WHERE scheme = 'ISIN'"), 0);
}

new_corporate_action split_of(instrument_id instrument, std::int64_t new_shares) {
  return {.instrument = instrument,
          .type = corporate_action_type::split,
          .ex_date = rename_day,
          .announce_date = {},
          .record_date = {},
          .pay_date = {},
          .cash_per_share = {},
          .currency = {},
          .new_shares = new_shares,
          .old_shares = 1,
          .related_instrument = {}};
}

TEST(SecmasterAmend, AmendCorporateActionOverwritesEveryField) {
  market market = market_with_acme_and_bolt_listed();
  const corporate_action_id split =
      add_corporate_action(market.secmaster, split_of(market.acme, 2));

  amend_corporate_action(market.secmaster, split,
                         {.instrument = market.acme,
                          .type = corporate_action_type::cash_dividend,
                          .ex_date = delisting_day,
                          .announce_date = rename_day,
                          .record_date = {},
                          .pay_date = {},
                          .cash_per_share = price_nanos{nanos_per_unit / 2},
                          .currency = "TST",
                          .new_shares = {},
                          .old_shares = {},
                          .related_instrument = {}});
  EXPECT_EQ(count(market.secmaster,
                  "corporate_action WHERE corporate_action_id = 1 "
                  "AND action_type = 'CASH_DIVIDEND' AND ex_date = '2025-01-01' "
                  "AND announce_date = '2024-06-01' AND cash_per_share_nanos = 500000000 "
                  "AND currency = 'TST' AND new_shares IS NULL AND old_shares IS NULL"),
            1);
  EXPECT_THROW(
      amend_corporate_action(market.secmaster, corporate_action_id{99}, split_of(market.acme, 3)),
      error);
}

TEST(SecmasterAmend, RemoveCorporateActionDeletesIt) {
  market market = market_with_acme_and_bolt_listed();
  const corporate_action_id split =
      add_corporate_action(market.secmaster, split_of(market.acme, 2));
  add_corporate_action(market.secmaster, split_of(market.acme, 3));

  remove_corporate_action(market.secmaster, split);
  EXPECT_EQ(count(market.secmaster, "corporate_action"), 1);
  EXPECT_EQ(count(market.secmaster, "corporate_action WHERE new_shares = 3"), 1);
  EXPECT_THROW(remove_corporate_action(market.secmaster, split), error);
}

TEST(SecmasterAmend, EveryWriteIsLoggedWithTheRowBeforeAndAfter) {
  market market = market_with_acme_and_bolt_listed();
  const corporate_action_id split =
      add_corporate_action(market.secmaster, split_of(market.acme, 2));
  amend_corporate_action(market.secmaster, split, split_of(market.acme, 3));
  remove_corporate_action(market.secmaster, split);

  const std::vector<change> changes = read_changes(market.secmaster, "corporate_action");
  ASSERT_EQ(changes.size(), 3u);
  EXPECT_EQ(changes[0].operation, "INSERT");
  EXPECT_EQ(changes[0].table, "corporate_action");
  EXPECT_EQ(changes[0].old_row, std::nullopt);
  EXPECT_NE(changes[0].new_row->find(R"("new_shares":2)"), std::string::npos);
  EXPECT_EQ(changes[1].operation, "UPDATE");
  EXPECT_EQ(changes[1].old_row, changes[0].new_row);
  EXPECT_NE(changes[1].new_row->find(R"("new_shares":3)"), std::string::npos);
  EXPECT_EQ(changes[2].operation, "DELETE");
  EXPECT_EQ(changes[2].old_row, changes[1].new_row);
  EXPECT_EQ(changes[2].new_row, std::nullopt);
  EXPECT_LT(changes[0].id, changes[1].id);
  EXPECT_EQ(changes[0].changed_at.size(), std::string("2026-01-31T12:00:00.000Z").size());

  EXPECT_GT(read_changes(market.secmaster).size(), changes.size());
  EXPECT_TRUE(read_changes(market.secmaster, "issuer").empty());
}

TEST(SecmasterAmend, AnAmendmentThatChangesNothingIsNotLogged) {
  market market = market_with_acme_and_bolt_listed();
  const std::size_t logged = read_changes(market.secmaster).size();

  amend_venue(market.secmaster, market.test_exchange,
              {.code = "XTST", .mic = {}, .name = "Test Exchange", .country = {}, .timezone = {}});
  amend_listing(market.secmaster, market.acme_listing,
                {.currency = "TST", .is_primary = {}, .listed_on = listing_day,
                 .delisted_on = {}});
  EXPECT_EQ(read_changes(market.secmaster).size(), logged);
}

TEST(SecmasterAmend, AFailedWriteIsNotLogged) {
  market market = market_with_acme_and_bolt_listed();
  const std::size_t logged = read_changes(market.secmaster).size();

  EXPECT_THROW(assign_symbol(market.secmaster, market.acme_listing, "VENUE", "BOLT", rename_day),
               std::runtime_error);
  EXPECT_THROW(set_symbols(market.secmaster, market.acme_listing, "VENUE",
                               std::array{symbol_period{always_listed, "BOLT"}}),
               std::runtime_error);
  EXPECT_EQ(read_changes(market.secmaster).size(), logged);
}

}  // namespace
