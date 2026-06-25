add_executable(ReaderShortcutDecisionTest
  ReaderShortcutDecisionTest.cpp
)

target_include_directories(ReaderShortcutDecisionTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReaderShortcutDecisionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderShortcutDecisionTest)
