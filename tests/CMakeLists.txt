# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the parts of the service that work without a bus: path
# checks, id parsing, the SPP binary socket, the channel manager and the pair
# state. They link the service's own sources (without main.cpp) and GoogleTest
# from the sysroot.

find_library(GTEST_LIBRARY gtest REQUIRED)
find_library(GTEST_MAIN_LIBRARY gtest_main REQUIRED)

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)

set(COMPONENT_SOURCES ${SOURCES})
list(FILTER COMPONENT_SOURCES INCLUDE REGEX "\\.cpp$")
list(FILTER COMPONENT_SOURCES EXCLUDE REGEX "/main\\.cpp$")

add_executable(webos-bluetooth-service-tests ${TEST_SOURCES} ${COMPONENT_SOURCES})

target_include_directories(webos-bluetooth-service-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/Configured/src
)

# The binary socket lives in the test's working directory, and retries come
# every 20 ms instead of every 10 s.
target_compile_definitions(webos-bluetooth-service-tests PRIVATE
    BINARY_SOCKET_DIRECTORY="bt-test-sockets"
    WRITE_RETRY_SLEEP_TIME=20
)

target_link_libraries(webos-bluetooth-service-tests
    ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY}
    ${GLIB2_LDFLAGS} ${LUNASERVICE2_LDFLAGS} ${PBNJSON_CXX_LDFLAGS}
    ${GIO2_LDFLAGS} ${GIO-UNIX_LDFLAGS} ${PMLOG_LDFLAGS}
    rt pthread dl luna-service2++ ${EXT_LIBS})

add_test(NAME webos-bluetooth-service-unit COMMAND webos-bluetooth-service-tests)

# The audio reconnect on the real manager and profile services (tests/glue): a SIL and a Luna bus in the test
# process, and a clock the test moves. fakels.cpp's luna-service2 calls are found before the library's; the
# service's SIL factory loads glue-sil.so from the build directory (or $GLUE_SIL_DIR), which calls back into the test, so the test
# exports its symbols.
add_executable(webos-bluetooth-service-glue-tests
    glue/test_audioreconnect_glue.cpp glue/fakesil.cpp glue/fakels.cpp unit/support.cpp ${COMPONENT_SOURCES})
target_include_directories(webos-bluetooth-service-glue-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/Configured/src
)
target_compile_definitions(webos-bluetooth-service-glue-tests PRIVATE
    AUDIO_RECONNECT_FILE="bt-test-audioReconnect.txt"
    GLUE_SIL_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
set_target_properties(webos-bluetooth-service-glue-tests PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(webos-bluetooth-service-glue-tests
    ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY}
    ${GLIB2_LDFLAGS} ${LUNASERVICE2_LDFLAGS} ${PBNJSON_CXX_LDFLAGS}
    ${GIO2_LDFLAGS} ${GIO-UNIX_LDFLAGS} ${PMLOG_LDFLAGS}
    rt pthread dl luna-service2++ ${EXT_LIBS})

add_library(glue-sil MODULE glue/silmodule.cpp)
set_target_properties(glue-sil PROPERTIES PREFIX "")
target_link_options(glue-sil PRIVATE -Wl,-z,undefs)
add_dependencies(webos-bluetooth-service-glue-tests glue-sil)

add_test(NAME webos-bluetooth-service-glue COMMAND webos-bluetooth-service-glue-tests)

# A SIL with no Bluetooth behind it, for driving the service through scenarios (tests/scenarios). Loaded with
# WEBOS_BLUETOOTH_SIL=fake WEBOS_BLUETOOTH_SIL_BASE_PATH=<its directory>.
add_library(fake MODULE fakesil/fakesil.cpp)
set_target_properties(fake PROPERTIES PREFIX "")
target_link_libraries(fake ${GLIB2_LDFLAGS})
# Sanitizer runtimes, when built with them, come from the service that loads it.
target_link_options(fake PRIVATE -Wl,-z,undefs)

# libFuzzer target for what an ANCS provider sends (clang only, -DBUILD_FUZZING=ON). It links the parser alone.
option(BUILD_FUZZING "Build the libFuzzer targets (clang)" OFF)
if(BUILD_FUZZING)
    add_executable(fuzz-ancsparser fuzz/fuzz_ancsparser.cpp ${PROJECT_SOURCE_DIR}/src/ancsparser.cpp)
    target_include_directories(fuzz-ancsparser PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(fuzz-ancsparser PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(fuzz-ancsparser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
