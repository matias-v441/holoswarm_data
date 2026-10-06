#pragma once

/**
 * \file workspace_db.hpp
 * \brief Workspaces and stored queues (the holoswarm SQLite database) over the mrs_terrain worlds.
 *
 * A workspace is a named set of worlds of the mrs_terrain dataset. Workspaces and the queues created
 * in them are kept in a database file of their own. Worlds (safety area, origin, vertical limits) and
 * the RGB tiles they need are read from the dataset's worlds.sqlite, which is opened read-only: it is
 * part of the installed dataset and rebuilt with it.
 *
 * The database location is resolved by the library, not by the caller: a default-constructed
 * WorkspaceDb opens the database shipped with this package (database/holoswarm.sqlite in its
 * source tree, which share/holoswarm_data/database links to) and the dataset installed with
 * heightmap_sampler. Edits are written to that file in place, so they can be committed.
 *
 * Database (PRAGMA user_version = 1):
 *   workspaces(name PRIMARY KEY, description, created_at)
 *   workspace_worlds(workspace, world)          -- world = mrs_terrain worlds.name
 *   queues(queue_id PRIMARY KEY, workspace, created_at, updated_at, body)   -- body = queue JSON
 *
 * Not thread-safe: callers serialize access (the bridge holds one mutex around it).
 */

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace holoswarm_data
{

class WorkspaceDbError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct LatLon
{
  double lat = 0.0;
  double lon = 0.0;
};

struct Bounds
{
  double west  = 0.0;
  double south = 0.0;
  double east  = 0.0;
  double north = 0.0;

  void include(double lat, double lon);
  bool empty() const {
    return west > east;
  }
};

struct World
{
  std::string             name;
  std::optional<LatLon>   origin;
  std::optional<double>   min_z;
  std::optional<double>   max_z;
  std::vector<LatLon>     safety_area; ///< in file order
  Bounds                  bounds{1.0, 1.0, -1.0, -1.0};
};

struct Workspace
{
  std::string              name;
  std::string              description;
  std::vector<std::string> worlds;
};

struct RgbTile
{
  std::filesystem::path path;
  Bounds                bounds; ///< EPSG:4326 extent of the tile
};

/// The database shipped with this package: share/holoswarm_data/database/holoswarm.sqlite. Throws WorkspaceDbError.
std::filesystem::path defaultDatabasePath();

/// The mrs_terrain dataset installed with heightmap_sampler: share/heightmap_sampler/dataset. Throws WorkspaceDbError.
std::filesystem::path defaultTerrainDatasetDir();

class WorkspaceDb {
public:
  /// Opens this package's database and the installed mrs_terrain dataset. Throws WorkspaceDbError.
  WorkspaceDb();

  /// Opens (creating) the database and the dataset's worlds.sqlite; an empty path means its default. Throws WorkspaceDbError.
  WorkspaceDb(const std::filesystem::path &db_path, const std::filesystem::path &terrain_dataset_dir);
  ~WorkspaceDb();

  WorkspaceDb(const WorkspaceDb &)            = delete;
  WorkspaceDb &operator=(const WorkspaceDb &) = delete;

  const std::filesystem::path &databasePath() const {
    return db_path_;
  }
  const std::filesystem::path &datasetDir() const {
    return dataset_dir_;
  }

  // | ----------------------- worlds (mrs_terrain, read-only) ----------------------- |

  std::vector<std::string> worldNames() const;
  std::vector<World>       worlds(const std::vector<std::string> &names) const;
  std::vector<RgbTile>     rgbTiles(const std::vector<std::string> &world_names) const;

  // | ----------------------- workspaces ----------------------- |

  /// Creates the workspace if missing and adds the worlds it does not list yet.
  void                     ensureWorkspace(const std::string &name, const std::string &description, const std::vector<std::string> &worlds);
  std::vector<Workspace>   workspaces() const;
  std::optional<Workspace> workspace(const std::string &name) const;

  // | ----------------------- queues ----------------------- |

  void saveQueue(const std::string &queue_id, const std::string &workspace, double created_at, double updated_at, const nlohmann::json &body);
  void deleteQueue(const std::string &queue_id);

  /// Every stored queue body, in creation order.
  std::vector<nlohmann::json> loadQueues() const;

private:
  sqlite3              *db_      = nullptr; ///< workspace database (read-write)
  sqlite3              *terrain_ = nullptr; ///< mrs_terrain worlds.sqlite (read-only)
  std::filesystem::path db_path_;
  std::filesystem::path dataset_dir_;
};

} // namespace holoswarm_data
