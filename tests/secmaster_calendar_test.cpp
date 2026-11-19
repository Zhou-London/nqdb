#include <nq/db/secmaster/calendar.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <nq/db/secmaster/text.h>
#include <nq/db/secmaster/loader.h>
#include <nq/db/secmaster/writer.h>

#include "secmaster_test_database.h"

namespace {

using namespace std::chrono_literals;
using namespace nq::db::secmaster;
using nq::db::sqlite::database;
using std::chrono::sys_days;

constexpr venue_id test_exchange{1};
constexpr venue_id alternative{2};

const time_zone utc = zone_named("Etc/UTC");
const time_zone new_york = zone_named("America/New_York");

constexpr sys_days monday{2024y / 1 / 1};
constexpr sys_days tuesday{2024y / 1 / 2};
constexpr sys_days wednesday{2024y / 1 / 3};
constexpr sys_days friday{2024y / 1 / 5};
constexpr sys_days saturday{2024y / 1 / 6};
constexpr sys_days sunday{2024y / 1 / 7};
constexpr sys_days next_monday{2024y / 1 / 8};
constexpr sys_days next_tuesday{2024y / 1 / 9};
constexpr day_range two_weeks{.from = monday, .until = sys_days{2024y / 1 / 15}};

constexpr session_hours full_day{.opens = 9h + 30min, .closes = 16h};
constexpr session_hours half_day{.opens = 9h + 30min, .closes = 13h};
constexpr session_hours morning{.opens = 9h, .closes = 11h + 30min};
constexpr session_hours afternoon{.opens = 13h, .closes = 15h};

std::vector<weekly_session> weekdays_with(std::initializer_list<session_hours> sessions) {
  std::vector<weekly_session> week;
  for (const std::chrono::weekday weekday : {std::chrono::Monday, std::chrono::Tuesday,
                                             std::chrono::Wednesday, std::chrono::Thursday,
                                             std::chrono::Friday}) {
    for (const session_hours& hours : sessions) week.push_back({weekday, hours});
  }
  return week;
}

calendar_rules weekdays_from(sys_days valid_from, std::initializer_list<session_hours> sessions) {
  calendar_rules rules;
  for (const weekly_session& session : weekdays_with(sessions)) {
    rules.weekly_sessions.push_back({{.from = valid_from, .until = std::nullopt}, session});
  }
  return rules;
}

template <typename T>
std::vector<T> copy_of(std::span<const T> values) {
  return {values.begin(), values.end()};
}

TEST(SecmasterCalendar, ExpandsTheWeeklySessionsIntoTradingDays) {
  const calendar loaded(test_exchange, utc, two_weeks, weekdays_from(monday, {full_day}));

  EXPECT_EQ(loaded.venue(), test_exchange);
  EXPECT_EQ(loaded.days(), two_weeks);
  EXPECT_EQ(loaded.trading_days().size(), 10u);
  EXPECT_EQ(loaded.trading_days().front(), monday);
  EXPECT_EQ(loaded.trading_days().back(), sys_days{2024y / 1 / 12});
  EXPECT_TRUE(loaded.is_trading_day(friday));
  EXPECT_FALSE(loaded.is_trading_day(saturday));
  EXPECT_FALSE(loaded.is_trading_day(sunday));
  EXPECT_EQ(copy_of(loaded.sessions_on(friday)), (std::vector{full_day}));
  EXPECT_TRUE(loaded.sessions_on(saturday).empty());
}

TEST(SecmasterCalendar, KnowsNothingOutsideItsDays) {
  const calendar loaded(test_exchange, utc, {.from = tuesday, .until = friday},
                        weekdays_from(monday, {full_day}));

  EXPECT_EQ(copy_of(loaded.trading_days()),
            (std::vector{tuesday, wednesday, sys_days{2024y / 1 / 4}}));
  EXPECT_FALSE(loaded.is_trading_day(monday));
  EXPECT_FALSE(loaded.is_trading_day(friday));
  EXPECT_TRUE(loaded.sessions_on(friday).empty());
}

TEST(SecmasterCalendar, AClosedDayIsNotATradingDay) {
  calendar_rules rules = weekdays_from(monday, {full_day});
  rules.closed_days = {next_monday, monday};
  const calendar loaded(test_exchange, utc, two_weeks, rules);

  EXPECT_EQ(loaded.trading_days().size(), 8u);
  EXPECT_FALSE(loaded.is_trading_day(monday));
  EXPECT_FALSE(loaded.is_trading_day(next_monday));
  EXPECT_TRUE(loaded.is_trading_day(tuesday));
}

TEST(SecmasterCalendar, SpecialSessionsReplaceTheWeeklySessionsOfTheirDay) {
  calendar_rules rules = weekdays_from(monday, {morning, afternoon});
  rules.special_sessions = {{.day = saturday, .hours = full_day},
                            {.day = wednesday, .hours = half_day}};
  const calendar loaded(test_exchange, utc, two_weeks, rules);

  EXPECT_EQ(copy_of(loaded.sessions_on(tuesday)), (std::vector{morning, afternoon}));
  EXPECT_EQ(copy_of(loaded.sessions_on(wednesday)), (std::vector{half_day}));
  EXPECT_EQ(copy_of(loaded.sessions_on(saturday)), (std::vector{full_day}));
  EXPECT_EQ(loaded.trading_days().size(), 11u);
}

TEST(SecmasterCalendar, WeeklySessionsApplyOnlyWhileTheyAreValid) {
  calendar_rules rules;
  for (const weekly_session& session : weekdays_with({full_day})) {
    rules.weekly_sessions.push_back({{.from = tuesday, .until = next_monday}, session});
  }
  for (const weekly_session& session : weekdays_with({half_day})) {
    rules.weekly_sessions.push_back({{.from = next_monday, .until = std::nullopt}, session});
  }
  const calendar loaded(test_exchange, utc, two_weeks, rules);

  EXPECT_FALSE(loaded.is_trading_day(monday));
  EXPECT_EQ(copy_of(loaded.sessions_on(tuesday)), (std::vector{full_day}));
  EXPECT_EQ(copy_of(loaded.sessions_on(friday)), (std::vector{full_day}));
  EXPECT_EQ(copy_of(loaded.sessions_on(next_monday)), (std::vector{half_day}));
}

TEST(SecmasterCalendar, IndexesCountTradingDaysFromZero) {
  calendar_rules rules = weekdays_from(monday, {full_day});
  rules.closed_days = {next_monday};
  const calendar loaded(test_exchange, utc, two_weeks, rules);

  EXPECT_EQ(loaded.index_of(monday), trading_day_index{0});
  EXPECT_EQ(loaded.index_of(friday), trading_day_index{4});
  EXPECT_EQ(loaded.index_of(next_tuesday), trading_day_index{5});
  EXPECT_EQ(loaded.index_of(saturday), std::nullopt);
  EXPECT_EQ(loaded.index_of(next_monday), std::nullopt);
  EXPECT_EQ(loaded.day_at(trading_day_index{5}), next_tuesday);
  EXPECT_EQ(copy_of(loaded.sessions_of(trading_day_index{5})), (std::vector{full_day}));
}

TEST(SecmasterCalendar, FindsTheNextAndThePreviousTradingDay) {
  calendar_rules rules = weekdays_from(monday, {full_day});
  rules.closed_days = {next_monday};
  const calendar loaded(test_exchange, utc, two_weeks, rules);

  EXPECT_EQ(loaded.next_trading_day(monday), tuesday);
  EXPECT_EQ(loaded.next_trading_day(friday), next_tuesday);
  EXPECT_EQ(loaded.next_trading_day(sunday), next_tuesday);
  EXPECT_EQ(loaded.next_trading_day(sys_days{2024y / 1 / 12}), std::nullopt);
  EXPECT_EQ(loaded.next_trading_day(sys_days{2023y / 12 / 1}), monday);

  EXPECT_EQ(loaded.previous_trading_day(tuesday), monday);
  EXPECT_EQ(loaded.previous_trading_day(next_tuesday), friday);
  EXPECT_EQ(loaded.previous_trading_day(next_monday), friday);
  EXPECT_EQ(loaded.previous_trading_day(monday), std::nullopt);
  EXPECT_EQ(loaded.previous_trading_day(sys_days{2024y / 2 / 1}), sys_days{2024y / 1 / 12});
}

TEST(SecmasterCalendar, SessionsOfADayAreInOpeningOrder) {
  const calendar loaded(test_exchange, utc, two_weeks, weekdays_from(monday, {afternoon, morning}));
  EXPECT_EQ(copy_of(loaded.sessions_on(monday)), (std::vector{morning, afternoon}));
}

TEST(SecmasterCalendar, ASessionMayOpenBeforeMidnightAndCloseAfterIt) {
  const session_hours overnight{.opens = -6h, .closes = 17h};
  const session_hours late{.opens = 20h, .closes = 26h};
  const calendar opens_early(test_exchange, utc, two_weeks, weekdays_from(monday, {overnight}));
  const calendar closes_late(test_exchange, utc, two_weeks, weekdays_from(monday, {late}));

  EXPECT_EQ(copy_of(opens_early.sessions_on(monday)), (std::vector{overnight}));
  EXPECT_EQ(opens_early.instants_on(monday).front().opens, sys_days{2023y / 12 / 31} + 18h);
  EXPECT_EQ(copy_of(closes_late.sessions_on(monday)), (std::vector{late}));
  EXPECT_EQ(closes_late.instants_on(monday).front().closes, tuesday + 2h);
}

TEST(SecmasterCalendar, RejectsSessionsThatOverlap) {
  const session_hours through_lunch{.opens = 11h, .closes = 14h};
  EXPECT_THROW(
      calendar(test_exchange, utc, two_weeks, weekdays_from(monday, {morning, through_lunch})),
      error);
  const session_hours right_after_the_morning{.opens = morning.closes, .closes = 13h};
  EXPECT_NO_THROW(calendar(test_exchange, utc, two_weeks,
                           weekdays_from(monday, {morning, right_after_the_morning})));
}

TEST(SecmasterCalendar, SessionsHaveUtcInstantsInTheTimeZoneOfTheVenue) {
  const sys_days friday_in_winter_time{2025y / 3 / 7};
  const sys_days monday_in_summer_time{2025y / 3 / 10};
  const calendar loaded(test_exchange, new_york,
                        {.from = friday_in_winter_time, .until = sys_days{2025y / 3 / 11}},
                        weekdays_from(monday, {full_day}));

  EXPECT_EQ(copy_of(loaded.instants_on(friday_in_winter_time)),
            (std::vector{session_instants{friday_in_winter_time + 14h + 30min,
                                          friday_in_winter_time + 21h}}));
  EXPECT_EQ(copy_of(loaded.instants_on(monday_in_summer_time)),
            (std::vector{session_instants{monday_in_summer_time + 13h + 30min,
                                          monday_in_summer_time + 20h}}));
  EXPECT_EQ(copy_of(loaded.instants_of(trading_day_index{1})),
            copy_of(loaded.instants_on(monday_in_summer_time)));
  EXPECT_TRUE(loaded.instants_on(sys_days{2025y / 3 / 8}).empty());
  EXPECT_EQ(loaded.zone().name(), "America/New_York");
}

TEST(SecmasterCalendar, UtcInstantsFollowSummerTimeInYearsFarAhead) {
  const sys_days summer_day{2045y / 7 / 10};
  const sys_days winter_day{2045y / 12 / 11};
  calendar_rules rules;
  rules.special_sessions = {{.day = summer_day, .hours = full_day},
                            {.day = winter_day, .hours = full_day}};
  const calendar loaded(test_exchange, new_york,
                        {.from = summer_day, .until = sys_days{2046y / 1 / 1}}, rules);

  EXPECT_EQ(loaded.instants_on(summer_day).front().opens, summer_day + 13h + 30min);
  EXPECT_EQ(loaded.instants_on(winter_day).front().opens, winter_day + 14h + 30min);
}

TEST(SecmasterCalendar, FindsTheTradingDayOfAnInstant) {
  calendar_rules rules = weekdays_from(monday, {morning, afternoon});
  rules.closed_days = {tuesday};
  const calendar loaded(test_exchange, new_york, two_weeks, rules);
  const auto at = [&](sys_days utc_day, std::chrono::minutes after_utc_midnight) {
    return loaded.trading_day_at(utc_day + after_utc_midnight);
  };

  EXPECT_EQ(at(monday, 14h), trading_day_index{0});
  EXPECT_EQ(at(monday, 16h + 29min), trading_day_index{0});
  EXPECT_EQ(at(monday, 18h), trading_day_index{0});
  EXPECT_EQ(at(wednesday, 14h), trading_day_index{1});
  EXPECT_EQ(at(friday, 19h + 59min), trading_day_index{3});
  EXPECT_EQ(at(next_monday, 14h), trading_day_index{4});

  EXPECT_EQ(at(monday, 13h + 59min), std::nullopt);
  EXPECT_EQ(at(monday, 16h + 30min), std::nullopt);
  EXPECT_EQ(at(monday, 17h + 59min), std::nullopt);
  EXPECT_EQ(at(monday, 20h), std::nullopt);
  EXPECT_EQ(at(tuesday, 14h), std::nullopt);
  EXPECT_EQ(at(saturday, 14h), std::nullopt);
  EXPECT_EQ(at(sys_days{2023y / 12 / 29}, 14h), std::nullopt);
  EXPECT_EQ(at(sys_days{2024y / 1 / 15}, 14h), std::nullopt);

  EXPECT_TRUE(loaded.is_open_at(monday + 14h));
  EXPECT_FALSE(loaded.is_open_at(monday + 17h));
}

TEST(SecmasterCalendar, ASessionThatOpensTheEveningBeforeBelongsToItsTradingDay) {
  const session_hours overnight{.opens = -6h, .closes = 17h};
  const calendar loaded(test_exchange, new_york, two_weeks, weekdays_from(monday, {overnight}));

  EXPECT_EQ(loaded.instants_on(monday).front().opens, sys_days{2023y / 12 / 31} + 23h);
  EXPECT_EQ(loaded.trading_day_at(sys_days{2023y / 12 / 31} + 23h + 30min), trading_day_index{0});
  EXPECT_EQ(loaded.trading_day_at(monday + 21h + 59min), trading_day_index{0});
  EXPECT_EQ(loaded.trading_day_at(monday + 22h + 30min), std::nullopt);
  EXPECT_EQ(loaded.trading_day_at(monday + 23h), trading_day_index{1});
  EXPECT_EQ(loaded.day_at(trading_day_index{1}), tuesday);
}

TEST(SecmasterCalendar, RejectsASessionThatOpensBeforeTheTradingDayBeforeItCloses) {
  const session_hours late{.opens = 20h, .closes = 26h};
  const session_hours early{.opens = 1h, .closes = 3h};
  const session_hours after_the_late_one{.opens = 2h, .closes = 3h};
  EXPECT_THROW(calendar(test_exchange, utc, two_weeks, weekdays_from(monday, {early, late})),
               error);
  EXPECT_NO_THROW(calendar(test_exchange, utc, two_weeks,
                           weekdays_from(monday, {after_the_late_one, late})));
}

TEST(SecmasterCalendar, ZoneNamedRejectsANameOutsideTheTimeZoneDatabase) {
  EXPECT_EQ(zone_named("Asia/Tokyo").name(), "Asia/Tokyo");
  EXPECT_THROW(zone_named("Mars/Olympus"), error);
  EXPECT_THROW(zone_named(""), error);
  EXPECT_EQ(iso_instant(utc_instant(zone_named("Asia/Tokyo"), monday, 8h)),
            "2023-12-31T23:00:00Z");
  EXPECT_EQ(iso_instant(utc_instant(new_york, sys_days{2025y / 3 / 9}, 2h + 30min)),
            "2025-03-09T07:00:00Z");
}

TEST(SecmasterCalendar, ClockTimeReadsAndWritesMinutesAfterMidnight) {
  EXPECT_EQ(parse_clock_time("09:30"), 9h + 30min);
  EXPECT_EQ(parse_clock_time("00:00"), 0min);
  EXPECT_EQ(parse_clock_time("26:00"), 26h);
  EXPECT_EQ(parse_clock_time("-06:00"), -6h);
  EXPECT_EQ(clock_time(9h + 30min), "09:30");
  EXPECT_EQ(clock_time(0min), "00:00");
  EXPECT_EQ(clock_time(26h), "26:00");
  EXPECT_EQ(clock_time(-(6h + 15min)), "-06:15");

  for (const char* malformed : {"", "9:30", "0930", "09:60", "09-30", "ab:cd", "09:3", "--6:00",
                                "09:30:00", " 9:30"}) {
    EXPECT_THROW(parse_clock_time(malformed), error) << malformed;
  }
}

std::vector<session_hours> sessions_on(database& secmaster, venue_id venue, sys_days day) {
  return copy_of(load_calendar(secmaster, venue, two_weeks).sessions_on(day));
}

TEST(SecmasterCalendar, SetWeeklySessionsAppliesFromTheDayGiven) {
  database secmaster = nqdb_test::secmaster_with_venues();
  set_week(secmaster, test_exchange, sys_days{2020y / 1 / 1}, weekdays_with({full_day}));
  set_week(secmaster, test_exchange, next_monday, weekdays_with({morning, afternoon}));

  EXPECT_EQ(sessions_on(secmaster, test_exchange, friday), (std::vector{full_day}));
  EXPECT_EQ(sessions_on(secmaster, test_exchange, next_monday), (std::vector{morning, afternoon}));
  EXPECT_TRUE(sessions_on(secmaster, test_exchange, saturday).empty());

  const calendar_rules rules = read_calendar_rules(secmaster, test_exchange);
  ASSERT_EQ(rules.weekly_sessions.size(), 15u);
  EXPECT_EQ(rules.weekly_sessions.front(),
            (weekly_session_period{{.from = sys_days{2020y / 1 / 1}, .until = next_monday},
                                   {std::chrono::Monday, full_day}}));
  EXPECT_EQ(rules.weekly_sessions.back(),
            (weekly_session_period{{.from = next_monday, .until = std::nullopt},
                                   {std::chrono::Friday, afternoon}}));
}

TEST(SecmasterCalendar, SetWeeklySessionsReplacesTheWeeksThatStartOnOrAfterTheDayGiven) {
  database secmaster = nqdb_test::secmaster_with_venues();
  set_week(secmaster, test_exchange, sys_days{2020y / 1 / 1}, weekdays_with({full_day}));
  set_week(secmaster, test_exchange, next_monday, weekdays_with({morning, afternoon}));

  set_week(secmaster, test_exchange, next_monday, weekdays_with({half_day}));
  EXPECT_EQ(sessions_on(secmaster, test_exchange, friday), (std::vector{full_day}));
  EXPECT_EQ(sessions_on(secmaster, test_exchange, next_monday), (std::vector{half_day}));

  set_week(secmaster, test_exchange, wednesday, weekdays_with({morning}));
  EXPECT_EQ(sessions_on(secmaster, test_exchange, tuesday), (std::vector{full_day}));
  EXPECT_EQ(sessions_on(secmaster, test_exchange, wednesday), (std::vector{morning}));
  EXPECT_EQ(sessions_on(secmaster, test_exchange, next_monday), (std::vector{morning}));

  set_week(secmaster, test_exchange, sys_days{2019y / 1 / 1}, {});
  EXPECT_TRUE(read_calendar_rules(secmaster, test_exchange).weekly_sessions.empty());
  EXPECT_TRUE(load_calendar(secmaster, test_exchange, two_weeks).trading_days().empty());
}

TEST(SecmasterCalendar, CloseDayAndSetSpecialSessionsOverrideTheWeekUntilRestored) {
  database secmaster = nqdb_test::secmaster_with_venues();
  set_week(secmaster, test_exchange, sys_days{2020y / 1 / 1}, weekdays_with({full_day}));

  close_day(secmaster, test_exchange, monday);
  close_day(secmaster, test_exchange, monday);
  set_day(secmaster, test_exchange, tuesday, std::array{half_day});
  set_day(secmaster, test_exchange, saturday, std::array{afternoon, morning});
  EXPECT_TRUE(sessions_on(secmaster, test_exchange, monday).empty());
  EXPECT_EQ(sessions_on(secmaster, test_exchange, tuesday), (std::vector{half_day}));
  EXPECT_EQ(sessions_on(secmaster, test_exchange, saturday), (std::vector{morning, afternoon}));

  set_day(secmaster, test_exchange, monday, std::array{half_day});
  close_day(secmaster, test_exchange, tuesday);
  EXPECT_EQ(sessions_on(secmaster, test_exchange, monday), (std::vector{half_day}));
  EXPECT_TRUE(sessions_on(secmaster, test_exchange, tuesday).empty());

  for (const sys_days day : {monday, tuesday, saturday, wednesday}) {
    reset_day(secmaster, test_exchange, day);
  }
  EXPECT_EQ(sessions_on(secmaster, test_exchange, monday), (std::vector{full_day}));
  EXPECT_EQ(sessions_on(secmaster, test_exchange, tuesday), (std::vector{full_day}));
  EXPECT_TRUE(sessions_on(secmaster, test_exchange, saturday).empty());
  const calendar_rules rules = read_calendar_rules(secmaster, test_exchange);
  EXPECT_TRUE(rules.closed_days.empty());
  EXPECT_TRUE(rules.special_sessions.empty());
}

TEST(SecmasterCalendar, EachVenueHasItsOwnCalendar) {
  database secmaster = nqdb_test::secmaster_with_venues();
  set_week(secmaster, test_exchange, monday, weekdays_with({full_day}));
  set_week(secmaster, alternative, monday, weekdays_with({morning, afternoon}));
  close_day(secmaster, alternative, tuesday);

  EXPECT_EQ(sessions_on(secmaster, test_exchange, tuesday), (std::vector{full_day}));
  EXPECT_TRUE(sessions_on(secmaster, alternative, tuesday).empty());
  EXPECT_EQ(sessions_on(secmaster, alternative, wednesday), (std::vector{morning, afternoon}));
  EXPECT_EQ(load_calendar(secmaster, alternative, two_weeks).venue(), alternative);
}

TEST(SecmasterCalendar, ARejectedWriteLeavesTheCalendarAsItWas) {
  database secmaster = nqdb_test::secmaster_with_venues();
  set_week(secmaster, test_exchange, monday, weekdays_with({full_day}));
  const session_hours through_lunch{.opens = 11h, .closes = 14h};
  const session_hours backwards{.opens = 16h, .closes = 9h};
  const venue_id unknown{99};

  EXPECT_THROW(set_week(secmaster, test_exchange, tuesday,
                                   weekdays_with({morning, through_lunch})),
               error);
  EXPECT_THROW(set_week(secmaster, test_exchange, tuesday, weekdays_with({backwards})),
               std::runtime_error);
  EXPECT_THROW(set_day(secmaster, test_exchange, tuesday,
                                    std::array{morning, through_lunch}),
               error);
  EXPECT_THROW(set_day(secmaster, test_exchange, tuesday, {}), error);
  EXPECT_THROW(set_week(secmaster, test_exchange, tuesday,
                                   weekdays_with({{.opens = 1h, .closes = 3h},
                                                  {.opens = 20h, .closes = 26h}})),
               error);
  EXPECT_THROW(set_day(secmaster, test_exchange, tuesday,
                                    std::array{session_hours{.opens = 20h, .closes = 36h}}),
               error);
  EXPECT_THROW(set_day(secmaster, test_exchange, tuesday,
                                    std::array{session_hours{.opens = -12h, .closes = 16h}}),
               error);
  EXPECT_THROW(set_week(secmaster, unknown, monday, weekdays_with({full_day})), error);
  EXPECT_THROW(close_day(secmaster, unknown, monday), error);
  EXPECT_THROW(set_day(secmaster, unknown, monday, std::array{half_day}), error);
  EXPECT_THROW(reset_day(secmaster, unknown, monday), error);

  EXPECT_EQ(load_calendar(secmaster, test_exchange, two_weeks).trading_days().size(), 10u);
  EXPECT_EQ(sessions_on(secmaster, test_exchange, tuesday), (std::vector{full_day}));
  EXPECT_EQ(read_calendar_rules(secmaster, test_exchange).weekly_sessions.size(), 5u);
}

TEST(SecmasterCalendar, LoadCalendarRejectsAnUnknownVenueAndARangeWithoutADay) {
  database secmaster = nqdb_test::secmaster_with_venues();
  EXPECT_THROW(load_calendar(secmaster, venue_id{99}, two_weeks), error);
  EXPECT_THROW(load_calendar(secmaster, test_exchange, {.from = monday, .until = monday}), error);
  EXPECT_TRUE(load_calendar(secmaster, test_exchange, two_weeks).trading_days().empty());
}

TEST(SecmasterCalendar, LoadCalendarUsesTheTimeZoneOfTheVenue) {
  database secmaster = nqdb_test::secmaster_with_venues();
  set_week(secmaster, test_exchange, monday, weekdays_with({full_day}));
  EXPECT_EQ(load_calendar(secmaster, test_exchange, two_weeks).instants_on(monday).front().opens,
            monday + 9h + 30min);

  amend_venue(secmaster, test_exchange, {.code = {}, .mic = {}, .name = {}, .country = {},
                                         .timezone = "America/New_York"});
  const calendar loaded = load_calendar(secmaster, test_exchange, two_weeks);
  EXPECT_EQ(loaded.instants_on(monday).front().opens, monday + 14h + 30min);
  EXPECT_EQ(loaded.trading_day_at(monday + 15h), trading_day_index{0});

  secmaster.run_script("UPDATE venue SET timezone = 'Mars/Olympus' WHERE venue_id = 1");
  EXPECT_THROW(load_calendar(secmaster, test_exchange, two_weeks), error);
}

TEST(SecmasterCalendar, AVenueTakesOnlyATimeZoneOfTheTimeZoneDatabase) {
  database secmaster = nqdb_test::secmaster_with_venues();
  EXPECT_THROW(add_venue(secmaster, {.code = "XNEW", .mic = {}, .name = "New Exchange",
                                     .country = {}, .timezone = "Mars/Olympus"}),
               error);
  EXPECT_THROW(amend_venue(secmaster, test_exchange, {.code = {}, .mic = {}, .name = "Renamed",
                                                      .country = {}, .timezone = "New York"}),
               error);
  EXPECT_NO_THROW(add_venue(secmaster, {.code = "XNEW", .mic = {}, .name = "New Exchange",
                                        .country = {}, .timezone = "Asia/Tokyo"}));
  EXPECT_EQ(read_changes(secmaster, "venue").size(), 3u);
}

TEST(SecmasterCalendar, CalendarWritesAreLogged) {
  database secmaster = nqdb_test::secmaster_with_venues();
  set_week(secmaster, test_exchange, monday, weekdays_with({full_day}));
  close_day(secmaster, test_exchange, monday);
  set_day(secmaster, test_exchange, tuesday, std::array{half_day});

  EXPECT_EQ(read_changes(secmaster, "weekly_session").size(), 5u);
  EXPECT_EQ(read_changes(secmaster, "closed_day").size(), 1u);
  EXPECT_EQ(read_changes(secmaster, "special_session").size(), 1u);
}

}  // namespace
