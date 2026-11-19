#include <nq/db/secmaster/writer.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
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
using std::chrono::days;
using std::chrono::sys_days;

constexpr sys_days listing_day{2020y / 1 / 1};
constexpr sys_days rename_day{2024y / 6 / 1};
constexpr sys_days delisting_day{2025y / 1 / 1};

constexpr auto everything = [](const venue&, const instrument&) { return true; };

struct market {
  database secmaster;
  venue_id test_exchange;
  tick_rule_id cent;
  instrument_id acme;
  listing_id acme_listing;
  listing_id bolt_listing;
};

instrument_id add_equity(database& secmaster, const std::string& name) {
  return add_instrument(secmaster, {.asset_class = asset_class::equity, .name = name,
                                    .issuer = {}, .cfi = {}, .spec = no_spec{}});
}

new_listing listing_on(const market& market, instrument_id instrument, const std::string& symbol,
                       sys_days listed_on) {
  return {.instrument = instrument,
          .venue = market.test_exchange,
          .currency = "TST",
          .is_primary = true,
          .listed_on = listed_on,
          .symbol = symbol,
          .spec = {.lot_size = quantity_nanos{nanos_per_unit}, .min_qty = {},
                   .tick_rule = market.cent}};
}

market market_with_acme_and_bolt_listed() {
  market market{.secmaster = database::in_memory(), .test_exchange = {}, .cent = {}, .acme = {},
                .acme_listing = {}, .bolt_listing = {}};
  create_schema(market.secmaster);
  market.test_exchange =
      add_venue(market.secmaster, {.code = "XTST", .mic = "XTST", .name = "Test Exchange",
                                   .country = "ZZ", .timezone = "Etc/UTC"});
  market.cent = add_tick_rule(market.secmaster, "CENT",
                              std::array{tick_band{price_nanos{0}, price_nanos{10'000'000}}});
  market.acme = add_equity(market.secmaster, "Acme");
  market.acme_listing =
      add_listing(market.secmaster, listing_on(market, market.acme, "ACME", listing_day));
  market.bolt_listing = add_listing(
      market.secmaster,
      listing_on(market, add_equity(market.secmaster, "Bolt"), "BOLT", listing_day));
  return market;
}

const listing_on_day& listing_of(const day_view& loaded, listing_id id) {
  return loaded.listings()[*loaded.listings().find(id)];
}

std::string acme_symbol_on(market& market, sys_days day) {
  return listing_of(day_view(market.secmaster, day, everything), market.acme_listing).symbol;
}

std::int64_t count(database& secmaster, const std::string& table_and_filter) {
  nq::db::sqlite::statement row = secmaster.prepare("SELECT count(*) FROM " + table_and_filter);
  row.next_row();
  return row.get<std::int64_t>(0);
}

std::string open_symbol(database& secmaster, listing_id listing) {
  nq::db::sqlite::statement row = secmaster.prepare(
      "SELECT symbol FROM listing_symbol WHERE listing_id = ?1 AND source = 'VENUE' "
      "AND valid_until IS NULL");
  row.bind(1, listing);
  return row.next_row() ? row.get<std::string>(0) : "none";
}

TEST(SecmasterWriter, AddedRecordsLoadWithTheValuesGiven) {
  market market = market_with_acme_and_bolt_listed();
  const day_view loaded = day_view(market.secmaster, listing_day, everything);

  ASSERT_EQ(loaded.listings().size(), 2u);
  const listing_on_day& acme_listing = listing_of(loaded, market.acme_listing);
  EXPECT_EQ(acme_listing.id, market.acme_listing);
  EXPECT_EQ(acme_listing.symbol, "ACME");
  EXPECT_EQ(acme_listing.currency, "TST");
  EXPECT_TRUE(acme_listing.is_primary);
  EXPECT_EQ(acme_listing.lot_size, quantity_nanos{nanos_per_unit});
  EXPECT_EQ(acme_listing.min_qty, std::nullopt);

  const venue& test_exchange = loaded.venues()[acme_listing.venue];
  EXPECT_EQ(test_exchange.id, market.test_exchange);
  EXPECT_EQ(test_exchange.mic, "XTST");
  EXPECT_EQ(test_exchange.country, "ZZ");

  const tick_rule& cent = loaded.tick_rules()[acme_listing.tick_rule];
  EXPECT_EQ(cent.id, market.cent);
  EXPECT_EQ(cent.tick_size_at(price_nanos{5 * nanos_per_unit}), price_nanos{10'000'000});

  const instrument& acme = loaded.instruments()[acme_listing.instrument];
  EXPECT_EQ(acme.id, market.acme);
  EXPECT_EQ(acme.asset_class, asset_class::equity);
  EXPECT_EQ(acme.name, "Acme");
}

TEST(SecmasterWriter, FindsVenuesAndTickRulesByName) {
  market market = market_with_acme_and_bolt_listed();
  EXPECT_EQ(find_venue(market.secmaster, "XTST"), market.test_exchange);
  EXPECT_EQ(find_venue(market.secmaster, "NONE"), std::nullopt);
  EXPECT_EQ(find_tick_rule(market.secmaster, "CENT"), market.cent);
  EXPECT_EQ(find_tick_rule(market.secmaster, "NONE"), std::nullopt);
}

TEST(SecmasterWriter, AddInstrumentStoresTheSpecOfItsAssetClass) {
  market market = market_with_acme_and_bolt_listed();
  const future_spec future{.underlying = market.acme,
                           .root = "AC",
                           .expiry = sys_days{2026y / 12 / 18},
                           .multiplier = multiplier_nanos{50 * nanos_per_unit},
                           .settlement = settlement_method::physical};
  const option_spec option{.underlying = market.acme,
                           .expiry = sys_days{2026y / 12 / 18},
                           .strike = price_nanos{50 * nanos_per_unit},
                           .right = option_right::put,
                           .style = exercise_style::european,
                           .multiplier = multiplier_nanos{100 * nanos_per_unit}};
  const pair_spec pair{.base_currency = "FOO", .quote_currency = "BAR"};
  const auto list = [&](asset_class asset_class, instrument_spec spec, std::string symbol) {
    const instrument_id added = add_instrument(
        market.secmaster, {.asset_class = asset_class, .name = symbol, .issuer = {}, .cfi = {},
                           .spec = std::move(spec)});
    add_listing(market.secmaster,
                {.instrument = added,
                 .venue = market.test_exchange,
                 .currency = "TST",
                 .is_primary = false,
                 .listed_on = listing_day,
                 .symbol = std::move(symbol),
                 .spec = {.lot_size = quantity_nanos{nanos_per_unit},
                          .min_qty = quantity_nanos{5},
                          .tick_rule = market.cent}});
    return added;
  };
  const instrument_id added_future = list(asset_class::future, future, "ACZ6");
  const instrument_id added_option = list(asset_class::option, option, "ACME P50");
  const instrument_id added_pair = list(asset_class::crypto, pair, "FOOBAR");

  const day_view loaded = day_view(market.secmaster, listing_day, everything);
  const auto spec_of = [&](instrument_id id) {
    return loaded.instruments()[*loaded.instruments().find(id)].spec;
  };
  const future_spec stored_future = std::get<future_spec>(spec_of(added_future));
  EXPECT_EQ(stored_future.underlying, future.underlying);
  EXPECT_EQ(stored_future.root, future.root);
  EXPECT_EQ(stored_future.expiry, future.expiry);
  EXPECT_EQ(stored_future.multiplier, future.multiplier);
  EXPECT_EQ(stored_future.settlement, future.settlement);

  const option_spec stored_option = std::get<option_spec>(spec_of(added_option));
  EXPECT_EQ(stored_option.underlying, option.underlying);
  EXPECT_EQ(stored_option.expiry, option.expiry);
  EXPECT_EQ(stored_option.strike, option.strike);
  EXPECT_EQ(stored_option.right, option.right);
  EXPECT_EQ(stored_option.style, option.style);
  EXPECT_EQ(stored_option.multiplier, option.multiplier);

  const pair_spec stored_pair = std::get<pair_spec>(spec_of(added_pair));
  EXPECT_EQ(stored_pair.base_currency, "FOO");
  EXPECT_EQ(stored_pair.quote_currency, "BAR");

  const auto last_listing = *loaded.find_listing("VENUE", *loaded.find_venue("XTST"), "FOOBAR");
  EXPECT_EQ(loaded.listings()[last_listing].min_qty, quantity_nanos{5});
}

TEST(SecmasterWriter, AddInstrumentRejectsASpecOfAnotherAssetClassAndStoresNothing) {
  market market = market_with_acme_and_bolt_listed();
  const pair_spec pair{.base_currency = "FOO", .quote_currency = "BAR"};
  EXPECT_THROW(add_instrument(market.secmaster, {.asset_class = asset_class::equity,
                                                 .name = "Mismatch", .issuer = {}, .cfi = {},
                                                 .spec = pair}),
               error);
  EXPECT_THROW(add_instrument(market.secmaster, {.asset_class = asset_class::future,
                                                 .name = "Mismatch", .issuer = {}, .cfi = {},
                                                 .spec = no_spec{}}),
               error);
  EXPECT_EQ(count(market.secmaster, "instrument"), 2);
}

TEST(SecmasterWriter, AFailedStepLeavesNoPartOfTheRecordBehind) {
  market market = market_with_acme_and_bolt_listed();
  const future_spec future_on_a_missing_underlying{.underlying = instrument_id{999},
                                                   .root = "AC",
                                                   .expiry = sys_days{2026y / 12 / 18},
                                                   .multiplier = multiplier_nanos{nanos_per_unit},
                                                   .settlement = settlement_method::cash};
  EXPECT_THROW(add_instrument(market.secmaster, {.asset_class = asset_class::future,
                                                 .name = "Orphan", .issuer = {}, .cfi = {},
                                                 .spec = future_on_a_missing_underlying}),
               nq::db::sqlite::error);
  EXPECT_EQ(count(market.secmaster, "instrument"), 2);

  EXPECT_THROW(add_listing(market.secmaster,
                           {.instrument = market.acme,
                            .venue = market.test_exchange,
                            .currency = "ALT",
                            .is_primary = false,
                            .listed_on = listing_day,
                            .symbol = "ACME",
                            .spec = {.lot_size = quantity_nanos{nanos_per_unit}, .min_qty = {},
                                     .tick_rule = market.cent}}),
               nq::db::sqlite::error);
  EXPECT_EQ(count(market.secmaster, "listing"), 2);

  EXPECT_THROW(add_tick_rule(market.secmaster, "EMPTY", {}), error);
  const std::array same_price_twice{tick_band{price_nanos{0}, price_nanos{1}},
                                    tick_band{price_nanos{0}, price_nanos{2}}};
  EXPECT_THROW(add_tick_rule(market.secmaster, "TWICE", same_price_twice), nq::db::sqlite::error);
  EXPECT_EQ(count(market.secmaster, "tick_rule"), 1);
}

TEST(SecmasterWriter, AssignSymbolRenamesFromTheDayGiven) {
  market market = market_with_acme_and_bolt_listed();
  assign_symbol(market.secmaster, market.acme_listing, "VENUE", "ACMX", rename_day);

  EXPECT_EQ(acme_symbol_on(market, rename_day - days{1}),
            "ACME");
  EXPECT_EQ(acme_symbol_on(market, rename_day), "ACMX");
}

TEST(SecmasterWriter, AssignSymbolAddsTheFirstSymbolOfAnotherSource) {
  market market = market_with_acme_and_bolt_listed();
  assign_symbol(market.secmaster, market.acme_listing, "VENDOR", "ACME.T", rename_day);

  const day_view loaded = day_view(market.secmaster, rename_day, everything);
  const venue_index test_exchange = *loaded.find_venue("XTST");
  EXPECT_EQ(loaded.find_listing("VENDOR", test_exchange, "ACME.T"),
            loaded.listings().find(market.acme_listing));
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACME"),
            loaded.listings().find(market.acme_listing));
}

TEST(SecmasterWriter, AssignSymbolBeforeTheCurrentOneStartedChangesNothing) {
  market market = market_with_acme_and_bolt_listed();
  EXPECT_THROW(
      assign_symbol(market.secmaster, market.acme_listing, "VENUE", "ACMX", listing_day - days{1}),
      nq::db::sqlite::error);
  EXPECT_THROW(assign_symbol(market.secmaster, market.acme_listing, "VENUE", "ACMX", listing_day),
               nq::db::sqlite::error);
  EXPECT_EQ(acme_symbol_on(market, listing_day), "ACME");
}

TEST(SecmasterWriter, AssignListingSpecAppliesFromTheDayGiven) {
  market market = market_with_acme_and_bolt_listed();
  const std::array bands{tick_band{price_nanos{0}, price_nanos{100'000}}};
  const tick_rule_id fine = add_tick_rule(market.secmaster, "FINE", bands);
  assign_listing_spec(market.secmaster, market.acme_listing,
                      {.lot_size = quantity_nanos{100 * nanos_per_unit},
                       .min_qty = quantity_nanos{nanos_per_unit},
                       .tick_rule = fine},
                      rename_day);

  const day_view before = day_view(market.secmaster, rename_day - days{1}, everything);
  EXPECT_EQ(listing_of(before, market.acme_listing).lot_size, quantity_nanos{nanos_per_unit});
  EXPECT_EQ(before.tick_rules()[listing_of(before, market.acme_listing).tick_rule].name, "CENT");

  const day_view after = day_view(market.secmaster, rename_day, everything);
  EXPECT_EQ(listing_of(after, market.acme_listing).lot_size, quantity_nanos{100 * nanos_per_unit});
  EXPECT_EQ(listing_of(after, market.acme_listing).min_qty, quantity_nanos{nanos_per_unit});
  EXPECT_EQ(after.tick_rules()[listing_of(after, market.acme_listing).tick_rule].name, "FINE");
}

TEST(SecmasterWriter, WritesToOneListingLeaveTheOtherUnchanged) {
  market market = market_with_acme_and_bolt_listed();
  const std::array bands{tick_band{price_nanos{0}, price_nanos{100'000}}};
  const tick_rule_id fine = add_tick_rule(market.secmaster, "FINE", bands);
  assign_symbol(market.secmaster, market.acme_listing, "VENUE", "ACMX", rename_day);
  assign_listing_spec(market.secmaster, market.acme_listing,
                      {.lot_size = quantity_nanos{100 * nanos_per_unit}, .min_qty = {},
                       .tick_rule = fine},
                      rename_day);
  delist(market.secmaster, market.acme_listing, delisting_day);

  const day_view loaded = day_view(market.secmaster, delisting_day, everything);
  ASSERT_EQ(loaded.listings().size(), 1u);
  const listing_on_day& bolt = listing_of(loaded, market.bolt_listing);
  EXPECT_EQ(bolt.symbol, "BOLT");
  EXPECT_EQ(bolt.lot_size, quantity_nanos{nanos_per_unit});
  EXPECT_EQ(loaded.tick_rules()[bolt.tick_rule].name, "CENT");
}

TEST(SecmasterWriter, DelistEndsTheListingWithItsSymbolsAndSpec) {
  market market = market_with_acme_and_bolt_listed();
  assign_symbol(market.secmaster, market.acme_listing, "VENDOR", "ACME.T", listing_day);
  delist(market.secmaster, market.acme_listing, delisting_day);

  EXPECT_EQ(day_view(market.secmaster, delisting_day - days{1}, everything).listings().size(),
            2u);
  EXPECT_EQ(day_view(market.secmaster, delisting_day, everything).listings().size(), 1u);
  EXPECT_EQ(count(market.secmaster, "listing_symbol WHERE listing_id = 1 "
                                    "AND valid_until = '2025-01-01'"),
            2);
  EXPECT_EQ(count(market.secmaster, "listing_spec WHERE listing_id = 1 "
                                    "AND valid_until = '2025-01-01'"),
            1);
}

TEST(SecmasterWriter, ADelistedSymbolIsFreeForANewListing) {
  market market = market_with_acme_and_bolt_listed();
  delist(market.secmaster, market.acme_listing, delisting_day);
  const instrument_id successor = add_equity(market.secmaster, "Acme Successor");
  EXPECT_NO_THROW(
      add_listing(market.secmaster, listing_on(market, successor, "ACME", delisting_day)));
}

TEST(SecmasterWriter, DelistRejectsAnUnknownOrDelistedListingAndChangesNothing) {
  market market = market_with_acme_and_bolt_listed();
  EXPECT_THROW(delist(market.secmaster, listing_id{999}, delisting_day), error);

  delist(market.secmaster, market.acme_listing, delisting_day);
  EXPECT_THROW(delist(market.secmaster, market.acme_listing, delisting_day + days{30}), error);
  EXPECT_THROW(delist(market.secmaster, market.acme_listing, delisting_day - days{30}), error);
  EXPECT_EQ(count(market.secmaster, "listing WHERE listing_id = 1 "
                                    "AND delisted_on = '2025-01-01'"),
            1);
}

TEST(SecmasterWriter, ADelistedListingTakesNoNewSymbolOrSpec) {
  market market = market_with_acme_and_bolt_listed();
  delist(market.secmaster, market.acme_listing, delisting_day);

  EXPECT_THROW(assign_symbol(market.secmaster, market.acme_listing, "VENUE", "ACMX",
                             delisting_day + days{1}),
               nq::db::sqlite::error);
  EXPECT_THROW(assign_symbol(market.secmaster, market.acme_listing, "VENDOR", "ACME.T",
                             delisting_day - days{1}),
               nq::db::sqlite::error);
  EXPECT_THROW(assign_listing_spec(market.secmaster, market.acme_listing,
                                   {.lot_size = quantity_nanos{nanos_per_unit}, .min_qty = {},
                                    .tick_rule = market.cent},
                                   delisting_day + days{1}),
               nq::db::sqlite::error);
  EXPECT_EQ(count(market.secmaster, "listing_symbol WHERE listing_id = 1"), 1);
  EXPECT_EQ(count(market.secmaster, "listing_spec WHERE listing_id = 1"), 1);
}

TEST(SecmasterWriter, AFailedAssignmentKeepsThePreviousValueOpen) {
  market market = market_with_acme_and_bolt_listed();
  EXPECT_THROW(assign_symbol(market.secmaster, market.acme_listing, "VENUE", "BOLT", rename_day),
               nq::db::sqlite::error);
  EXPECT_EQ(open_symbol(market.secmaster, market.acme_listing), "ACME");

  EXPECT_THROW(assign_listing_spec(market.secmaster, market.acme_listing,
                                   {.lot_size = quantity_nanos{nanos_per_unit}, .min_qty = {},
                                    .tick_rule = tick_rule_id{999}},
                                   rename_day),
               nq::db::sqlite::error);
  EXPECT_EQ(count(market.secmaster, "listing_spec WHERE listing_id = 1 AND valid_until IS NULL"),
            1);

  const instrument_id bolt = add_equity(market.secmaster, "Bolt Twin");
  assign_identifier(market.secmaster, market.acme, "ISIN", "ZZ0000000001", listing_day);
  assign_identifier(market.secmaster, bolt, "ISIN", "ZZ0000000002", listing_day);
  EXPECT_THROW(assign_identifier(market.secmaster, market.acme, "ISIN", "ZZ0000000002", rename_day),
               nq::db::sqlite::error);
  EXPECT_EQ(count(market.secmaster, "instrument_identifier WHERE value = 'ZZ0000000001' "
                                    "AND valid_until IS NULL"),
            1);
}

TEST(SecmasterWriter, AddVenueAndAddIssuerStoreEveryField) {
  market market = market_with_acme_and_bolt_listed();
  const venue_id bare = add_venue(market.secmaster, {.code = "XBAR", .mic = {}, .name = "Bare",
                                                     .country = {}, .timezone = "Etc/GMT"});
  EXPECT_EQ(count(market.secmaster, "venue WHERE venue_id = 1 AND code = 'XTST' AND mic = 'XTST' "
                                    "AND name = 'Test Exchange' AND country = 'ZZ' "
                                    "AND timezone = 'Etc/UTC'"),
            1);
  EXPECT_EQ(count(market.secmaster, std::format("venue WHERE venue_id = {} AND code = 'XBAR' "
                                                "AND mic IS NULL AND name = 'Bare' "
                                                "AND country IS NULL AND timezone = 'Etc/GMT'",
                                                bare.value())),
            1);

  const issuer_id holdings = add_issuer(
      market.secmaster, {.name = "Acme Holdings", .lei = "ZZZZ0000000000000001", .country = "ZZ"});
  add_issuer(market.secmaster, {.name = "Bolt Holdings", .lei = {}, .country = {}});
  const std::vector<issuer> issuers = read_issuers(market.secmaster);
  ASSERT_EQ(issuers.size(), 2u);
  EXPECT_EQ(issuers[0].id, holdings);
  EXPECT_EQ(issuers[0].name, "Acme Holdings");
  EXPECT_EQ(issuers[0].lei, "ZZZZ0000000000000001");
  EXPECT_EQ(issuers[0].country, "ZZ");
  EXPECT_EQ(issuers[1].lei, std::nullopt);
  EXPECT_EQ(issuers[1].country, std::nullopt);

  const instrument_id issued = add_instrument(
      market.secmaster, {.asset_class = asset_class::etf, .name = "Acme Fund",
                         .issuer = holdings, .cfi = "CEOGDU", .spec = no_spec{}});
  EXPECT_EQ(count(market.secmaster,
                  std::format("instrument WHERE instrument_id = {} AND issuer_id = {} "
                              "AND cfi = 'CEOGDU' AND asset_class = 'ETF'",
                              issued.value(), holdings.value())),
            1);
  EXPECT_EQ(read_instruments(market.secmaster).back().name, "Acme Fund");
}

TEST(SecmasterWriter, AssignIdentifierEndsThePreviousOneOfItsScheme) {
  market market = market_with_acme_and_bolt_listed();
  assign_identifier(market.secmaster, market.acme, "ISIN", "ZZ0000000001", listing_day);
  assign_identifier(market.secmaster, market.acme, "FIGI", "ZZG000000001", listing_day);
  assign_identifier(market.secmaster, market.acme, "ISIN", "ZZ0000000002", rename_day);

  nq::db::sqlite::statement row = market.secmaster.prepare(
      "SELECT scheme, value, valid_from, valid_until FROM instrument_identifier "
      "ORDER BY scheme, valid_from");
  using identifier_row = std::array<std::string, 4>;
  const auto next_identifier = [&] {
    row.next_row();
    return std::array{row.get<std::string>(0), row.get<std::string>(1), row.get<std::string>(2),
                      row.get<std::optional<std::string>>(3).value_or("open")};
  };
  EXPECT_EQ(next_identifier(), (identifier_row{"FIGI", "ZZG000000001", "2020-01-01", "open"}));
  EXPECT_EQ(next_identifier(),
            (identifier_row{"ISIN", "ZZ0000000001", "2020-01-01", "2024-06-01"}));
  EXPECT_EQ(next_identifier(), (identifier_row{"ISIN", "ZZ0000000002", "2024-06-01", "open"}));
}

TEST(SecmasterWriter, RecordCorporateActionStoresEveryField) {
  market market = market_with_acme_and_bolt_listed();
  const instrument_id spun_off = add_equity(market.secmaster, "Acme Parts");
  const corporate_action_id split = add_corporate_action(
      market.secmaster, {.instrument = market.acme,
                         .type = corporate_action_type::split,
                         .ex_date = rename_day,
                         .announce_date = rename_day - days{30},
                         .record_date = {},
                         .pay_date = {},
                         .cash_per_share = {},
                         .currency = {},
                         .new_shares = 3,
                         .old_shares = 2,
                         .related_instrument = {}});
  const corporate_action_id spinoff = add_corporate_action(
      market.secmaster, {.instrument = spun_off,
                         .type = corporate_action_type::spinoff,
                         .ex_date = rename_day,
                         .announce_date = {},
                         .record_date = rename_day + days{1},
                         .pay_date = rename_day + days{2},
                         .cash_per_share = price_nanos{250'000'000},
                         .currency = "TST",
                         .new_shares = {},
                         .old_shares = {},
                         .related_instrument = market.acme});
  EXPECT_LT(split, spinoff);

  nq::db::sqlite::statement row = market.secmaster.prepare(
      "SELECT action_type, ex_date, announce_date, record_date, pay_date, cash_per_share_nanos, "
      "currency, new_shares, old_shares, related_instrument_id, instrument_id "
      "FROM corporate_action ORDER BY corporate_action_id");
  ASSERT_TRUE(row.next_row());
  EXPECT_EQ(row.get<instrument_id>(10), market.acme);
  EXPECT_EQ(row.get<std::string>(0), "SPLIT");
  EXPECT_EQ(row.get<std::string>(1), "2024-06-01");
  EXPECT_EQ(row.get<std::string>(2), "2024-05-02");
  EXPECT_TRUE(row.is_null(3) && row.is_null(4) && row.is_null(5) && row.is_null(6));
  EXPECT_EQ(row.get<std::int64_t>(7), 3);
  EXPECT_EQ(row.get<std::int64_t>(8), 2);
  EXPECT_TRUE(row.is_null(9));

  ASSERT_TRUE(row.next_row());
  EXPECT_EQ(row.get<std::string>(0), "SPINOFF");
  EXPECT_TRUE(row.is_null(2));
  EXPECT_EQ(row.get<std::string>(3), "2024-06-02");
  EXPECT_EQ(row.get<std::string>(4), "2024-06-03");
  EXPECT_EQ(row.get<std::int64_t>(5), 250'000'000);
  EXPECT_EQ(row.get<std::string>(6), "TST");
  EXPECT_TRUE(row.is_null(7) && row.is_null(8));
  EXPECT_EQ(row.get<instrument_id>(9), market.acme);
  EXPECT_EQ(row.get<instrument_id>(10), spun_off);
}

TEST(SecmasterWriter, ACallersTransactionUndoesSeveralWritesTogether) {
  market market = market_with_acme_and_bolt_listed();
  {
    nq::db::sqlite::transaction batch(market.secmaster);
    assign_symbol(market.secmaster, market.acme_listing, "VENUE", "ACMX", rename_day);
    delist(market.secmaster, market.acme_listing, delisting_day);
  }
  const day_view loaded = day_view(market.secmaster, delisting_day, everything);
  ASSERT_EQ(loaded.listings().size(), 2u);
  EXPECT_EQ(listing_of(loaded, market.acme_listing).symbol, "ACME");
}

}  // namespace
