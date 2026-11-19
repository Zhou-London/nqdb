#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>

#include "cli/arguments.h"

namespace nq::db::cli {

struct database_commands {
  date_option archive_date;
  std::filesystem::path archive_directory;
  std::optional<std::string> logged_table;
  date_option check_date;
  universe_options check_universe;

  void add_to(CLI::App& app, const context& context) {
    app.add_subcommand("init", "Create an empty database")->callback([&context] {
      std::filesystem::create_directories(context.file().parent_path());
      sqlite::database created = sqlite::database::create(context.file());
      create_schema(created);
      context.out << context.file().string() << '\n';
    });

    CLI::App* archive_command =
        app.add_subcommand("archive", "Write a copy of the database named by a date");
    archive_date.add_to(*archive_command, "Date in the copy's name (default today, UTC)");
    archive_command->add_option("--dir", archive_directory,
                                "Directory of the copy (default archive/ beside the database)");
    archive_command->callback([this, &context] {
      const std::filesystem::path directory =
          archive_directory.empty() ? context.file().parent_path() / "archive"
                                    : std::filesystem::absolute(archive_directory);
      std::filesystem::create_directories(directory);
      sqlite::database database = context.read_only();
      context.out << archive(database, directory, archive_date.day()).string() << '\n';
    });

    CLI::App* log_command = app.add_subcommand("log", "Print every change to the database");
    log_command->add_option("--table", logged_table, "Only changes to this table");
    log_command->callback([this, &context] {
      sqlite::database database = context.read_only();
      context.out << "change_id\tchanged_at\ttable\toperation\told_row\tnew_row\n";
      for (const change& change : read_changes(database, logged_table)) {
        context.out << change.id << '\t' << change.changed_at << '\t' << change.table << '\t'
                    << change.operation << '\t' << change.old_row.value_or("") << '\t'
                    << change.new_row.value_or("") << '\n';
      }
    });

    CLI::App* check_command = app.add_subcommand(
        "check", "Load one day and fail if a listing lacks its symbol or its spec");
    check_date.add_to(*check_command);
    check_universe.add_to(*check_command);
    check_command->callback([this, &context] {
      sqlite::database database = context.read_only();
      const history loaded = load_history(database, one_day(check_date.day()), check_universe);
      context.out << loaded.listings().size() << " listings of " << loaded.instruments().size()
                  << " instruments load on " << check_date.date << '\n';
    });
  }
};

}  // namespace nq::db::cli
