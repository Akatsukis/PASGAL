pasgal_add_executable(dinic ${CMAKE_CURRENT_LIST_DIR}/dinic/dinic.cpp)
target_include_directories(dinic PRIVATE ${CMAKE_CURRENT_LIST_DIR})

pasgal_add_executable(push-relabel ${CMAKE_CURRENT_LIST_DIR}/push-relabel/push-relabel.cpp)
target_include_directories(push-relabel PRIVATE ${CMAKE_CURRENT_LIST_DIR})
