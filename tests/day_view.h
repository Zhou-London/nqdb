#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <nlib/id_table.h>
#include <nq/db/secmaster/history.h>
#include <nq/db/secmaster/loader.h>
#include <nq/db/sqlite.h>

namespace nqdb_test {

using namespace nq::db::secmaster;

struct listing_on_day {
  listing_id id;
  instrument_index instrument;
  venue_index venue;
  std::string symbol;
  std::string currency;
  bool is_primary;
  quantity_nanos lot_size;
  std::optional<quantity_nanos> min_qty;
  tick_rule_index tick_rule;
};

class day_view {
 public:
  template <typename Universe>
  day_view(nq::db::sqlite::database& database, std::chrono::sys_days day, Universe in_universe)
      : day_(day),
        loaded_(load_history(database, {.from = day, .until = day + std::chrono::days{1}},
                             std::move(in_universe))) {
    for (const listing& listing : loaded_.listings()) {
      const listing_index index = *loaded_.listings().find(listing.id);
      if (!loaded_.is_listed(index, day)) continue;
      const spec_period& spec = *loaded_.spec_on(index, day);
      listings_.insert(listing_on_day{
          .id = listing.id,
          .instrument = listing.instrument,
          .venue = listing.venue,
          .symbol = loaded_.symbol_on(venue_symbol_source, index, day)->symbol,
          .currency = listing.currency,
          .is_primary = listing.is_primary,
          .lot_size = spec.lot_size,
          .min_qty = spec.min_qty,
          .tick_rule = spec.tick_rule});
    }
  }

  const nlib::id_table<listing_index, listing_on_day>& listings() const { return listings_; }
  const history::venue_table& venues() const { return loaded_.venues(); }
  const history::instrument_table& instruments() const { return loaded_.instruments(); }
  const history::tick_rule_table& tick_rules() const { return loaded_.tick_rules(); }

  std::optional<venue_index> find_venue(std::string_view code) const {
    return loaded_.find_venue(code);
  }

  std::optional<listing_index> find_listing(std::string_view symbol_source, venue_index venue,
                                            std::string_view symbol) const {
    const std::optional<listing_index> in_history =
        loaded_.find_listing(symbol_source, venue, symbol, day_);
    if (!in_history) return std::nullopt;
    return listings_.find(loaded_.listings()[*in_history].id);
  }

 private:
  std::chrono::sys_days day_;
  history loaded_;
  nlib::id_table<listing_index, listing_on_day> listings_;
};

}  // namespace nqdb_test
