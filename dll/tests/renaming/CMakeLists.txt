cmake_minimum_required(VERSION 3.20)
project(RenamingCompatibility LANGUAGES CXX)
enable_testing()

# Compile the actual decoder from before native slot renaming was introduced.
# Keep this separate from the native-layout tests so it can run independently.
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" show
    de76098779ffcc45a78aa4f3751eb6b50a1eee19:dll/src/drill.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/legacy_drill.cpp"
    COMMAND_ERROR_IS_FATAL ANY)
file(READ "${CMAKE_CURRENT_BINARY_DIR}/legacy_drill.cpp" legacy_source)
string(REPLACE "namespace opendojo::drill {"
    "namespace opendojo::legacy_drill {\nusing namespace opendojo::drill;"
    legacy_source "${legacy_source}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/legacy_drill.cpp" "${legacy_source}")
add_executable(renaming_tests renaming_tests.cpp ../../src/drill.cpp
    ../../src/slot_labels.cpp "${CMAKE_CURRENT_BINARY_DIR}/legacy_drill.cpp")
target_include_directories(renaming_tests PRIVATE ../../include)
target_compile_features(renaming_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(renaming_tests PRIVATE /W4 /utf-8)
endif()
add_test(NAME renaming_compatibility COMMAND renaming_tests
    "${CMAKE_CURRENT_BINARY_DIR}/named-drill.txt")
