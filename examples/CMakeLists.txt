function(revenant_add_example target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE revenant::revenant)
  revenant_set_warnings(${target})
endfunction()

revenant_add_example(ticker_publisher ticker_publisher.cpp)
revenant_add_example(ticker_subscriber ticker_subscriber.cpp)
