#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nq/db/secmaster/error.h>
#include <nq/db/secmaster/records.h>

namespace nq::db::secmaster {

struct adjustment {
  std::chrono::sys_days ex_date;
  double price_factor;
  double volume_factor;

  friend bool operator==(const adjustment&, const adjustment&) = default;
};

class adjustments {
 public:
  adjustments() : adjustments(std::vector<adjustment>{}) {}

  explicit adjustments(std::vector<adjustment> steps) : steps_(std::move(steps)) {
    std::ranges::stable_sort(steps_, {}, &adjustment::ex_date);
    price_of_steps_before_.reserve(steps_.size() + 1);
    volume_of_steps_before_.reserve(steps_.size() + 1);
    price_of_steps_before_.push_back(1.0);
    volume_of_steps_before_.push_back(1.0);
    for (const adjustment& step : steps_) {
      price_of_steps_before_.push_back(price_of_steps_before_.back() * step.price_factor);
      volume_of_steps_before_.push_back(volume_of_steps_before_.back() * step.volume_factor);
    }
  }

  std::span<const adjustment> steps() const noexcept { return steps_; }

  double price_factor_on(std::chrono::sys_days day,
                         std::optional<std::chrono::sys_days> as_of = {}) const noexcept {
    return factor_of_steps_after(day, steps_until(as_of), price_of_steps_before_);
  }

  double volume_factor_on(std::chrono::sys_days day,
                          std::optional<std::chrono::sys_days> as_of = {}) const noexcept {
    return factor_of_steps_after(day, steps_until(as_of), volume_of_steps_before_);
  }

 private:
  std::size_t steps_until(std::optional<std::chrono::sys_days> last_ex_date) const noexcept {
    if (!last_ex_date) return steps_.size();
    const auto first_after =
        std::ranges::upper_bound(steps_, *last_ex_date, {}, &adjustment::ex_date);
    return static_cast<std::size_t>(first_after - steps_.begin());
  }

  double factor_of_steps_after(std::chrono::sys_days day, std::size_t steps_applied,
                               const std::vector<double>& product_of_steps_before) const noexcept {
    const std::size_t steps_in_the_price_of_the_day = steps_until(std::optional{day});
    if (steps_applied <= steps_in_the_price_of_the_day) return 1.0;
    return product_of_steps_before[steps_applied] /
           product_of_steps_before[steps_in_the_price_of_the_day];
  }

  std::vector<adjustment> steps_;
  std::vector<double> price_of_steps_before_;
  std::vector<double> volume_of_steps_before_;
};

inline bool changes_the_share_count(corporate_action_type type) noexcept {
  return type == corporate_action_type::split || type == corporate_action_type::stock_dividend;
}

inline adjustment share_count_adjustment(const corporate_action& action) {
  const double shares_after_per_share_before =
      static_cast<double>(*action.new_shares) / static_cast<double>(*action.old_shares);
  return {.ex_date = action.ex_date,
          .price_factor = 1.0 / shares_after_per_share_before,
          .volume_factor = shares_after_per_share_before};
}

inline adjustment cash_dividend_adjustment(const corporate_action& action,
                                           std::string_view price_currency,
                                           std::optional<price_nanos> close_before_ex_date) {
  if (action.currency != price_currency) {
    throw error(std::format("corporate action {} pays {}, the prices are in {}",
                            action.id.value(), action.currency.value_or("no currency"),
                            price_currency));
  }
  if (!close_before_ex_date) {
    throw error(std::format("corporate action {} needs the close before {}", action.id.value(),
                            action.ex_date));
  }
  if (*close_before_ex_date <= *action.cash_per_share) {
    throw error(std::format("corporate action {} pays more than the close before {}",
                            action.id.value(), action.ex_date));
  }
  const auto close = static_cast<double>(close_before_ex_date->value());
  const auto cash = static_cast<double>(action.cash_per_share->value());
  return {.ex_date = action.ex_date, .price_factor = (close - cash) / close, .volume_factor = 1.0};
}

inline adjustments split_adjustments(std::span<const corporate_action> actions_of_one_instrument) {
  std::vector<adjustment> steps;
  for (const corporate_action& action : actions_of_one_instrument) {
    if (changes_the_share_count(action.type)) steps.push_back(share_count_adjustment(action));
  }
  return adjustments(std::move(steps));
}

template <typename CloseBefore>
  requires std::is_invocable_r_v<std::optional<price_nanos>, CloseBefore, std::chrono::sys_days>
adjustments split_and_dividend_adjustments(
    std::span<const corporate_action> actions_of_one_instrument, std::string_view price_currency,
    CloseBefore close_before) {
  std::vector<adjustment> steps;
  for (const corporate_action& action : actions_of_one_instrument) {
    if (changes_the_share_count(action.type)) {
      steps.push_back(share_count_adjustment(action));
    } else if (action.type == corporate_action_type::cash_dividend) {
      steps.push_back(
          cash_dividend_adjustment(action, price_currency, close_before(action.ex_date)));
    }
  }
  return adjustments(std::move(steps));
}

}  // namespace nq::db::secmaster
