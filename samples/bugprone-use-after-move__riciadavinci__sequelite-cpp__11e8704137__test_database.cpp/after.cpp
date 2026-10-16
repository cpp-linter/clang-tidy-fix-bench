#include <gtest/gtest.h>
#include <sequelite/types.hpp>
#include <sequelite/sequelite.hpp>

#include <optional>
#include <string>
#include <cstdio>

namespace sequelite::testing {

// ===================================
// === Tests for Database Creation ===

TEST(DatabaseCreationTest, InMemoryDatabaseSuccess) {
    auto db = Database::create(":memory:");
    
    EXPECT_TRUE(db.has_value());
    EXPECT_TRUE(db);            // Tests std::optional truthiness
    EXPECT_TRUE(db.value());    // Tests Database's internal operator bool()
}

TEST(DatabaseCreationTest, FileBasedDatabaseSuccess) {
    const std::string filename = "test_database.db";

    // Ensure clean slate just in case
    std::remove(filename.c_str());

    {
        auto db = Database::create(filename);
        
        EXPECT_TRUE(db.has_value());
        EXPECT_TRUE(db);
        EXPECT_TRUE(db.value());
    }

    // Clean up the file after test completion
    std::remove(filename.c_str());
}

TEST(DatabaseCreationTest, EmptyFilenameFailure) {
    auto db = Database::create("");

    EXPECT_FALSE(db.has_value());
    EXPECT_FALSE(db);
    // DO NOT call db.value() here, because there is no database instance!
}

TEST(DatabaseCreationTest, InvalidFilePathFailure) {
    auto db = Database::create("non_existent_directory_123456/test.db");

    EXPECT_FALSE(db.has_value());
    EXPECT_FALSE(db);
}


// ===================================================
// === Tests for Database Semantics & PIMPL Safety ===

TEST(DatabaseSemanticsTest, MoveConstructorSuccess) {
    auto db_opt = Database::create(":memory:");
    ASSERT_TRUE(db_opt.has_value());

    Database original_db = std::move(*db_opt);
    Database moved_db {std::move(original_db)};

    EXPECT_FALSE(original_db);
    
    EXPECT_TRUE(moved_db);

    auto result = moved_db.execute("CREATE TABLE test (id INT);");
    EXPECT_TRUE(result.success_flag);
    EXPECT_TRUE(result.ok());
}

TEST(DatabaseSemanticsTest, MoveAssignmentSuccess) {
    auto opt1 = Database::create(":memory:");
    auto opt2 = Database::create(":memory:");
    ASSERT_TRUE(opt1.has_value());
    ASSERT_TRUE(opt2.has_value());

    Database db1 = std::move(*opt1);
    Database db2 = std::move(*opt2);
    ASSERT_TRUE(db1.execute("CREATE TABLE table1 (id INT);").ok());
    ASSERT_TRUE(db2.execute("CREATE TABLE table2 (id INT);").ok());

    db1 = std::move(db2);

    EXPECT_FALSE(db2);

    EXPECT_TRUE(db1);
    EXPECT_TRUE(db1.execute("INSERT INTO table2 (id) VALUES (42);").ok());
    EXPECT_FALSE(db1.execute("SELECT * FROM table1;").ok());
}

TEST(DatabaseSemanticsTest, BooleanOperatorEvaluation) {
    auto db_opt = Database::create(":memory:");
    ASSERT_TRUE(db_opt.has_value());

    Database db = std::move(*db_opt);
    EXPECT_TRUE(db);

    Database moved_to_db = std::move(db);
    
    EXPECT_FALSE(db);
    EXPECT_TRUE(moved_to_db);
}


// ========================================
// === Tests for Database SQL Execution ===

TEST(DatabaseExecutionTest, ValidDdlDmlSuccess) {
    auto db_opt = Database::create(":memory:");
    ASSERT_TRUE(db_opt.has_value());

    Database db = std::move(*db_opt);

    EXPECT_TRUE(db.execute("CREATE TABLE test1 (id INT);").ok());
    EXPECT_TRUE(db.execute("INSERT INTO test1 (id) VALUES (67);").ok());
}

TEST(DatabaseExecutionTest, EmptyQueryGuardFailure) {
    auto db_opt = Database::create(":memory:");
    ASSERT_TRUE(db_opt.has_value());
    Database db = std::move(*db_opt);

    auto result = db.execute(""); 

    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.message.empty());
}

TEST(DatabaseExecutionTest, MalformedSqlErrorHandling) {
    auto db_opt = Database::create(":memory:");
    ASSERT_TRUE(db_opt.has_value());
    Database db = std::move(*db_opt);

    // Intentional typo
    auto result = db.execute("CREATE TABLE test 1 (id INTR);");

    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.message.empty());
}

TEST(DatabaseExecutionTest, NullDatabaseHandleFailure) {
    auto db_opt = Database::create(":memory:");
    ASSERT_TRUE(db_opt.has_value());
    
    Database db = std::move(*db_opt);
    ASSERT_TRUE(db);
    
    Database db_moved_to(std::move(db));
    ASSERT_TRUE(db_moved_to);

    auto result = db.execute("CREATE TABLE test (id INT);"); // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.message.empty());
}

}; // namespace sequelite::testing