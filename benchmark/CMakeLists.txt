# Benchmarks for CRiS-MPC/3pc

find_package(OpenMP)

function(add_benchmark target_name)
  set(source_file "primitives/${target_name}.cpp")
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

add_benchmark(bench_gate)
add_benchmark(bench_linear)
add_benchmark(bench_mult)
add_benchmark(bench_propagate)
add_benchmark(bench_permsh)
add_benchmark(bench_amor_permshare)
add_benchmark(bench_unshuffle)
add_benchmark(bench_shuffle ${CMAKE_SOURCE_DIR}/benchmark/graphiti/bench_shuffle.cpp)
add_benchmark(microbench_InitGraphiti ${CMAKE_SOURCE_DIR}/benchmark/graphiti/microbench_InitGraphiti.cpp)
add_benchmark(bench_BfsMpaGraphiti ${CMAKE_SOURCE_DIR}/benchmark/graphiti/bench_BfsMpaGraphiti.cpp)
add_benchmark(bench_PrMpaGraphiti ${CMAKE_SOURCE_DIR}/benchmark/graphiti/bench_PrMpaGraphiti.cpp)
add_benchmark(bench_PrMpaGraSP ${CMAKE_SOURCE_DIR}/benchmark/grasp/bench_PrMpaGraSP.cpp)
add_benchmark(RiskPropagation ${CMAKE_SOURCE_DIR}/benchmark/grasp/RiskPropagation.cpp)
add_benchmark(GroupConnection ${CMAKE_SOURCE_DIR}/benchmark/grasp/GroupConnection.cpp)
add_benchmark(bench_sort)
