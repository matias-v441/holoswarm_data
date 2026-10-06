# holoswarm_data

The holoswarm workspace database and the C++ library that reads and edits it (`holoswarm_data::WorkspaceDb`).
The bridge (`iroc_bridge`) links it.

A workspace is a named set of worlds of the mrs_terrain dataset. The database keeps the workspaces and the
mission queues created in them:

```
workspaces(name PRIMARY KEY, description, created_at)
workspace_worlds(workspace, world)          -- world = mrs_terrain worlds.name
queues(queue_id PRIMARY KEY, workspace, created_at, updated_at, body)   -- body = queue JSON
```

Worlds (safety areas, origins, vertical limits) and their RGB tiles come from the mrs_terrain dataset installed
with `heightmap_sampler` (`share/heightmap_sampler/dataset/worlds.sqlite`). That file is opened read-only.

## The bundled database

`database/holoswarm.sqlite` is the package's own database, and it is tracked in this repository. A
default-constructed `WorkspaceDb` opens it without being given a path:

```cpp
holoswarm_data::WorkspaceDb db;  // share/holoswarm_data/database/holoswarm.sqlite + the installed dataset
for (const auto &ws : db.workspaces()) { ... }
```

Unlike the heightmap dataset, this database is written to, so it is not copied at install time.
`share/holoswarm_data/database` is a **relative** symlink back to `database/` in the source tree. Edits made
through the bridge therefore land in this repository and show up in `git status`, ready to commit. The link
is relative because the compose builder and the runtime containers mount the workspace at different paths;
`install/` and `src/` are siblings under both.

The database uses a rollback journal rather than WAL, so the tracked file is always complete on its own.

To link a different directory (it must hold, or be allowed to create, `holoswarm.sqlite`):

```bash
colcon build --packages-select holoswarm_data --cmake-args -DHOLOSWARM_DATA_DATABASE_DIR=/path/to/database
```

Or pass a path explicitly at runtime: `WorkspaceDb(db_path, terrain_dataset_dir)`, where an empty argument
means its default. The bridge does this through `iroc_bridge/workspaces/db_path`; the integration tests use it
to keep a database of their own.

## Tests

```bash
colcon build --packages-select holoswarm_data --cmake-args -DBUILD_TESTING=ON
colcon test --packages-select holoswarm_data
```
