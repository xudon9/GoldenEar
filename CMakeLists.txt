cmake_minimum_required(VERSION 3.20)
project(GoldenEar LANGUAGES C CXX RC)

add_library(golden-ear-ldac STATIC
  third_party/ldac/encoder/src/ldaclib.c
  third_party/ldac/encoder/src/ldacBT.c
  third_party/ldac/decoder/libldacdec.c
  third_party/ldac/decoder/bit_allocation.c
  third_party/ldac/decoder/bit_reader.c
  third_party/ldac/decoder/huffCodes.c
  third_party/ldac/decoder/imdct.c
  third_party/ldac/decoder/spectrum.c
  third_party/ldac/decoder/utility.c)
target_compile_features(golden-ear-ldac PRIVATE c_std_11)
target_include_directories(golden-ear-ldac PUBLIC third_party/ldac/encoder/inc third_party/ldac/decoder)

add_library(golden-ear-core src/audio.cpp src/i18n.cpp src/encoded_audio_cache.cpp)
target_include_directories(golden-ear-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(golden-ear-core PUBLIC cxx_std_20)
target_compile_definitions(golden-ear-core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(golden-ear-core PUBLIC winmm PRIVATE golden-ear-ldac)

add_executable(golden-ear WIN32 src/app.cpp src/app.rc)
target_include_directories(golden-ear PRIVATE "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}/assets>")
set_property(SOURCE src/app.rc PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/golden-ear.ico")
target_link_libraries(golden-ear PRIVATE golden-ear-core advapi32 comdlg32 shell32 gdi32 user32 ole32 dwmapi uuid)
add_custom_target(golden-ear-tracks
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:golden-ear>/tracks"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    goldberg-aria.flac goldberg-variatio-1.flac beethoven-op18-6-i.flac LICENSE.md
    "$<TARGET_FILE_DIR:golden-ear>/tracks"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:golden-ear>/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ldac/encoder/LICENSE"
    "$<TARGET_FILE_DIR:golden-ear>/licenses/ldac-encoder-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ldac/encoder/NOTICE"
    "$<TARGET_FILE_DIR:golden-ear>/licenses/ldac-encoder-NOTICE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ldac/decoder/LICENSE"
    "$<TARGET_FILE_DIR:golden-ear>/licenses/ldac-decoder-LICENSE.txt"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/tracks"
  VERBATIM)
add_dependencies(golden-ear golden-ear-tracks)
if(MINGW)
  target_link_options(golden-ear PRIVATE -municode -static)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(golden-ear-tests tests/tests.cpp)
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
