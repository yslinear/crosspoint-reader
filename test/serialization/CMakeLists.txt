add_executable(SerializationLimitsTest
  SerializationLimitsTest.cpp
)

target_link_libraries(SerializationLimitsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SerializationLimitsTest)
