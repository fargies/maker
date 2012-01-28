
set(maker_SRCS
    main.c)
add_executable(maker ${maker_SRCS})
target_link_libraries(maker rt pthread)

install(TARGETS maker
    RUNTIME DESTINATION bin)

