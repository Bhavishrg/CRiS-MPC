# Benchmarks for SS_PSI/3pc

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

  target_include_directories(${target_name}
    PRIVATE
      ${CMAKE_SOURCE_DIR}
  )
endfunction()

add_3pc_benchmark(bench_gate)
add_3pc_benchmark(bench_linear)
add_3pc_benchmark(bench_mult)
add_3pc_benchmark(bench_propagate)