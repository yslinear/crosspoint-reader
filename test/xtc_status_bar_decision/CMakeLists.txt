add_executable(XtcStatusBarDecisionTest
  XtcStatusBarDecisionTest.cpp
)

target_include_directories(XtcStatusBarDecisionTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(XtcStatusBarDecisionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(XtcStatusBarDecisionTest)
