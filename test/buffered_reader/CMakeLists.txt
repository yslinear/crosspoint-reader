add_executable(BufferedReaderTest
  BufferedReaderTest.cpp
)

# BufferedHalReader.h includes <Memory.h> (lib/Memory). The reader itself is
# header-only and SDK-free, so no other deps are needed.
target_include_directories(BufferedReaderTest PRIVATE
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(BufferedReaderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BufferedReaderTest)
