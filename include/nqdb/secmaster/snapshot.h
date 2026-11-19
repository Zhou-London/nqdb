#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <nlib/id_table.h>
#include <nqdb/secmaster/records.h>

namespace nqdb::secmaster {

using nlib::id_table;

inline constexpr std::string_view venue_symbol_source = "VENUE";

struct listing_symbol {
  std::string symbol;
  listing_index listing;

  friend bool operator==(const listing_symbol&, const listing_symbol&) = default;
};

class snapshot {
 public:
  using venue_table = id_table<venue_index, venue>;
  using instrument_table = id_table<instrument_index, instrument>;
  using tick_rule_table = id_table<tick_rule_index, tick_rule>;
  using listing_table = id_table<listing_index, listing>;
  using symbol_key = std::tuple<std::string, venue_index, std::string>;
  using symbol_map = std::map<symbol_key, listing_index, std::less<>>;

  snapshot(std::chrono::sys_days as_of, venue_table venues, instrument_table instruments,
           tick_rule_table tick_rules, listing_table listings, symbol_map listing_by_symbol)
      : as_of_(as_of),
        venues_(std::move(venues)),
        instruments_(std::move(instruments)),
        tick_rules_(std::move(tick_rules)),
        listings_(std::move(listings)),
        listing_by_symbol_(std::move(listing_by_symbol)) {}

  std::chrono::sys_days as_of() const noexcept { return as_of_; }
  const venue_table& venues() const noexcept { return venues_; }
  const instrument_table& instruments() const noexcept { return instruments_; }
  const tick_rule_table& tick_rules() const noexcept { return tick_rules_; }
  const listing_table& listings() const noexcept { return listings_; }

  std::optional<venue_index> find_venue(std::string_view code) const noexcept {
    const auto found = std::ranges::find(venues_, code, &venue::code);
    return found == venues_.end() ? std::nullopt : venues_.find(found->id);
  }

  std::optional<listing_index> find_listing(std::string_view symbol_source, venue_index venue,
                                            std::string_view symbol) const noexcept {
    const auto found = listing_by_symbol_.find(std::tuple{symbol_source, venue, symbol});
    return found == listing_by_symbol_.end() ? std::nullopt : std::optional{found->second};
  }

  std::vector<listing_symbol> symbols_from(std::string_view symbol_source) const {
    std::vector<listing_symbol> symbols;
    const auto first_of_source =
        listing_by_symbol_.lower_bound(std::tuple{symbol_source, venue_index{}, std::string_view{}});
    for (auto each = first_of_source;
         each != listing_by_symbol_.end() && std::get<0>(each->first) == symbol_source; ++each) {
      symbols.push_back({.symbol = std::get<2>(each->first), .listing = each->second});
    }
    return symbols;
  }

 private:
  std::chrono::sys_days as_of_;
  venue_table venues_;
  instrument_table instruments_;
  tick_rule_table tick_rules_;
  listing_table listings_;
  symbol_map listing_by_symbol_;
};

}  // namespace nqdb::secmaster
