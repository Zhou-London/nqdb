<p align="center"><img src="https://capsule-render.vercel.app/api?type=blur&height=240&color=gradient&text=NQ%20Database&fontSize=64&fontAlign=50&fontAlignY=45&desc=v0.1.0&descSize=22&descAlign=50&descAlignY=65" /></p>

<p align="center">
  <img alt="Version 0.1.0" src="https://img.shields.io/badge/version-0.1.0-blue" />
  <img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" />
  <img alt="CMake 4.3+" src="https://img.shields.io/badge/CMake-4.3%2B-064F8C?logo=cmake&logoColor=white" />
  <img alt="SQLite 3.38+" src="https://img.shields.io/badge/SQLite-3.38%2B-003B57?logo=sqlite&logoColor=white" />
  <img alt="Header-only library" src="https://img.shields.io/badge/library-header--only-orange" />
</p>

# NQ Database

Project of NowQuant.

NQ Database stores reference data for trading. The first module is the
security master: venues, instruments, listings, symbols, tick rules and
corporate actions, and trading calendars. A program reads it through the
header-only C++23 library `nq::db`. The `nqdb` command line program writes it.

## Model

- **Instrument** is the economic asset, for example one company's common
  stock. Each future and each option contract is its own instrument.
- **Listing** is one instrument on one venue in one currency.
- **Symbol** names a listing for one source: `VENUE` for the venue's own
  symbol, any other name for a data vendor's symbol.
- Instruments and listings have permanent ids. An id is never reused.
- Symbols, listing specs and identifiers are valid from one day until another.
  A rename ends the old symbol and starts the new one. The listing id stays.
- `assign` and `delist` record a change in the market. `amend`, `set`, `undelist`
  and `remove` correct a record that was entered wrong.
- A corporate action is stored as it was announced: the ex date, the share
  ratio, the cash per share. Adjusted prices are not stored. The adjustment
  factors are computed from the actions when they are read.
- The `change_log` table holds every write: the time, the table, and the row
  before and after as JSON. Triggers fill it, so each program that writes to
  the file is logged.
- Prices, quantities and multipliers are integers that count 1e-9 units.
- A venue's calendar is its weekly sessions plus the days that differ: closed
  days and days with special sessions. Session times are in the venue's local
  time. They count from midnight of the trading day, so `-06:00` is 18:00 on
  the day before and `26:00` is 02:00 on the day after.
- A venue's `timezone` is an IANA name such as `America/New_York`. A loaded
  calendar gives each session its opening and closing time in UTC. Summer time
  follows the time zone database.

The data lives in one SQLite file. A process loads a range of days into an
immutable `nq::db::reference` at start-up. SQL never runs after the load.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The build fetches nlib, cctz, CLI11 and GoogleTest. It needs SQLite 3.38 or
later and the IANA time zone database on the system.

## Command line

`nqdb` reads `--db`, then `NQDB_PATH`, then `~/.nqdb/secmaster.db`.

```bash
nqdb init
nqdb venue add --code XNAS --name "Nasdaq" --timezone America/New_York
nqdb tick-rule add --name US_EQUITY --band 0:0.0001 --band 1:0.01
nqdb instrument add --asset-class EQUITY --name "Example Corp"
nqdb listing add --instrument 1 --venue XNAS --currency USD --symbol EXMP \
                 --listed-on 2020-01-02 --lot-size 1 --tick-rule US_EQUITY
```

Each `add` prints the new id on standard output. Errors go to standard error
with a nonzero exit code, and the database stays as it was.

| Command | What it does |
|---|---|
| `init` | Creates an empty database. |
| `venue add`, `issuer add`, `tick-rule add` | Add one record and print its id. |
| `instrument add` | Adds an equity, ETF, bond or index and prints its id. |
| `instrument add-future`, `add-option`, `add-pair` | Add a contract or a currency pair with its spec. |
| `instrument assign-identifier` | Gives an instrument an ISIN, CUSIP or other identifier from a day on. |
| `instrument list`, `issuer list` | Print every instrument or issuer with its id. |
| `listing add` | Lists an instrument on a venue with its symbol, lot size and tick rule. |
| `listing assign-symbol` | Gives a listing a new symbol from a day on. `--source` selects a vendor's symbol. |
| `listing assign-spec` | Gives a listing a new lot size and tick rule from a day on. |
| `listing delist` | Ends a listing with its symbols and spec. The symbol is free again. |
| `action add` | Adds a split, dividend, spin-off or other corporate action and prints its id. |
| `venue amend`, `issuer amend`, `instrument amend` | Correct the options given. The other fields keep their values. |
| `tick-rule amend` | Corrects the name. `--band` replaces every band. |
| `instrument amend-future`, `amend-option`, `amend-pair` | Replace the whole spec of a contract or a currency pair. |
| `listing amend` | Corrects the currency, the primary flag, the first day or the delisting day. Symbols and specs that start or end on the old day move to the new day. |
| `listing undelist` | Lists a delisted listing again. The symbols and spec that ended on the delisting day have no end again. |
| `listing set-symbols`, `listing set-specs`, `instrument set-identifiers` | Replace every period with the `--period` options given. Use them to correct a value or a day, or to add an earlier period. |
| `action amend`, `action remove` | Replace every field of a corporate action, or delete it. |
| `action list` | Prints the corporate actions, tab-separated. `--instrument` selects one instrument. |
| `action factors` | Prints the adjustment factors of an instrument for its splits and stock dividends. `--currency` and `--close` add its cash dividends. |
| `log` | Prints every change, tab-separated. `--table` selects one table. |
| `calendar set-week` | Gives a venue its weekly sessions from a day on. Weeks that start on or after that day are replaced. |
| `calendar close-day` | Closes a venue for whole days, for example holidays. |
| `calendar set-day` | Gives one day its own sessions, for example an early close. |
| `calendar reset-day` | Gives days their weekly sessions back after `close-day` or `set-day`. |
| `calendar rules` | Prints the weekly sessions, closed days and special sessions of a venue. |
| `calendar list` | Prints the sessions of each trading day in a range of days, in local time and in UTC. |
| `calendar at` | Prints the trading day of a UTC time. Fails if the venue is closed at that time. |
| `listing list` | Prints the listings that trade on one day, tab-separated. |
| `listing find` | Prints the listing id that a symbol names on one day. |
| `listing tick-size` | Prints the tick size of a symbol at a price on one day. |
| `listing history` | Prints the lifetime, symbols and specs of the listings over a range of days, tab-separated. |
| `tick-rule list` | Prints every band of every tick rule. |
| `check` | Loads one day and fails if a listing lacks a symbol or a spec. |
| `archive` | Writes a copy named `secmaster-YYYY-MM-DD.db` into `archive/` beside the database. |

`nqdb <command> --help` lists the options of a command.

Correct the symbols of a listing by giving every period again. `nqdb listing history`
prints the periods it has. An empty `UNTIL` has no end:

```bash
nqdb listing set-symbols --listing 1 \
     --period 2020-01-02:2023-03-01:EXMP --period 2023-03-01::EXMQ
nqdb listing set-specs --listing 1 --period 2020-01-02::1::US_EQUITY
```

Enter a calendar as the regular week, then the days that differ:

```bash
nqdb calendar set-week --venue XNAS --from 2000-01-03 --hours MON-FRI:09:30-16:00
nqdb calendar close-day --venue XNAS --date 2025-11-27 --date 2025-12-25
nqdb calendar set-day --venue XNAS --date 2025-11-28 --session 09:30-13:00
nqdb calendar list --venue XNAS --from 2025-11-24 --until 2025-12-01
nqdb calendar at --venue XNAS --time 2025-11-28T15:00:00Z
```

Repeat `--hours` and `--session` for a venue with a lunch break. A session
that opens on the day before starts with a minus sign: `--session -06:00-17:00`.

Print the adjustment factors of an instrument. A cash dividend needs the last
close before its ex date, because the database holds no prices:

```bash
nqdb action factors --instrument 1
nqdb action factors --instrument 1 --currency USD --close 2023-06-01:187.5
nqdb action factors --instrument 1 --as-of 2024-01-31
```

A correction that breaks a rule fails, and the database stays as it was. The
rules are the ones `assign` follows: periods of one listing do not overlap, a
symbol names one listing per venue at a time, and each period lies inside the
lifetime of its listing. `amend` cannot clear an optional field such as `--mic`.

## Library

```cmake
FetchContent_Declare(nqdb GIT_REPOSITORY <url> GIT_TAG <tag>)
FetchContent_MakeAvailable(nqdb)
target_link_libraries(my_target PRIVATE nq::db)
```

`nq::db::reference` answers questions about listings, markets and corporate
actions. It hides the tables, the validity periods and the tick rules. Load one
at start-up for the days the program works on:

```cpp
#include <nq/db.h>

const nq::db::reference ref =
    nq::db::open(nq::db::default_path(), {.from = first_day, .until = day_after_last});

const nq::db::listing exmp = *ref.find("XNAS", "EXMP", some_day);
```

- `until` is the first day after the range.
- `default_path()` is `NQDB_PATH`, or `~/.nqdb/secmaster.db` without it. `nqdb`
  uses the same default.
- A third argument selects the listings to load:
  `[](const nq::db::venue& venue, const nq::db::instrument&) { return venue.code == "XNAS"; }`.
- `open` fails if a listing lacks a symbol or a spec on a listed day of the
  range, or if two sessions of a venue overlap.
- A question about a listing or a market on a day outside the range has no
  answer: it returns `std::nullopt`, `false` or an empty span. Adjustment
  factors answer for every day.

### Listings

A `listing` is a handle. It is valid in the `reference` that gave it.

```cpp
ref.find("XNAS", "EXMP", some_day);              // by the venue's symbol of that day
ref.find("XNAS", "EXMP.O", day, "VENDOR");      // by a data vendor's symbol
ref.find(nq::db::listing_id{1});                 // by the permanent id

ref.id(exmp);                    // permanent id, the same in every reference
exmp.number();                   // dense number from 0, an index for your own arrays
ref.listing_at(number);          // the listing with that number
ref.instrument_of(exmp);         // name, asset class, contract spec
ref.venue_of(exmp);              // code, MIC, name, country, time zone
ref.currency(exmp);
ref.is_primary(exmp);

ref.is_listed(exmp, some_day);
ref.symbol(exmp, some_day);
ref.symbol(exmp, some_day, "VENDOR");
ref.lot_size(exmp, some_day);
ref.min_qty(exmp, some_day);
ref.tick_size(exmp, some_day, price);
ref.tick_bands(exmp, some_day);  // every band, to keep beside a hot loop
```

- A symbol may name another listing on another day. Find the listing with the
  day of the data you read.
- The tick size depends on the price. `tick_bands` gives each price from which
  another tick size applies.
- Prices and quantities are `price_nanos` and `quantity_nanos`: integers that
  count 1e-9 units.
- `ref.change_days()` lists the days after the first on which a listing starts,
  ends, or takes another symbol or spec. Every answer about a listing stays the
  same from one change day until the next.

### Markets

A `market` is the handle of a venue. It carries the trading calendar.

```cpp
const nq::db::market nasdaq = *ref.find_market("XNAS");  // or ref.market_of(exmp)

ref.is_trading_day(nasdaq, some_day);
ref.next_trading_day(nasdaq, some_day);
ref.previous_trading_day(nasdaq, some_day);
ref.trading_days(nasdaq);              // every trading day of the range

ref.sessions(nasdaq, some_day);        // opening and closing times in UTC
ref.local_sessions(nasdaq, some_day);  // minutes after local midnight
ref.is_open(nasdaq, utc_time);
ref.trading_day(nasdaq, utc_time);     // the trading day of a bar or a tick
```

- A session contains its opening time and ends before its closing time.
- A session that opens on the evening before belongs to its own trading day.
- A venue without sessions has no trading day.

### Adjustment factors

```cpp
const double adjusted_close = raw_close * ref.price_factor(exmp, bar_day);
const double adjusted_volume = raw_volume * ref.volume_factor(exmp, bar_day);
const double as_seen_then = raw_close * ref.price_factor(exmp, bar_day, decision_day);

const nq::db::adjustments with_dividends = ref.adjustments_with_dividends(
    exmp, [&](std::chrono::sys_days ex_date) { return last_close_before(ex_date); });
const double total_return_close = raw_close * with_dividends.price_factor_on(bar_day);

ref.corporate_actions(exmp);  // every action of the instrument, by ex date
```

- A factor adjusts a day for every action with a later ex date. The factor of a
  day on or after the last ex date is 1.
- `price_factor` and `volume_factor` adjust for splits and stock dividends. A
  split of `new_shares` for `old_shares` multiplies earlier prices by
  `old_shares / new_shares` and earlier volumes by the inverse.
- `price_factor(listing, day, as_of)` applies only the ex dates until `as_of`.
  Use it in a backtest, so that a decision on one day sees no later action.
- A cash dividend multiplies earlier prices by `(close - cash) / close`. `close`
  is the last close before the ex date, in the currency of the listing. The
  caller supplies it, because the database holds no prices.
- Spin-offs, mergers, rights issues and delistings give no factor.

### Inside the library

`nq::db::secmaster` and `nq::db::sqlite` hold the parts that `reference` and
`nqdb` are built from. A program that only reads needs none of them.

| Header | Content |
|---|---|
| `nq/db.h` | `reference`, `listing`, `market`, `open()`, `default_path()`. |
| `nq/db/sqlite.h` | `database`, `statement`, `transaction` over the SQLite C API. |
| `nq/db/secmaster/schema.h` | `create_schema()`, `open()`, `open_read_only()`, `archive()`. `open` checks the schema version. |
| `nq/db/secmaster/records.h` | The ids, indexes, enums and record structs. |
| `nq/db/secmaster/loader.h` | `load_history()`, `load_calendar()` and the `read_*` functions. |
| `nq/db/secmaster/history.h` | `history`: the listings of a range of days, one period for each run of days with the same symbol or spec. |
| `nq/db/secmaster/calendar.h` | `calendar`: the trading days and sessions of one venue. |
| `nq/db/secmaster/adjustment.h` | `adjustments`, `split_adjustments()`, `split_and_dividend_adjustments()`. |
| `nq/db/secmaster/writer.h` | The writes behind each `nqdb` command. |
| `nq/db/secmaster/time_zone.h` | `zone_named()`, `utc_instant()` over cctz. |
| `nq/db/secmaster/text.h` | Each text form with its parser: `iso_date`/`parse_iso_date`, `clock_time`/`parse_clock_time`, `iso_instant`/`parse_iso_instant`, `decimal`/`parse_decimal`. |
