#include <holoswarm_data/workspace_db.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using holoswarm_data::WorkspaceDb;

namespace
{

/// A dataset directory holding a minimal worlds.sqlite: two worlds, one RGB tile.
class WorkspaceDbTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("holoswarm_data_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(root_ / "dataset");

    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open((root_ / "dataset" / "worlds.sqlite").c_str(), &db), SQLITE_OK);
    const char *sql = R"sql(
CREATE TABLE worlds (name TEXT PRIMARY KEY, origin_lat REAL, origin_lon REAL, min_z REAL, max_z REAL);
CREATE TABLE safety_area_vertices (world TEXT, seq INTEGER, lat REAL, lon REAL, PRIMARY KEY (world, seq));
CREATE TABLE tiles (ix INTEGER, iy INTEGER, rgb_path TEXT, rgb_west REAL, rgb_south REAL, rgb_east REAL, rgb_north REAL,
                    PRIMARY KEY (ix, iy));
CREATE TABLE world_tiles (world TEXT, ix INTEGER, iy INTEGER, PRIMARY KEY (world, ix, iy));
INSERT INTO worlds VALUES ('alpha', 50.0, 14.0, 0.0, 30.0), ('beta', NULL, NULL, NULL, NULL);
INSERT INTO safety_area_vertices VALUES ('alpha', 0, 50.0, 14.0), ('alpha', 1, 50.1, 14.2), ('alpha', 2, 49.9, 14.1);
INSERT INTO tiles VALUES (1, 2, 'rgb/tile_1_2.tif', 13.0, 49.0, 15.0, 51.0);
INSERT INTO world_tiles VALUES ('alpha', 1, 2);
)sql";
    ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db);
    sqlite3_close(db);
  }

  void TearDown() override {
    fs::remove_all(root_);
  }

  fs::path dbPath() const {
    return root_ / "data" / "holoswarm.sqlite";
  }
  fs::path datasetDir() const {
    return root_ / "dataset";
  }

  fs::path root_;
};

TEST_F(WorkspaceDbTest, ReadsWorldsAndTiles) {
  WorkspaceDb db(dbPath(), datasetDir());
  EXPECT_EQ(db.databasePath(), dbPath());
  EXPECT_EQ(db.worldNames(), (std::vector<std::string>{"alpha", "beta"}));

  const auto worlds = db.worlds({"beta", "alpha", "missing"});
  ASSERT_EQ(worlds.size(), 2u);
  EXPECT_EQ(worlds[0].name, "beta");
  EXPECT_FALSE(worlds[0].origin);
  EXPECT_TRUE(worlds[0].bounds.empty());

  const auto &alpha = worlds[1];
  ASSERT_TRUE(alpha.origin);
  EXPECT_DOUBLE_EQ(alpha.origin->lat, 50.0);
  EXPECT_EQ(alpha.safety_area.size(), 3u);
  EXPECT_DOUBLE_EQ(alpha.bounds.west, 14.0);
  EXPECT_DOUBLE_EQ(alpha.bounds.east, 14.2);
  EXPECT_DOUBLE_EQ(alpha.bounds.south, 49.9);
  EXPECT_DOUBLE_EQ(alpha.bounds.north, 50.1);

  const auto tiles = db.rgbTiles({"alpha", "beta"});
  ASSERT_EQ(tiles.size(), 1u);
  EXPECT_EQ(tiles[0].path, datasetDir() / "rgb/tile_1_2.tif");
}

TEST_F(WorkspaceDbTest, KeepsWorkspacesAndQueuesAcrossReopen) {
  {
    WorkspaceDb db(dbPath(), datasetDir());
    db.ensureWorkspace("ws", "first", {"alpha"});
    db.ensureWorkspace("ws", "ignored", {"beta", "alpha"});
    db.saveQueue("q1", "ws", 1.0, 1.0, {{"id", "q1"}});
    db.saveQueue("q2", "ws", 2.0, 2.0, {{"id", "q2"}});
    db.saveQueue("q1", "ws", 1.0, 3.0, {{"id", "q1"}, {"edited", true}});
    db.deleteQueue("q2");
  }

  // the database is a single file: no WAL beside it once closed
  EXPECT_FALSE(fs::exists(dbPath().string() + "-wal"));

  WorkspaceDb db(dbPath(), datasetDir());
  const auto ws = db.workspace("ws");
  ASSERT_TRUE(ws);
  EXPECT_EQ(ws->description, "first");
  EXPECT_EQ(ws->worlds, (std::vector<std::string>{"alpha", "beta"}));
  EXPECT_FALSE(db.workspace("other"));

  const auto queues = db.loadQueues();
  ASSERT_EQ(queues.size(), 1u);
  EXPECT_EQ(queues[0]["id"], "q1");
  EXPECT_EQ(queues[0]["edited"], true);
}

TEST_F(WorkspaceDbTest, RefusesMissingDataset) {
  EXPECT_THROW(WorkspaceDb(dbPath(), root_ / "nowhere"), holoswarm_data::WorkspaceDbError);
}

} // namespace
