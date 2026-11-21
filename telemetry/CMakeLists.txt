cmake_minimum_required(VERSION 3.16)
project(robohub-telemetry C)

# ---------------------------------------------------------------------------
# Locating sibling modules
# ---------------------------------------------------------------------------
# Telemetry binds the event bus to transport and net, so it needs all three.
# Paths are never hard-coded: a variable the parent build already set (rho writes
# one for every dependency) wins, then the environment, then a sibling checkout,
# then a fetch.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/deps.cmake")
endif()

# Where a RoboHub module's sources are: <NAME>_DIR in the environment; else a
# folder beside this one or above it — by the module's own name (robohub-common
# and the other repositories of a workspace) or by its folder in the robohub-node
# collection (robohub-core is core/, robohub-transport link/, robohub-telemetry
# telemetry/, robohub-freertos kernels/freertos/, robohub-device-node
# device-node/); else fetched — the collection's modules from robohub-node, whose
# old repositories are archived, and anything else from its own repository.
function(robohub_locate_module module out_var)
  string(TOUPPER "${module}" upper)
  string(REPLACE "-" "_" upper "${upper}")
  if(${out_var})
    return()
  endif()
  if(DEFINED ENV{${upper}_DIR})
    set(${out_var} "$ENV{${upper}_DIR}" PARENT_SCOPE)
    return()
  endif()
  set(in_node "")
  if(module STREQUAL "robohub-core")
    set(in_node "core")
  elseif(module STREQUAL "robohub-transport")
    set(in_node "link")
  elseif(module STREQUAL "robohub-telemetry")
    set(in_node "telemetry")
  elseif(module STREQUAL "robohub-freertos")
    set(in_node "kernels/freertos")
  elseif(module STREQUAL "robohub-device-node")
    set(in_node "device-node")
  endif()
  set(dir "${CMAKE_CURRENT_SOURCE_DIR}")
  foreach(level RANGE 8)
    get_filename_component(parent "${dir}/.." ABSOLUTE)
    if(in_node AND EXISTS "${parent}/${in_node}/CMakeLists.txt" AND EXISTS "${parent}/${in_node}/module.yml")
      set(${out_var} "${parent}/${in_node}" PARENT_SCOPE)
      return()
    endif()
    if(in_node AND EXISTS "${parent}/robohub-node/${in_node}/CMakeLists.txt")
      set(${out_var} "${parent}/robohub-node/${in_node}" PARENT_SCOPE)
      return()
    endif()
    if(NOT in_node AND EXISTS "${parent}/${module}/CMakeLists.txt")
      set(${out_var} "${parent}/${module}" PARENT_SCOPE)
      return()
    endif()
    set(dir "${parent}")
  endforeach()
  include(FetchContent)
  if(in_node)
    message(STATUS "[${PROJECT_NAME}] fetching robohub-node for ${module}")
    FetchContent_Declare(robohub-node
      GIT_REPOSITORY "https://github.com/RoboHubPlatform/robohub-node.git"
      GIT_TAG main GIT_SHALLOW TRUE GIT_PROGRESS TRUE)
    FetchContent_GetProperties(robohub-node)
    if(NOT robohub-node_POPULATED)
      FetchContent_Populate(robohub-node)
    endif()
    set(${out_var} "${robohub-node_SOURCE_DIR}/${in_node}" PARENT_SCOPE)
    return()
  endif()
  message(STATUS "[${PROJECT_NAME}] fetching ${module}")
  FetchContent_Declare(${module}
    GIT_REPOSITORY "https://github.com/RoboHubPlatform/${module}.git"
    GIT_TAG main GIT_SHALLOW TRUE GIT_PROGRESS TRUE)
  FetchContent_MakeAvailable(${module})
  set(${out_var} "${${module}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

robohub_locate_module(robohub-common ROBOHUB_COMMON_DIR)
robohub_locate_module(robohub-core ROBOHUB_CORE_DIR)
robohub_locate_module(robohub-transport ROBOHUB_TRANSPORT_DIR)
if(NOT ROBOHUB_COMMON_DIR OR NOT ROBOHUB_CORE_DIR OR NOT ROBOHUB_TRANSPORT_DIR)
  message(FATAL_ERROR "robohub-common, robohub-core or robohub-transport not found and could not be fetched")
endif()
message(STATUS "[${PROJECT_NAME}] robohub-common at ${ROBOHUB_COMMON_DIR}")
message(STATUS "[${PROJECT_NAME}] robohub-core at ${ROBOHUB_CORE_DIR}")
message(STATUS "[${PROJECT_NAME}] robohub-transport at ${ROBOHUB_TRANSPORT_DIR}")

# Added only when this module *is* the project. Built as a dependency, the
# generated project adds these itself, and adding them here too would be two
# targets of one name. ESP-IDF's `project()` makes every directory look
# top-level, so the question is asked of the source directory rather than of
# PROJECT_IS_TOP_LEVEL (as robohub-esp32 does; robohub ADR-0025).
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  add_subdirectory("${ROBOHUB_COMMON_DIR}" robohub-common)
  add_subdirectory("${ROBOHUB_CORE_DIR}" robohub-core)
  add_subdirectory("${ROBOHUB_TRANSPORT_DIR}" robohub-transport)
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(robohub-telemetry STATIC src/telemetry.c)

target_include_directories(robohub-telemetry PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ROBOHUB_COMMON_DIR}/include
    ${ROBOHUB_COMMON_DIR}/include/robohub
    ${ROBOHUB_CORE_DIR}/include
    ${ROBOHUB_TRANSPORT_DIR}/include
)
target_link_libraries(robohub-telemetry PUBLIC
    robohub-common
    robohub-core
    robohub-transport)
target_compile_features(robohub-telemetry PUBLIC c_std_11)

# ---------------------------------------------------------------------------
# Host tests
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL AND RHO_BUILD_HOST_TESTS AND NOT RHO_TARGET_IS_BAREMETAL)
  message(STATUS "[${PROJECT_NAME}] registering host unit tests")
  enable_testing()
  add_executable(robohub-telemetry-test tests/test_telemetry.c)
  target_link_libraries(robohub-telemetry-test PRIVATE robohub-telemetry)
  target_include_directories(robohub-telemetry-test PRIVATE "${ROBOHUB_CORE_DIR}/src")
  add_test(NAME robohub-telemetry-test COMMAND robohub-telemetry-test)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(FILES module.yml DESTINATION share/robohub-telemetry)
install(DIRECTORY include/ DESTINATION include)
install(TARGETS robohub-telemetry
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
