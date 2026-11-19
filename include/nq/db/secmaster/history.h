#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <nlib/id_table.h>
#include <nq/db/secmaster/records.h>

namespace nq::db::secmaster {

struct day_range {
  std::chrono::sys_days from;
  std::chrono::sys_days until;

  bool contains(std::chrono::sys_days day) const noexcept { return from <= day && day < until; }

  friend bool operator==(const day_range&, const day_range&) = default;
};

struct validity {
  std::chrono::sys_days from;
  std::optional<std::chrono::sys_days> until;

  bool covers(std::chrono::sys_days day) const noexcept {
    return from <= day && (!until || day < *until);
  }

  friend bool operator==(const validity&, const validity&) = default;
};

struct listing {
  listing_id id;
  instrument_index instrument;
  venue_index venue;
  std::string currency;
  bool is_primary;
  validity listed;
};

struct symbol_period {
  validity valid;
  std::string symbol;

  friend bool operator==(const symbol_period&, const symbol_period&) = default;
};

struct spec_period {
  validity valid;
  quantity_nanos lot_size;
  std::optional<quantity_nanos> min_qty;
  tick_rule_index tick_rule;

  friend bool operator==(const spec_period&, const spec_period&) = default;
};

template <typename Period>
class periods_by_listing {
 public:
  explicit periods_by_listing(std::size_t listing_count) : periods_of_(listing_count) {}

  void append_in_listing_then_day_order(listing_index listing, Period period) {
    slice& periods = periods_of_[listing.value()];
    if (periods.count == 0) periods.first = static_cast<std::uint32_t>(periods_.size());
    ++periods.count;
    periods_.push_back(std::move(period));
  }

  std::span<const Period> of(listing_index listing) const noexcept {
    const slice periods = periods_of_[listing.value()];
    return std::span{periods_}.subspan(periods.first, periods.count);
  }

  const Period* on(listing_index listing, std::chrono::sys_days day) const noexcept {
    const std::span<const Period> periods = of(listing);
    const auto first_after = std::ranges::upper_bound(
        periods, day, {}, [](const Period& period) { return period.valid.from; });
    if (first_after == periods.begin()) return nullptr;
    const Period& latest = *std::prev(first_after);
    return latest.valid.covers(day) ? &latest : nullptr;
  }

 private:
  struct slice {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
  };

  std::vector<slice> periods_of_;
  std::vector<Period> periods_;
};

class history {
 public:
  using venue_table = nlib::id_table<venue_index, venue>;
  using instrument_table = nlib::id_table<instrument_index, instrument>;
  using tick_rule_table = nlib::id_table<tick_rule_index, tick_rule>;
  using listing_table = nlib::id_table<listing_index, listing>;
  using symbol_periods = periods_by_listing<symbol_period>;
  using spec_periods = periods_by_listing<spec_period>;
  using symbol_periods_by_source = std::map<std::string, symbol_periods, std::less<>>;

  history(day_range days, venue_table venues, instrument_table instruments,
          tick_rule_table tick_rules, listing_table listings, symbol_periods_by_source symbols,
          spec_periods specs)
      : days_(days),
        venues_(std::move(venues)),
        instruments_(std::move(instruments)),
        tick_rules_(std::move(tick_rules)),
        listings_(std::move(listings)),
        symbols_(std::move(symbols)),
        specs_(std::move(specs)),
        symbol_uses_(uses_by_source_venue_symbol_then_day()),
        change_days_(days_after_the_first_with_a_change()) {}

  day_range days() const noexcept { return days_; }
  const venue_table& venues() const noexcept { return venues_; }
  const instrument_table& instruments() const noexcept { return instruments_; }
  const tick_rule_table& tick_rules() const noexcept { return tick_rules_; }
  const listing_table& listings() const noexcept { return listings_; }

  std::span<const std::chrono::sys_days> change_days() const noexcept { return change_days_; }

  std::vector<std::string_view> symbol_sources() const {
    std::vector<std::string_view> sources;
    for (const auto& [source, periods] : symbols_) sources.push_back(source);
    return sources;
  }

  std::optional<venue_index> find_venue(std::string_view code) const noexcept {
    const auto found = std::ranges::find(venues_, code, &venue::code);
    return found == venues_.end() ? std::nullopt : venues_.find(found->id);
  }

  bool is_listed(listing_index listing, std::chrono::sys_days day) const noexcept {
    return days_.contains(day) && listings_[listing].listed.covers(day);
  }

  std::span<const spec_period> specs_of(listing_index listing) const noexcept {
    return specs_.of(listing);
  }

  const spec_period* spec_on(listing_index listing, std::chrono::sys_days day) const noexcept {
    return days_.contains(day) ? specs_.on(listing, day) : nullptr;
  }

  std::span<const symbol_period> symbols_of(std::string_view symbol_source,
                                            listing_index listing) const noexcept {
    const auto from_source = symbols_.find(symbol_source);
    return from_source == symbols_.end() ? std::span<const symbol_period>{}
                                         : from_source->second.of(listing);
  }

  const symbol_period* symbol_on(std::string_view symbol_source, listing_index listing,
                                 std::chrono::sys_days day) const noexcept {
    const auto from_source = symbols_.find(symbol_source);
    const bool is_known = from_source != symbols_.end() && days_.contains(day);
    return is_known ? from_source->second.on(listing, day) : nullptr;
  }

  std::optional<listing_index> find_listing(std::string_view symbol_source, venue_index venue,
                                            std::string_view symbol,
                                            std::chrono::sys_days day) const noexcept {
    if (!days_.contains(day)) return std::nullopt;
    const auto first_after = std::ranges::upper_bound(
        symbol_uses_, std::tuple{symbol_source, venue, symbol, day}, {}, &symbol_use::sort_key);
    if (first_after == symbol_uses_.begin()) return std::nullopt;
    const symbol_use& latest = *std::prev(first_after);
    const bool names_a_listing = latest.source == symbol_source && latest.venue == venue &&
                                 latest.symbol == symbol && latest.valid.covers(day);
    return names_a_listing ? std::optional{latest.listing} : std::nullopt;
  }

 private:
  struct symbol_use {
    std::string_view source;
    venue_index venue;
    std::string_view symbol;
    validity valid;
    listing_index listing;

    std::tuple<std::string_view, venue_index, std::string_view, std::chrono::sys_days> sort_key()
        const noexcept {
      return {source, venue, symbol, valid.from};
    }
  };

  std::vector<symbol_use> uses_by_source_venue_symbol_then_day() const {
    std::vector<symbol_use> uses;
    for (const auto& [source, periods] : symbols_) {
      for (const listing& listing : listings_) {
        const listing_index index = *listings_.find(listing.id);
        for (const symbol_period& period : periods.of(index)) {
          uses.push_back({.source = source,
                          .venue = listing.venue,
                          .symbol = period.symbol,
                          .valid = period.valid,
                          .listing = index});
        }
      }
    }
    std::ranges::sort(uses, {}, &symbol_use::sort_key);
    return uses;
  }

  std::vector<std::chrono::sys_days> days_after_the_first_with_a_change() const {
    std::vector<std::chrono::sys_days> change_days;
    const auto add_ends_of = [&](const validity& valid) {
      const std::optional<std::chrono::sys_days> first_day{valid.from};
      for (const std::optional<std::chrono::sys_days>& end : {first_day, valid.until}) {
        if (end && days_.contains(*end) && *end != days_.from) change_days.push_back(*end);
      }
    };
    for (const listing& listing : listings_) {
      const listing_index index = *listings_.find(listing.id);
      add_ends_of(listing.listed);
      for (const spec_period& period : specs_.of(index)) add_ends_of(period.valid);
      for (const auto& [source, periods] : symbols_) {
        for (const symbol_period& period : periods.of(index)) add_ends_of(period.valid);
      }
    }
    std::ranges::sort(change_days);
    const auto repeated = std::ranges::unique(change_days);
    change_days.erase(repeated.begin(), repeated.end());
    return change_days;
  }

  day_range days_;
  venue_table venues_;
  instrument_table instruments_;
  tick_rule_table tick_rules_;
  listing_table listings_;
  symbol_periods_by_source symbols_;
  spec_periods specs_;
  std::vector<symbol_use> symbol_uses_;
  std::vector<std::chrono::sys_days> change_days_;
};

}  // namespace nq::db::secmaster
