#pragma once

#include <cstdlib>
#include <exception>
#include <ostream>

#include <CLI/CLI.hpp>

#include "cli/action_commands.h"
#include "cli/arguments.h"
#include "cli/calendar_commands.h"
#include "cli/database_commands.h"
#include "cli/instrument_commands.h"
#include "cli/listing_commands.h"
#include "cli/venue_commands.h"

namespace nq::db::cli {

inline int run(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
  CLI::App app{"nqdb: the security master database"};
  app.require_subcommand(1);

  context context{.database_file = {}, .out = out};
  app.add_option("--db", context.database_file,
                 "Database file (default $NQDB_PATH, else ~/.nqdb/secmaster.db)");

  database_commands database_commands;
  venue_commands venue_commands;
  issuer_commands issuer_commands;
  tick_rule_commands tick_rule_commands;
  instrument_commands instrument_commands;
  listing_commands listing_commands;
  action_commands action_commands;
  calendar_commands calendar_commands;
  database_commands.add_to(app, context);
  venue_commands.add_to(app, context);
  issuer_commands.add_to(app, context);
  tick_rule_commands.add_to(app, context);
  instrument_commands.add_to(app, context);
  listing_commands.add_to(app, context);
  action_commands.add_to(app, context);
  calendar_commands.add_to(app, context);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& parse_error) {
    return app.exit(parse_error, out, err);
  } catch (const std::exception& failure) {
    err << "nqdb: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

}  // namespace nq::db::cli
