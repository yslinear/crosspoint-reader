add_executable(ThumbScaleTest
  ThumbScaleTest.cpp
)

target_include_directories(ThumbScaleTest PRIVATE
  ${REPO_ROOT}/lib/Xtc
)

target_link_libraries(ThumbScaleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ThumbScaleTest)
