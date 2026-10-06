cmake_minimum_required(VERSION 3.16)
project(holoswarm_data)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra)
endif()

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)

# --- the library --------------------------------------------------------
#
# nlohmann_json is PUBLIC (queue bodies are nlohmann::json in the header);
# SQLite stays PRIVATE behind the forward-declared sqlite3 handle.

add_library(holoswarm_data SHARED
  src/workspace_db.cpp
)
target_include_directories(holoswarm_data
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(holoswarm_data PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(holoswarm_data PRIVATE SQLite::SQLite3 ament_index_cpp::ament_index_cpp)

# --- the database -------------------------------------------------------
#
# The database ships inside this package, so a default-constructed WorkspaceDb
# needs no path from the caller.  Unlike heightmap_sampler's dataset it is
# written to, and the edits belong in this repository, so it is not copied:
# share/holoswarm_data/database becomes a *relative* symlink to the source
# directory.  Relative, because the workspace is mounted at different paths
# when it is built and when it runs (compose/compose.build.yaml mounts
# holoswarm_ros_packages itself, the runtime containers mount its parent);
# install/ and src/ are siblings in both.  Which directory it is, is a
# build-time decision:
#
#   colcon build --packages-select holoswarm_data --cmake-args \
#     -DHOLOSWARM_DATA_DATABASE_DIR=/path/to/database

set(HOLOSWARM_DATA_DATABASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/database"
    CACHE PATH "Directory holding holoswarm.sqlite, linked from this package's share directory")

install(CODE "
  set(_share \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME}\")
  file(MAKE_DIRECTORY \"\${_share}\")
  file(RELATIVE_PATH _target \"\${_share}\" \"${HOLOSWARM_DATA_DATABASE_DIR}\")
  if(IS_SYMLINK \"\${_share}/database\" OR EXISTS \"\${_share}/database\")
    file(REMOVE_RECURSE \"\${_share}/database\")
  endif()
  message(STATUS \"Linking: \${_share}/database -> \${_target}\")
  file(CREATE_LINK \"\${_target}\" \"\${_share}/database\" SYMBOLIC)
")

# --- install and export -------------------------------------------------

install(DIRECTORY include/ DESTINATION include)

install(TARGETS holoswarm_data
  EXPORT export_holoswarm_data
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

ament_export_targets(export_holoswarm_data HAS_LIBRARY_TARGET)
ament_export_include_directories(include)
ament_export_libraries(holoswarm_data)
ament_export_dependencies(nlohmann_json)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_workspace_db test/test_workspace_db.cpp)
  target_link_libraries(test_workspace_db holoswarm_data SQLite::SQLite3)
endif()

ament_package()
