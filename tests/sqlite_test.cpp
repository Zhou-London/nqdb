#include <nqdb/sqlite.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "temporary_directory.h"

namespace {

using nqdb::sqlite::database;
using nqdb::sqlite::statement;

database fruit_database() {
  database fruits = database::in_memory();
  fruits.run_script("CREATE TABLE fruit (name TEXT NOT NULL, weight INTEGER);"
                    "INSERT INTO fruit VALUES ('fig', 40), ('plum', NULL);");
  return fruits;
}

TEST(Sqlite, ReadsRowsColumnByColumn) {
  database fruits = fruit_database();
  statement row = fruits.prepare("SELECT name, weight FROM fruit ORDER BY name");

  ASSERT_TRUE(row.next_row());
  EXPECT_EQ(row.get<std::string_view>(0), "fig");
  EXPECT_EQ(row.get<std::int64_t>(1), 40);
  EXPECT_EQ(row.get<std::optional<std::int64_t>>(1), 40);

  ASSERT_TRUE(row.next_row());
  EXPECT_EQ(row.get<std::string_view>(0), "plum");
  EXPECT_TRUE(row.is_null(1));
  EXPECT_EQ(row.get<std::optional<std::int64_t>>(1), std::nullopt);

  EXPECT_FALSE(row.next_row());
}

TEST(Sqlite, RewindRunsAStatementAgainWithNewParameters) {
  database fruits = fruit_database();
  statement row = fruits.prepare("SELECT weight FROM fruit WHERE name = ?1 AND weight > ?2");

  row.bind(1, "fig").bind(2, 10);
  EXPECT_TRUE(row.next_row());

  row.rewind();
  row.bind(2, 50);
  EXPECT_FALSE(row.next_row());
}

TEST(Sqlite, ReportsSqliteMessagesAsErrors) {
  database fruits = fruit_database();
  EXPECT_THROW(fruits.prepare("SELECT missing FROM fruit"), nqdb::sqlite::error);
  EXPECT_THROW(fruits.run_script("INSERT INTO fruit VALUES (NULL, 1)"), nqdb::sqlite::error);
  EXPECT_THROW(database::open_read_only("/nonexistent/fruit.db"), nqdb::sqlite::error);
}

TEST(Sqlite, EnforcesForeignKeys) {
  database orchard = database::in_memory();
  orchard.run_script("CREATE TABLE tree (tree_id INTEGER PRIMARY KEY);"
                     "CREATE TABLE fruit (tree_id INTEGER NOT NULL REFERENCES tree);");
  EXPECT_THROW(orchard.run_script("INSERT INTO fruit VALUES (1)"), nqdb::sqlite::error);
}

TEST(Sqlite, CopyToWritesAFileThatOpensReadOnly) {
  const nqdb_test::temporary_directory directory;
  const std::filesystem::path copy = directory.path() / "fruit.db";
  fruit_database().copy_to(copy);

  database reopened = database::open_read_only(copy);
  statement row = reopened.prepare("SELECT count(*) FROM fruit");
  ASSERT_TRUE(row.next_row());
  EXPECT_EQ(row.get<std::int64_t>(0), 2);
  EXPECT_THROW(reopened.run_script("DELETE FROM fruit"), nqdb::sqlite::error);
}

TEST(Sqlite, CopyToRefusesToOverwriteAnExistingFile) {
  const nqdb_test::temporary_directory directory;
  const std::filesystem::path copy = directory.path() / "fruit.db";
  fruit_database().copy_to(copy);
  EXPECT_THROW(fruit_database().copy_to(copy), nqdb::sqlite::error);
}

TEST(Sqlite, CreateMakesANewFileAndRefusesAnExistingOne) {
  const nqdb_test::temporary_directory directory;
  const std::filesystem::path file = directory.path() / "new.db";
  {
    database created = database::create(file);
    created.run_script("CREATE TABLE fruit (name TEXT); INSERT INTO fruit VALUES ('fig');");
  }
  EXPECT_THROW(database::create(file), nqdb::sqlite::error);

  database reopened = database::open(file);
  statement row = reopened.prepare("SELECT name FROM fruit");
  ASSERT_TRUE(row.next_row());
  EXPECT_EQ(row.get<std::string_view>(0), "fig");
}

TEST(Sqlite, OpenRefusesAMissingFile) {
  const nqdb_test::temporary_directory directory;
  EXPECT_THROW(database::open(directory.path() / "missing.db"), nqdb::sqlite::error);
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "missing.db"));
}

std::int64_t fruit_count(database& fruits) {
  statement row = fruits.prepare("SELECT count(*) FROM fruit");
  row.next_row();
  return row.get<std::int64_t>(0);
}

TEST(Sqlite, TransactionKeepsItsWritesOnlyWhenCommitted) {
  database fruits = fruit_database();
  {
    nqdb::sqlite::transaction abandoned(fruits);
    fruits.run_script("INSERT INTO fruit VALUES ('pear', 1)");
  }
  EXPECT_EQ(fruit_count(fruits), 2);
  {
    nqdb::sqlite::transaction kept(fruits);
    fruits.run_script("INSERT INTO fruit VALUES ('pear', 1)");
    kept.commit();
  }
  EXPECT_EQ(fruit_count(fruits), 3);
}

TEST(Sqlite, AnAbandonedOuterTransactionUndoesACommittedInnerOne) {
  database fruits = fruit_database();
  {
    nqdb::sqlite::transaction outer(fruits);
    {
      nqdb::sqlite::transaction inner(fruits);
      fruits.run_script("INSERT INTO fruit VALUES ('pear', 1)");
      inner.commit();
    }
    EXPECT_EQ(fruit_count(fruits), 3);
  }
  EXPECT_EQ(fruit_count(fruits), 2);
}

TEST(Sqlite, AnAbandonedInnerTransactionLeavesTheOuterOneIntact) {
  database fruits = fruit_database();
  nqdb::sqlite::transaction outer(fruits);
  fruits.run_script("INSERT INTO fruit VALUES ('pear', 1)");
  {
    nqdb::sqlite::transaction inner(fruits);
    fruits.run_script("INSERT INTO fruit VALUES ('lime', 1)");
  }
  outer.commit();
  EXPECT_EQ(fruit_count(fruits), 3);
}

TEST(Sqlite, AnAbandonedTransactionLeavesLaterWritesOutsideAnyTransaction) {
  const nqdb_test::temporary_directory directory;
  const std::filesystem::path file = directory.path() / "fruit.db";
  fruit_database().copy_to(file);
  database writer = database::open(file);
  {
    nqdb::sqlite::transaction abandoned(writer);
    writer.run_script("INSERT INTO fruit VALUES ('pear', 1)");
  }
  writer.run_script("INSERT INTO fruit VALUES ('lime', 1)");

  database another_connection = database::open_read_only(file);
  EXPECT_EQ(fruit_count(another_connection), 3);
}

TEST(Sqlite, BindCopiesTextSoItMayEndBeforeTheStatementRuns) {
  database fruits = fruit_database();
  statement by_name = fruits.prepare("SELECT weight FROM fruit WHERE name = ?1");
  {
    const std::string short_lived = "fig";
    by_name.bind(1, short_lived);
  }
  ASSERT_TRUE(by_name.next_row());
  EXPECT_EQ(by_name.get<std::int64_t>(0), 40);
}

TEST(Sqlite, BindsOptionalsAndIntegerWrappers) {
  struct weight {
    explicit weight(std::int64_t grams) : grams_(grams) {}
    std::int64_t value() const { return grams_; }
    std::int64_t grams_;
  };
  database fruits = fruit_database();
  statement by_weight = fruits.prepare("SELECT name FROM fruit WHERE weight IS ?1");
  by_weight.bind(1, weight{40});
  ASSERT_TRUE(by_weight.next_row());
  EXPECT_EQ(by_weight.get<std::string>(0), "fig");

  by_weight.rewind();
  by_weight.bind(1, std::optional<std::int64_t>{});
  ASSERT_TRUE(by_weight.next_row());
  EXPECT_EQ(by_weight.get<std::string>(0), "plum");

  statement weights = fruits.prepare("SELECT weight FROM fruit ORDER BY name");
  ASSERT_TRUE(weights.next_row());
  EXPECT_EQ(weights.get<weight>(0).value(), 40);
  ASSERT_TRUE(weights.next_row());
  EXPECT_FALSE(weights.get<std::optional<weight>>(0).has_value());
}

TEST(Sqlite, ReportsChangedRowsAndTheLastInsertedId) {
  database fruits = fruit_database();
  fruits.run_script("INSERT INTO fruit VALUES ('pear', 1)");
  EXPECT_EQ(fruits.last_inserted_id(), 3);
  fruits.run_script("UPDATE fruit SET weight = 2 WHERE name <> 'fig'");
  EXPECT_EQ(fruits.changed_rows(), 2);
}

}  // namespace
