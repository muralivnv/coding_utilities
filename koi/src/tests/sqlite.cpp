// Tests for sqlite.h: the statement wrapper, and what it does when a prepare
// fails.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void StmtSurvivesAFailedPrepare() {
  TEST_CASE("sqlite: an unprepared statement is a no-op, not a crash");

  sqlite3* db = nullptr;
  EXPECT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
  if (db == nullptr) return;

  // What a corrupt file, or a schema this build does not know, produces.
  Stmt bad{db, "SELECT nope FROM missing_table WHERE x = ?;"};
  EXPECT_FALSE(static_cast<bool>(bad));
  EXPECT_TRUE(bad.handle == nullptr);

  // Every accessor, in the order a caller that forgot to check would reach
  // them. Reset() is the one that used to dereference null inside SQLite:
  // sqlite3_clear_bindings does not check its argument the way bind, step,
  // reset and column_text all do.
  bad.Text(1, "hello");
  bad.Int(2, 7);
  bad.Real(3, 1.5);
  bad.Null(4);
  EXPECT_FALSE(bad.Step());
  bad.Run();
  EXPECT_EQ(bad.Column(0), std::string{});
  EXPECT_EQ(bad.Integer(0), std::int64_t{0});
  EXPECT_FALSE(bad.Double(0) > 0.0);
  bad.Reset();

  // And a prepared one still works, so the guards did not swallow the real path.
  ExecSql(db, "CREATE TABLE t(a TEXT, b INTEGER);");
  {
    Stmt insert{db, "INSERT INTO t(a,b) VALUES(?,?);"};
    EXPECT_TRUE(static_cast<bool>(insert));
    for (int i = 0; i < 2; ++i) {
      insert.Text(1, "row");
      insert.Int(2, i);
      insert.Run();
      insert.Reset();
    }
  }
  {
    Stmt count{db, "SELECT COUNT(*), a FROM t;"};
    EXPECT_TRUE(count.Step());
    EXPECT_EQ(count.Integer(0), std::int64_t{2});
    EXPECT_EQ(count.Column(1), std::string{"row"});
  }
  sqlite3_close(db);
}

}  // namespace koi
