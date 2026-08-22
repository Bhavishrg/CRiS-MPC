# Crake — private detection of cycles in federated graphs.
#
# Two binaries:
#   crake_holder    the data holders' offline step: partition the graph across
#                   m clients and write arithmetic RSS share files
#   crake_protocol  the three computing servers; run one process per party
#
# Use ../run_crake.sh to build and drive both in one command.

find_package(OpenMP)

# The holder is a standalone client tool: it never touches the MPC backend.
add_executable(crake_holder ${CMAKE_CURRENT_SOURCE_DIR}/holder/crake_holder.cpp)

add_executable(crake_protocol
  ${CMAKE_CURRENT_SOURCE_DIR}/crake.cpp
  ${CMAKE_SOURCE_DIR}/benchmark/utils.cpp   # bench::StatsPoint, bench::saveJson
)

target_link_libraries(crake_protocol
  PRIVATE
    emp-tool::emp-tool
    nlohmann_json::nlohmann_json
)

# The online evaluator parallelises each circuit level with OpenMP.  Without
# this the build still succeeds but every party runs single-threaded.
if(OpenMP_CXX_FOUND)
  target_link_libraries(crake_protocol PRIVATE OpenMP::OpenMP_CXX)
endif()
