#include "holoswarm_data/workspace_db.hpp"

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

namespace holoswarm_data
{

namespace
{

constexpr int kSchemaVersion = 1;

const char *kSchema = R"sql(
CREATE TABLE IF NOT EXISTS workspaces (
    name        TEXT PRIMARY KEY,
    description TEXT NOT NULL DEFAULT '',
    created_at  REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS workspace_worlds (
    workspace TEXT NOT NULL REFERENCES workspaces(name) ON DELETE CASCADE,
    world     TEXT NOT NULL,
    PRIMARY KEY (workspace, world)
);
CREATE TABLE IF NOT EXISTS queues (
    queue_id   TEXT PRIMARY KEY,
    workspace  TEXT NOT NULL REFERENCES workspaces(name),
    created_at REAL NOT NULL,
    updated_at REAL NOT NULL,
    body       TEXT NOT NULL
);
)sql";

double now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Prepared statement, finalized on scope exit.
class Statement {
public:
  Statement(sqlite3 *db, const char *sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      throw WorkspaceDbError(std::string("SQL error: ") + sqlite3_errmsg(db) + " in: " + sql);
    }
  }
  ~Statement() {
    sqlite3_finalize(stmt_);
  }
  Statement(const Statement &)            = delete;
  Statement &operator=(const Statement &) = delete;

  Statement &bind(int index, const std::string &value) {
    sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Statement &bind(int index, double value) {
    sqlite3_bind_double(stmt_, index, value);
    return *this;
  }

  /// true while there is a row
  bool step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
      return true;
    }
    if (rc != SQLITE_DONE) {
      throw WorkspaceDbError(std::string("SQL error: ") + sqlite3_errmsg(db_));
    }
    return false;
  }

  std::string text(int column) const {
    const auto *value = sqlite3_column_text(stmt_, column);
    return value ? reinterpret_cast<const char *>(value) : "";
  }
  bool isNull(int column) const {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
  }
  double real(int column) const {
    return sqlite3_column_double(stmt_, column);
  }
  std::optional<double> optionalReal(int column) const {
    return isNull(column) ? std::nullopt : std::optional<double>(real(column));
  }

private:
  sqlite3      *db_;
  sqlite3_stmt *stmt_ = nullptr;
};

void exec(sqlite3 *db, const char *sql) {
  char *error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "unknown error";
    sqlite3_free(error);
    throw WorkspaceDbError("SQL error: " + message);
  }
}

sqlite3 *open(const std::filesystem::path &path, int flags) {
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
    const std::string message = db ? sqlite3_errmsg(db) : "out of memory";
    sqlite3_close(db);
    throw WorkspaceDbError("Cannot open " + path.string() + ": " + message);
  }
  sqlite3_busy_timeout(db, 2000);
  return db;
}

/// "?,?,?" for n placeholders
std::string placeholders(size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) {
    out += i ? ",?" : "?";
  }
  return out;
}

std::filesystem::path shareDirectory(const std::string &package) {
  try {
    return ament_index_cpp::get_package_share_directory(package);
  }
  catch (const std::exception &e) {
    throw WorkspaceDbError("Cannot locate the " + package + " share directory: " + e.what());
  }
}

} // namespace

std::filesystem::path defaultDatabasePath() {
  return shareDirectory("holoswarm_data") / "database" / "holoswarm.sqlite";
}

std::filesystem::path defaultTerrainDatasetDir() {
  return shareDirectory("heightmap_sampler") / "dataset";
}

void Bounds::include(double lat, double lon) {
  if (empty()) {
    west = east = lon;
    south = north = lat;
    return;
  }
  west  = std::min(west, lon);
  east  = std::max(east, lon);
  south = std::min(south, lat);
  north = std::max(north, lat);
}

WorkspaceDb::WorkspaceDb() : WorkspaceDb({}, {}) {
}

WorkspaceDb::WorkspaceDb(const std::filesystem::path &db_path, const std::filesystem::path &terrain_dataset_dir)
    : db_path_(db_path.empty() ? defaultDatabasePath() : db_path),
      dataset_dir_(terrain_dataset_dir.empty() ? defaultTerrainDatasetDir() : terrain_dataset_dir) {
  const auto worlds_db = dataset_dir_ / "worlds.sqlite";
  if (!std::filesystem::exists(worlds_db)) {
    throw WorkspaceDbError("No worlds database at " + worlds_db.string());
  }
  terrain_ = open(worlds_db, SQLITE_OPEN_READONLY);

  try {
    if (db_path_.has_parent_path()) {
      std::filesystem::create_directories(db_path_.parent_path());
    }
    db_ = open(db_path_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    // A rollback journal, not WAL: the default database is tracked in git, so the .sqlite file must
    // be complete on its own, without -wal/-shm files beside it.
    exec(db_, "PRAGMA journal_mode = DELETE; PRAGMA foreign_keys = ON;");

    Statement version(db_, "PRAGMA user_version");
    version.step();
    const int found = static_cast<int>(version.real(0));
    if (found > kSchemaVersion) {
      throw WorkspaceDbError(db_path_.string() + " has schema version " + std::to_string(found) + ", newer than this library (" +
                             std::to_string(kSchemaVersion) + ")");
    }
    exec(db_, kSchema);
    exec(db_, ("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());
  }
  catch (...) {
    sqlite3_close(db_);
    sqlite3_close(terrain_);
    throw;
  }
}

WorkspaceDb::~WorkspaceDb() {
  sqlite3_close(db_);
  sqlite3_close(terrain_);
}

// | ----------------------- worlds ----------------------- |

std::vector<std::string> WorkspaceDb::worldNames() const {
  std::vector<std::string> out;
  Statement                q(terrain_, "SELECT name FROM worlds ORDER BY name");
  while (q.step()) {
    out.push_back(q.text(0));
  }
  return out;
}

std::vector<World> WorkspaceDb::worlds(const std::vector<std::string> &names) const {
  std::map<std::string, World> found;
  if (names.empty()) {
    return {};
  }

  {
    const std::string sql = "SELECT name, origin_lat, origin_lon, min_z, max_z FROM worlds WHERE name IN (" + placeholders(names.size()) + ")";
    Statement         q(terrain_, sql.c_str());
    for (size_t i = 0; i < names.size(); i++) {
      q.bind(static_cast<int>(i + 1), names[i]);
    }
    while (q.step()) {
      World w;
      w.name = q.text(0);
      if (!q.isNull(1) && !q.isNull(2)) {
        w.origin = LatLon{q.real(1), q.real(2)};
      }
      w.min_z            = q.optionalReal(3);
      w.max_z            = q.optionalReal(4);
      found[w.name] = std::move(w);
    }
  }
  {
    const std::string sql =
        "SELECT world, lat, lon FROM safety_area_vertices WHERE world IN (" + placeholders(names.size()) + ") ORDER BY world, seq";
    Statement q(terrain_, sql.c_str());
    for (size_t i = 0; i < names.size(); i++) {
      q.bind(static_cast<int>(i + 1), names[i]);
    }
    while (q.step()) {
      auto it = found.find(q.text(0));
      if (it != found.end()) {
        it->second.safety_area.push_back({q.real(1), q.real(2)});
        it->second.bounds.include(q.real(1), q.real(2));
      }
    }
  }

  std::vector<World> out;
  for (const auto &name : names) {
    if (auto it = found.find(name); it != found.end()) {
      out.push_back(it->second);
    }
  }
  return out;
}

std::vector<RgbTile> WorkspaceDb::rgbTiles(const std::vector<std::string> &world_names) const {
  if (world_names.empty()) {
    return {};
  }
  const std::string sql = "SELECT DISTINCT t.rgb_path, t.rgb_west, t.rgb_south, t.rgb_east, t.rgb_north "
                          "FROM world_tiles w JOIN tiles t ON t.ix = w.ix AND t.iy = w.iy "
                          "WHERE t.rgb_path IS NOT NULL AND w.world IN (" +
                          placeholders(world_names.size()) + ") ORDER BY t.iy, t.ix";
  Statement q(terrain_, sql.c_str());
  for (size_t i = 0; i < world_names.size(); i++) {
    q.bind(static_cast<int>(i + 1), world_names[i]);
  }
  std::vector<RgbTile> out;
  while (q.step()) {
    out.push_back({dataset_dir_ / q.text(0), Bounds{q.real(1), q.real(2), q.real(3), q.real(4)}});
  }
  return out;
}

// | ----------------------- workspaces ----------------------- |

void WorkspaceDb::ensureWorkspace(const std::string &name, const std::string &description, const std::vector<std::string> &worlds) {
  exec(db_, "BEGIN");
  try {
    Statement(db_, "INSERT OR IGNORE INTO workspaces (name, description, created_at) VALUES (?, ?, ?)").bind(1, name).bind(2, description).bind(3, now()).step();
    for (const auto &world : worlds) {
      Statement(db_, "INSERT OR IGNORE INTO workspace_worlds (workspace, world) VALUES (?, ?)").bind(1, name).bind(2, world).step();
    }
    exec(db_, "COMMIT");
  }
  catch (...) {
    exec(db_, "ROLLBACK");
    throw;
  }
}

std::vector<Workspace> WorkspaceDb::workspaces() const {
  std::vector<Workspace> out;
  {
    Statement q(db_, "SELECT name, description FROM workspaces ORDER BY name");
    while (q.step()) {
      out.push_back({q.text(0), q.text(1), {}});
    }
  }
  for (auto &ws : out) {
    Statement q(db_, "SELECT world FROM workspace_worlds WHERE workspace = ? ORDER BY world");
    q.bind(1, ws.name);
    while (q.step()) {
      ws.worlds.push_back(q.text(0));
    }
  }
  return out;
}

std::optional<Workspace> WorkspaceDb::workspace(const std::string &name) const {
  for (auto &ws : workspaces()) {
    if (ws.name == name) {
      return ws;
    }
  }
  return std::nullopt;
}

// | ----------------------- queues ----------------------- |

void WorkspaceDb::saveQueue(const std::string &queue_id, const std::string &workspace, double created_at, double updated_at,
                            const nlohmann::json &body) {
  Statement(db_, "INSERT INTO queues (queue_id, workspace, created_at, updated_at, body) VALUES (?, ?, ?, ?, ?) "
                 "ON CONFLICT(queue_id) DO UPDATE SET workspace = excluded.workspace, updated_at = excluded.updated_at, body = excluded.body")
      .bind(1, queue_id)
      .bind(2, workspace)
      .bind(3, created_at)
      .bind(4, updated_at)
      .bind(5, body.dump())
      .step();
}

void WorkspaceDb::deleteQueue(const std::string &queue_id) {
  Statement(db_, "DELETE FROM queues WHERE queue_id = ?").bind(1, queue_id).step();
}

std::vector<nlohmann::json> WorkspaceDb::loadQueues() const {
  std::vector<nlohmann::json> out;
  Statement                   q(db_, "SELECT body FROM queues ORDER BY created_at, queue_id");
  while (q.step()) {
    try {
      out.push_back(nlohmann::json::parse(q.text(0)));
    }
    catch (const nlohmann::json::exception &) {
      // a damaged row must not take the others down; it is skipped
    }
  }
  return out;
}

} // namespace holoswarm_data
