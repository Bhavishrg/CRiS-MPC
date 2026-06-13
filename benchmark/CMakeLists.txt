# Benchmarks for CRiS-MPC/3pc

find_package(OpenMP)

function(add_3pc_benchmark target_name)
  add_executable(${target_name}
    ${target_name}.cpp
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