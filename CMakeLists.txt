cmake_minimum_required(VERSION 3.20)
project(GoldenEar LANGUAGES CXX RC)

add_library(golden-ear-core audio.cpp i18n.cpp)
target_compile_features(golden-ear-core PUBLIC cxx_std_20)
target_compile_definitions(golden-ear-core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(golden-ear-core PUBLIC winmm)

add_executable(golden-ear WIN32 app.cpp app.rc)
target_include_directories(golden-ear PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(SOURCE app.rc PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/golden-ear.ico")
target_link_libraries(golden-ear PRIVATE golden-ear-core advapi32 comdlg32 shell32 gdi32 user32 ole32 dwmapi uuid)
add_custom_target(golden-ear-tracks
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:golden-ear>/tracks"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    goldberg-aria.flac goldberg-variatio-1.flac beethoven-op18-6-i.flac LICENSE.md
    "$<TARGET_FILE_DIR:golden-ear>/tracks"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/tracks"
  VERBATIM)
add_dependencies(golden-ear golden-ear-tracks)
if(MINGW)
  target_link_options(golden-ear PRIVATE -municode -static)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(golden-ear-tests tests.cpp)
  target_link_libraries(golden-ear-tests PRIVATE golden-ear-core)
  if(MINGW)
    target_link_options(golden-ear-tests PRIVATE -static)
  endif()
  add_test(NAME golden-ear-core COMMAND golden-ear-tests)
  find_program(GOLDEN_EAR_TEST_FFMPEG ffmpeg)
  if(GOLDEN_EAR_TEST_FFMPEG)
    add_test(NAME golden-ear-codecs COMMAND golden-ear-tests --audio)
    set_tests_properties(golden-ear-codecs PROPERTIES ENVIRONMENT
      "GOLDEN_EAR_FFMPEG=${GOLDEN_EAR_TEST_FFMPEG};TMP=${CMAKE_CURRENT_BINARY_DIR};TEMP=${CMAKE_CURRENT_BINARY_DIR}")
  endif()
endif()

if(MSVC)
  target_compile_options(golden-ear-core PRIVATE /W4 /utf-8)
  target_compile_options(golden-ear PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8>")
  if(BUILD_TESTING)
    target_compile_options(golden-ear-tests PRIVATE /utf-8)
  endif()
else()
  target_compile_options(golden-ear-core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(golden-ear PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>")
endif()
