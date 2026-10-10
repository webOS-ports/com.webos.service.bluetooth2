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
