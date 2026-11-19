# NQ Database

Project of NowQuant.

NQ Database stores reference data for trading. The first module is the
security master: venues, instruments, listings, symbols, tick rules and
corporate actions. It ships as a header-only C++23 library and as the `nqdb`
command line program. Both call the same functions.

## Model

- **Instrument** is the economic asset, for example one company's common
  stock. Each future and each option contract is its own instrument.
- **Listing** is one instrument on one venue in one currency.
- **Symbol** names a listing for one source: `VENUE` for the venue's own
  symbol, any other name for a data vendor's symbol.
- Instruments and listings have permanent ids. An id is never reused.
- Symbols, listing specs and identifiers are valid from one day until another.
  A rename ends the old symbol and starts the new one. The listing id stays.
- Prices, quantities and multipliers are integers that count 1e-9 units.

The data lives in one SQLite file. A process loads one day of it into an
immutable snapshot of dense arrays at start-up. SQL never runs after that.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The build fetches nlib, CLI11 and GoogleTest. It needs SQLite 3.37 or later on
the system.

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
| `listing delist` | Ends a listing with its symbols and spec. The symbol is free again. A listing is delisted once. |
| `action record` | Records a split, dividend, spin-off or other corporate action. |
| `list` | Prints the listings that trade on one day, tab-separated. |
| `find` | Prints the listing id that a symbol names on one day. |
| `check` | Loads one day and fails if a listing lacks a symbol or a spec. |
| `archive` | Writes a copy named `secmaster-YYYY-MM-DD.db` into `archive/` beside the database. |

`nqdb <command> --help` lists the options of a command.

## Library

```cmake
FetchContent_Declare(nqdb GIT_REPOSITORY <url> GIT_TAG <tag>)
FetchContent_MakeAvailable(nqdb)
target_link_libraries(my_target PRIVATE nqdb::nqdb)
```

Read one day at start-up:

```cpp
#include <nqdb/secmaster/loader.h>

using namespace nqdb::secmaster;

nqdb::sqlite::database database = open_read_only(file);
const snapshot today = load_snapshot(database, as_of, [](const venue& venue, const instrument&) {
  return venue.code == "XNAS";
});

const venue_index nasdaq = *today.find_venue("XNAS");
const listing_index listing = *today.find_listing("VENUE", nasdaq, "EXMP");
const tick_rule& ticks = today.tick_rules()[today.listings()[listing].tick_rule];
```

`listing_index` is dense: it counts from 0 in listing id order. Use it to index
your own arrays. It is valid in one snapshot only. A snapshot of another day or
another universe gives a listing another `listing_index`. The listing id is the
same in every snapshot: `today.listings()[listing].id`.

A consolidated feed gives a symbol without its venue. `symbols_from` lists the
symbols of one source with their listings:

```cpp
std::map<std::string, listing_index> listing_by_vendor_symbol;
for (listing_symbol& each : today.symbols_from("VENDOR")) {
  const bool is_first = listing_by_vendor_symbol.emplace(std::move(each.symbol), each.listing).second;
  if (!is_first) { /* the symbol names listings on two venues */ }
}
```

One source may give the same symbol to listings on two venues. `symbols_from`
returns one element for each listing, in venue order, then in symbol order.

Write with the functions the command line uses:

```cpp
#include <nqdb/secmaster/writer.h>

nqdb::sqlite::transaction all_or_nothing(database);
assign_symbol(database, listing, "VENUE", "EXMQ", rename_day);
delist(database, other_listing, last_day);
all_or_nothing.commit();
```

| Header | Content |
|---|---|
| `nqdb/sqlite.h` | `database`, `statement`, `transaction` over the SQLite C API. |
| `nqdb/secmaster/schema.h` | `create_schema()`, `open()`, `open_read_only()`, `archive()`. |
| `nqdb/secmaster/records.h` | The ids, indexes, enums and record structs. |
| `nqdb/secmaster/loader.h` | `load_snapshot()`, `read_instruments()`, `read_issuers()`. |
| `nqdb/secmaster/snapshot.h` | `snapshot`, `find_venue()`, `find_listing()`, `symbols_from()`. |
| `nqdb/secmaster/writer.h` | `add_*`, `assign_*`, `delist()`, `record_corporate_action()`. |
| `nqdb/secmaster/decimal.h` | Conversion between decimal text and 1e-9 integers. |
