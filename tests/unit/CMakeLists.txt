# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# ---- Unit tests ----
add_executable(test_query_string
    test_query_string.cpp
)
target_link_libraries(test_query_string PRIVATE cvknxd_util GTest::gtest GTest::gtest_main)
add_test(NAME test_query_string COMMAND test_query_string)

add_executable(test_hex
    test_hex.cpp
)
target_link_libraries(test_hex PRIVATE cvknxd_util GTest::gtest GTest::gtest_main)
add_test(NAME test_hex COMMAND test_hex)

add_executable(test_knxd_protocol
    test_knxd_protocol.cpp
)
target_link_libraries(test_knxd_protocol PRIVATE cvknxd_knxd GTest::gtest GTest::gtest_main)
add_test(NAME test_knxd_protocol COMMAND test_knxd_protocol)

add_executable(test_knxd_address
    test_knxd_address.cpp
)
target_link_libraries(test_knxd_address PRIVATE cvknxd_knxd GTest::gtest GTest::gtest_main)
add_test(NAME test_knxd_address COMMAND test_knxd_address)

add_executable(test_json_builder
    test_json_builder.cpp
)
target_link_libraries(test_json_builder PRIVATE cvknxd_util GTest::gtest GTest::gtest_main)
add_test(NAME test_json_builder COMMAND test_json_builder)

add_executable(test_session_store
    test_session_store.cpp
)
target_link_libraries(test_session_store PRIVATE cvknxd_state GTest::gtest GTest::gtest_main)
add_test(NAME test_session_store COMMAND test_session_store)

add_executable(test_login_handler
    test_login_handler.cpp
)
target_link_libraries(test_login_handler PRIVATE cvknxd_handlers cvknxd_state GTest::gtest GTest::gtest_main)
add_test(NAME test_login_handler COMMAND test_login_handler)

add_executable(test_debug_log
    test_debug_log.cpp
)
target_link_libraries(test_debug_log PRIVATE cvknxd_util GTest::gtest GTest::gtest_main)
add_test(NAME test_debug_log COMMAND test_debug_log)

add_executable(test_version
    test_version.cpp
)
target_link_libraries(test_version PRIVATE cvknxd_version GTest::gtest GTest::gtest_main)
add_test(NAME test_version COMMAND test_version)

add_executable(test_fcgi_server
    test_fcgi_server.cpp
)
target_link_libraries(test_fcgi_server PRIVATE cvknxd_fcgi cvknxd_util GTest::gtest GTest::gtest_main)
add_test(NAME test_fcgi_server COMMAND test_fcgi_server)
