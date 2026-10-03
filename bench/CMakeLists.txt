# Benchmarks are not tests: they are built only with REVENANT_BUILD_BENCH (the bench preset) and
# report distributions; CI only checks that they build and run.
function(revenant_add_bench target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE revenant::revenant)
  revenant_set_warnings(${target})
endfunction()

revenant_add_bench(bench_latency_pingpong latency_pingpong.cpp)
revenant_add_bench(bench_fanout fanout.cpp)
revenant_add_bench(bench_recovery recovery.cpp)
