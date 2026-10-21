add_library(xenia-hid-remote STATIC)
xe_platform_sources(xenia-hid-remote ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-remote PUBLIC xenia-base xenia-hid)
xe_target_defaults(xenia-hid-remote)

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
