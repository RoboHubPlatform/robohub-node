cmake_minimum_required(VERSION 3.16)
project(robohub-transport C)

# ---------------------------------------------------------------------------
# Locating sibling modules
# ---------------------------------------------------------------------------
# Framing sits between the physical link and the runtime, so it needs the common
# contracts and the core event bus it publishes onto. Paths are never hard-coded:
# a variable the parent build already set (rho writes one for every dependency)
# wins, then the environment, then a sibling checkout, then a fetch.
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
if(NOT ROBOHUB_COMMON_DIR OR NOT ROBOHUB_CORE_DIR)
  message(FATAL_ERROR "robohub-common or robohub-core not found and could not be fetched")
endif()
message(STATUS "[${PROJECT_NAME}] robohub-common at ${ROBOHUB_COMMON_DIR}")
message(STATUS "[${PROJECT_NAME}] robohub-core at ${ROBOHUB_CORE_DIR}")

# Added only when this module *is* the project. Built as a dependency, the
# generated project adds these itself, and adding them here too would be two
# targets of one name. ESP-IDF's `project()` makes every directory look
# top-level, so the question is asked of the source directory rather than of
# PROJECT_IS_TOP_LEVEL (as robohub-esp32 does; robohub ADR-0025).
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  add_subdirectory("${ROBOHUB_COMMON_DIR}" robohub-common)
  add_subdirectory("${ROBOHUB_CORE_DIR}" robohub-core)
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(robohub-transport STATIC src/transport.c src/secure.c)

target_include_directories(robohub-transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ROBOHUB_COMMON_DIR}/include
    ${ROBOHUB_COMMON_DIR}/include/robohub
    ${ROBOHUB_CORE_DIR}/include
)
target_link_libraries(robohub-transport PUBLIC robohub-common robohub-core)
target_compile_features(robohub-transport PUBLIC c_std_11)

# ---------------------------------------------------------------------------
# Host tests
# ---------------------------------------------------------------------------
# Framing is ordinary portable C and runs on the machine doing the building. The
# test drives the event bus directly, so it reaches the core's private initialiser
# the way a runtime would. Both tests link a keystore of their own
# (tests/keystore_double.c) in place of a platform's.
if(PROJECT_IS_TOP_LEVEL AND RHO_BUILD_HOST_TESTS AND NOT RHO_TARGET_IS_BAREMETAL)
  message(STATUS "[${PROJECT_NAME}] registering host unit tests")
  enable_testing()
  add_executable(robohub-transport-test tests/test_transport.c tests/keystore_double.c)
  target_link_libraries(robohub-transport-test PRIVATE robohub-transport)
  target_include_directories(robohub-transport-test PRIVATE "${ROBOHUB_CORE_DIR}/src")
  add_test(NAME robohub-transport-test COMMAND robohub-transport-test)
  add_executable(robohub-transport-secure-test tests/test_secure.c tests/keystore_double.c)
  target_link_libraries(robohub-transport-secure-test PRIVATE robohub-transport)
  add_test(NAME robohub-transport-secure-test COMMAND robohub-transport-secure-test)
  # The board against the known-answer vectors robohub-gateway is checked
  # against too (tests/vectors/secure.json, written by tests/vectors/generate.py).
  add_executable(robohub-transport-vectors-test tests/test_vectors.c tests/keystore_double.c)
  target_link_libraries(robohub-transport-vectors-test PRIVATE robohub-transport)
  add_test(NAME robohub-transport-vectors-test
           COMMAND robohub-transport-vectors-test "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/secure.json")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(FILES module.yml DESTINATION share/robohub-transport)
install(DIRECTORY include/ DESTINATION include)
install(TARGETS robohub-transport
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
