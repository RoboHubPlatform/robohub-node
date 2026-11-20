cmake_minimum_required(VERSION 3.16)
project(robohub-managed C)

# ---------------------------------------------------------------------------
# The managed layer: firmware updates over the link (RNODE-UPD-*, ADR-0003).
# It needs the common contracts (the update slot), the core and the link.
# ---------------------------------------------------------------------------
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
  message(FATAL_ERROR "robohub-managed needs robohub-common, robohub-core and robohub-transport")
endif()

if(PROJECT_IS_TOP_LEVEL)
  add_subdirectory("${ROBOHUB_COMMON_DIR}" robohub-common)
  add_subdirectory("${ROBOHUB_CORE_DIR}" robohub-core)
  add_subdirectory("${ROBOHUB_TRANSPORT_DIR}" robohub-transport)
endif()

# Monocypher, vendored and unchanged (vendor/monocypher/README.md): only the
# Ed25519 check is called, and a board's link discards the rest.
add_library(robohub-managed STATIC
    src/managed.c
    vendor/monocypher/monocypher.c
    vendor/monocypher/monocypher-ed25519.c
)
target_include_directories(robohub-managed PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ROBOHUB_COMMON_DIR}/include
    ${ROBOHUB_COMMON_DIR}/include/robohub
    ${ROBOHUB_CORE_DIR}/include
    ${ROBOHUB_TRANSPORT_DIR}/include
)
target_include_directories(robohub-managed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/monocypher)
target_link_libraries(robohub-managed PUBLIC robohub-common robohub-core robohub-transport)
target_compile_features(robohub-managed PUBLIC c_std_11)

# Host tests: a fake slot, a fake clock and a gateway end of the link.
if(PROJECT_IS_TOP_LEVEL AND RHO_BUILD_HOST_TESTS AND NOT RHO_TARGET_IS_BAREMETAL)
  message(STATUS "[robohub-managed] registering host tests")
  enable_testing()
  add_executable(robohub-managed-test tests/test_managed.c)
  target_link_libraries(robohub-managed-test PRIVATE robohub-managed)
  target_include_directories(robohub-managed-test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/vendor/monocypher
      ${ROBOHUB_CORE_DIR}/src)
  add_test(NAME robohub-managed-test COMMAND robohub-managed-test)

  # The end to end update (tests/e2e_update.py): a Linux host board, with
  # robohub-linux's file slot, updated by a Python gateway over a pseudo-terminal.
  # Built when robohub-linux is checked out beside the collection.
  get_filename_component(ROBOHUB_LINUX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../robohub-linux" ABSOLUTE)
  if(DEFINED ENV{ROBOHUB_LINUX_DIR})
    set(ROBOHUB_LINUX_DIR "$ENV{ROBOHUB_LINUX_DIR}")
  endif()
  # The python3 a shell finds, which is where the test's `cryptography` is.
  find_program(ROBOHUB_PYTHON3 python3)
  if(EXISTS "${ROBOHUB_LINUX_DIR}/CMakeLists.txt" AND ROBOHUB_PYTHON3)
    add_subdirectory("${ROBOHUB_LINUX_DIR}" robohub-linux)
    add_executable(robohub-managed-host-node tests/e2e/host_node.c)
    target_link_libraries(robohub-managed-host-node PRIVATE robohub-managed robohub-linux)
    add_test(NAME robohub-managed-e2e-update
             COMMAND ${ROBOHUB_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_update.py
                     $<TARGET_FILE:robohub-managed-host-node>)
    # 77: the machine has no `cryptography` to sign with — a skip, said, not a failure.
    set_tests_properties(robohub-managed-e2e-update PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
  else()
    message(STATUS "[robohub-managed] robohub-linux not beside the collection: the end to end update is not built")
  endif()
endif()

install(FILES module.yml Kconfig DESTINATION share/robohub-managed)
install(DIRECTORY include/ DESTINATION include)
install(TARGETS robohub-managed ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
