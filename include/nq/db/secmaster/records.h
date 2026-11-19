#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlib/strong_int.h>

namespace nq::db::secmaster {

using nlib::strong_int;

using venue_id = strong_int<struct venue_id_tag, std::int64_t>;
using issuer_id = strong_int<struct issuer_id_tag, std::int64_t>;
using instrument_id = strong_int<struct instrument_id_tag, std::int64_t>;
using listing_id = strong_int<struct listing_id_tag, std::int64_t>;
using tick_rule_id = strong_int<struct tick_rule_id_tag, std::int64_t>;
using corporate_action_id = strong_int<struct corporate_action_id_tag, std::int64_t>;

using venue_index = strong_int<struct venue_index_tag, std::uint32_t>;
using instrument_index = strong_int<struct instrument_index_tag, std::uint32_t>;
using listing_index = strong_int<struct listing_index_tag, std::uint32_t>;
using tick_rule_index = strong_int<struct tick_rule_index_tag, std::uint32_t>;

using trading_day_index = strong_int<struct trading_day_index_tag, std::uint32_t>;

using price_nanos = strong_int<struct price_nanos_tag, std::int64_t>;
using quantity_nanos = strong_int<struct quantity_nanos_tag, std::int64_t>;
using multiplier_nanos = strong_int<struct multiplier_nanos_tag, std::int64_t>;
inline constexpr std::int64_t nanos_per_unit = 1'000'000'000;

inline constexpr std::string_view venue_symbol_source = "VENUE";

template <typename Enum>
using enum_name = std::pair<std::string_view, Enum>;

template <typename Enum, std::size_t Count>
constexpr Enum enum_named(std::string_view name,
                          const std::array<enum_name<Enum>, Count>& names) noexcept {
  return std::ranges::find(names, name, &enum_name<Enum>::first)->second;
}

template <typename Enum, std::size_t Count>
constexpr std::string_view name_of(Enum value,
                                   const std::array<enum_name<Enum>, Count>& names) noexcept {
  return std::ranges::find(names, value, &enum_name<Enum>::second)->first;
}

enum class asset_class : std::uint8_t { equity, etf, future, option, fx, crypto, bond, index };
inline constexpr auto asset_class_names = std::to_array<enum_name<asset_class>>({
    {"EQUITY", asset_class::equity},
    {"ETF", asset_class::etf},
    {"FUTURE", asset_class::future},
    {"OPTION", asset_class::option},
    {"FX", asset_class::fx},
    {"CRYPTO", asset_class::crypto},
    {"BOND", asset_class::bond},
    {"INDEX", asset_class::index},
});

enum class corporate_action_type : std::uint8_t {
  cash_dividend,
  stock_dividend,
  split,
  spinoff,
  merger,
  rights,
  delisting,
};
inline constexpr auto corporate_action_type_names =
    std::to_array<enum_name<corporate_action_type>>({
        {"CASH_DIVIDEND", corporate_action_type::cash_dividend},
        {"STOCK_DIVIDEND", corporate_action_type::stock_dividend},
        {"SPLIT", corporate_action_type::split},
        {"SPINOFF", corporate_action_type::spinoff},
        {"MERGER", corporate_action_type::merger},
        {"RIGHTS", corporate_action_type::rights},
        {"DELISTING", corporate_action_type::delisting},
    });

enum class settlement_method : std::uint8_t { cash, physical };
inline constexpr auto settlement_method_names = std::to_array<enum_name<settlement_method>>({
    {"CASH", settlement_method::cash},
    {"PHYSICAL", settlement_method::physical},
});

enum class option_right : std::uint8_t { put, call };
inline constexpr auto option_right_names = std::to_array<enum_name<option_right>>({
    {"PUT", option_right::put},
    {"CALL", option_right::call},
});

enum class exercise_style : std::uint8_t { american, european };
inline constexpr auto exercise_style_names = std::to_array<enum_name<exercise_style>>({
    {"AMERICAN", exercise_style::american},
    {"EUROPEAN", exercise_style::european},
});

struct venue {
  venue_id id;
  std::string code;
  std::optional<std::string> mic;
  std::string name;
  std::optional<std::string> country;
  std::string timezone;
};

struct issuer {
  issuer_id id;
  std::string name;
  std::optional<std::string> lei;
  std::optional<std::string> country;
};

struct future_spec {
  instrument_id underlying;
  std::string root;
  std::chrono::sys_days expiry;
  multiplier_nanos multiplier;
  settlement_method settlement;
};

struct option_spec {
  instrument_id underlying;
  std::chrono::sys_days expiry;
  price_nanos strike;
  option_right right;
  exercise_style style;
  multiplier_nanos multiplier;
};

struct pair_spec {
  std::string base_currency;
  std::string quote_currency;
};

struct no_spec {};

using instrument_spec = std::variant<no_spec, future_spec, option_spec, pair_spec>;

template <typename Spec>
constexpr bool describes(asset_class) noexcept = delete;
template <>
constexpr bool describes<future_spec>(asset_class asset_class) noexcept {
  return asset_class == asset_class::future;
}
template <>
constexpr bool describes<option_spec>(asset_class asset_class) noexcept {
  return asset_class == asset_class::option;
}
template <>
constexpr bool describes<pair_spec>(asset_class asset_class) noexcept {
  return asset_class == asset_class::fx || asset_class == asset_class::crypto;
}
template <>
constexpr bool describes<no_spec>(asset_class asset_class) noexcept {
  return !describes<future_spec>(asset_class) && !describes<option_spec>(asset_class) &&
         !describes<pair_spec>(asset_class);
}

struct instrument {
  instrument_id id;
  secmaster::asset_class asset_class;
  std::string name;
  instrument_spec spec;
};

struct tick_band {
  price_nanos price_from;
  price_nanos tick_size;
};

struct tick_rule {
  tick_rule_id id;
  std::string name;
  std::vector<tick_band> bands_by_ascending_price;

  price_nanos tick_size_at(price_nanos price) const noexcept {
    const auto first_band_above =
        std::ranges::upper_bound(bands_by_ascending_price, price, {}, &tick_band::price_from);
    const auto lowest_band = bands_by_ascending_price.begin();
    return first_band_above == lowest_band ? lowest_band->tick_size
                                           : std::prev(first_band_above)->tick_size;
  }
};

struct session_hours {
  std::chrono::minutes opens;
  std::chrono::minutes closes;

  friend bool operator==(const session_hours&, const session_hours&) = default;
};

struct session_instants {
  std::chrono::sys_seconds opens;
  std::chrono::sys_seconds closes;

  friend bool operator==(const session_instants&, const session_instants&) = default;
};

struct weekly_session {
  std::chrono::weekday weekday;
  session_hours hours;

  friend bool operator==(const weekly_session&, const weekly_session&) = default;
};

struct corporate_action {
  corporate_action_id id;
  instrument_id instrument;
  corporate_action_type type;
  std::chrono::sys_days ex_date;
  std::optional<std::chrono::sys_days> announce_date;
  std::optional<std::chrono::sys_days> record_date;
  std::optional<std::chrono::sys_days> pay_date;
  std::optional<price_nanos> cash_per_share;
  std::optional<std::string> currency;
  std::optional<std::int64_t> new_shares;
  std::optional<std::int64_t> old_shares;
  std::optional<instrument_id> related_instrument;
  std::string recorded_at;
};

struct change {
  std::int64_t id;
  std::string changed_at;
  std::string table;
  std::string operation;
  std::optional<std::string> old_row;
  std::optional<std::string> new_row;
};

}  // namespace nq::db::secmaster
