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

add_executable(test_knxd_client
    test_knxd_client.cpp
)
target_link_libraries(test_knxd_client PRIVATE cvknxd_knxd cvknxd_mocks GTest::gtest GTest::gtest_main)
add_test(NAME test_knxd_client COMMAND test_knxd_client)

add_executable(test_read_handler
    test_read_handler.cpp
)
target_link_libraries(test_read_handler PRIVATE cvknxd_handlers cvknxd_knxd cvknxd_state cvknxd_mocks GTest::gtest GTest::gtest_main)
add_test(NAME test_read_handler COMMAND test_read_handler)

add_executable(test_write_handler
    test_write_handler.cpp
)
target_link_libraries(test_write_handler PRIVATE cvknxd_handlers cvknxd_knxd cvknxd_state cvknxd_mocks GTest::gtest GTest::gtest_main)
add_test(NAME test_write_handler COMMAND test_write_handler)

add_executable(test_fcgi_server_socket
    test_fcgi_server_socket.cpp
)
target_link_libraries(test_fcgi_server_socket PRIVATE cvknxd_fcgi cvknxd_util GTest::gtest GTest::gtest_main)
add_test(NAME test_fcgi_server_socket COMMAND test_fcgi_server_socket)
