# Benchmarks for CRiS-MPC/3pc

find_package(OpenMP)

function(add_3pc_benchmark target_name)
  set(source_file "${target_name}.cpp")
  if(ARGN)
    list(GET ARGN 0 source_file)
  endif()

  add_executable(${target_name}
    ${source_file}
    utils.cpp
  )

  target_link_libraries(${target_name}
    PRIVATE
      emp-tool::emp-tool
      nlohmann_json::nlohmann_json
  )

  if(OpenMP_CXX_FOUND)
    target_link_libraries(${target_name}
      PRIVATE
        OpenMP::OpenMP_CXX
    )
  endif()

  target_include_directories(${target_name}
    PRIVATE
      ${CMAKE_SOURCE_DIR}
  )
endfunction()

add_3pc_benchmark(bench_gate)
add_3pc_benchmark(bench_linear)
add_3pc_benchmark(bench_mult)
add_3pc_benchmark(bench_propagate)
add_3pc_benchmark(bench_permsh)
add_3pc_benchmark(bench_unshuffle)
add_3pc_benchmark(bench_repeat_shuffle ${CMAKE_SOURCE_DIR}/graphiti/bench_repeat_shuffle.cpp)
add_3pc_benchmark(bench_bfs_mpa ${CMAKE_SOURCE_DIR}/graphiti/bench_bfs_mpa.cpp)
add_3pc_benchmark(bench_pagerank_mpa ${CMAKE_SOURCE_DIR}/graphiti/bench_pagerank_mpa.cpp)
add_3pc_benchmark(bench_sort)
