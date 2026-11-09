cmake_minimum_required(VERSION 3.10)
project(samsung_ssc_encoder C)

option(SSC_ENCODER_BUILD_EXAMPLES "Build the command-line tools" ON)
option(SSC_ENCODER_BUILD_TESTS "Build the conformance test (needs the decoder copy in tests/decoder)" ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W3)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

find_library(MATH_LIBRARY m)

# The encoder library
add_library(ssc_encoder
  src/ssc_encoder.c
  src/ssc_enc_analysis.c
  src/ssc_enc_alloc.c
  src/ssc_enc_bands.c
  src/ssc_enc_ec.c
  src/ssc_enc_energy.c
  src/ssc_enc_mdct.c
  src/ssc_enc_pitch.c
  src/ssc_enc_tables.c)
target_include_directories(ssc_encoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MATH_LIBRARY)
  target_link_libraries(ssc_encoder PUBLIC ${MATH_LIBRARY})
endif()

# A copy of the SSC decoder, used by the round-trip example and the tests
set(SSC_DECODER_SOURCES
  tests/decoder/ssc_alloc.c
  tests/decoder/ssc_bands.c
  tests/decoder/ssc_cwrs.c
  tests/decoder/ssc_decoder.c
  tests/decoder/ssc_ec.c
  tests/decoder/ssc_energy.c
  tests/decoder/ssc_synth.c
  tests/decoder/ssc_tables.c
  tests/decoder/ssc_twiddle.c)

if(SSC_ENCODER_BUILD_EXAMPLES OR SSC_ENCODER_BUILD_TESTS)
  add_library(ssc_decoder_test STATIC ${SSC_DECODER_SOURCES})
  target_include_directories(ssc_decoder_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/decoder)
  target_compile_definitions(ssc_decoder_test PUBLIC SSC_DEBUG_TELL)
  if(MATH_LIBRARY)
    target_link_libraries(ssc_decoder_test PUBLIC ${MATH_LIBRARY})
  endif()
endif()

if(SSC_ENCODER_BUILD_EXAMPLES)
  add_executable(ssc_enc_tool examples/ssc_enc_tool.c)
  target_link_libraries(ssc_enc_tool ssc_encoder)
  add_executable(ssc_roundtrip examples/ssc_roundtrip.c)
  target_link_libraries(ssc_roundtrip ssc_encoder ssc_decoder_test)
endif()

if(SSC_ENCODER_BUILD_TESTS)
  add_executable(ssc_conformance tests/ssc_conformance.c)
  target_link_libraries(ssc_conformance ssc_encoder ssc_decoder_test)
  enable_testing()
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME conformance
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py
                     --bin $<TARGET_FILE:ssc_conformance>)
  endif()
endif()
