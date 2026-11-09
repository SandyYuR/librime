file(GLOB rime_test_src ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/test)
if(TARGET rime-test-engine)
  set(memory_test_src
    ${CMAKE_CURRENT_SOURCE_DIR}/commit_round_test.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_transaction_test.cc)
  list(REMOVE_ITEM rime_test_src ${memory_test_src})
  add_executable(rime_memory_test rime_test_main.cc ${memory_test_src})
  target_sources(rime_memory_test PRIVATE ${rime_plugins_objs})
  target_link_libraries(rime_memory_test rime-test-engine
    ${GTEST_LIBRARIES})
  add_test(NAME rime_memory_test
    COMMAND rime_memory_test
    WORKING_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
endif()
add_executable(rime_test ${rime_test_src})
target_link_libraries(rime_test
  ${rime_library}
  ${rime_dict_library}
  ${rime_gears_library}
  ${rime_levers_library}
  ${GTEST_LIBRARIES})
if(BUILD_SHARED_LIBS)
  target_compile_definitions(rime_test PRIVATE RIME_IMPORTS)
endif(BUILD_SHARED_LIBS)
if(NOT Opencc_DICT_DIR)
  message(FATAL_ERROR "Could not locate the installed opencc dict directory "
                       "(e.g. t2s.json); required to build rime_test.")
endif()
target_compile_definitions(rime_test PRIVATE
  RIME_OPENCC_DICT_DIR="${Opencc_DICT_DIR}")
if(TARGET rime_memory_test)
  target_compile_definitions(rime_memory_test PRIVATE
    RIME_OPENCC_DICT_DIR="${Opencc_DICT_DIR}")
endif()

file(GLOB test_data_files ${PROJECT_SOURCE_DIR}/data/test/*.yaml)
file(COPY ${test_data_files} DESTINATION ${EXECUTABLE_OUTPUT_PATH})

add_test(NAME rime_test
  COMMAND rime_test
  WORKING_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
