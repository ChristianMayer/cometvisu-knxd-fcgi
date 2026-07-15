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

add_executable(test_concurrent_clients
    test_concurrent_clients.cpp
)
target_link_libraries(test_concurrent_clients PRIVATE cvknxd_fcgi cvknxd_util GTest::gtest GTest::gtest_main ${FCGI_LIBRARY} pthread)
add_test(NAME test_concurrent_clients COMMAND test_concurrent_clients)

add_executable(test_knxd_client_concurrency
    test_knxd_client_concurrency.cpp
)
target_link_libraries(test_knxd_client_concurrency PRIVATE cvknxd_knxd GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_knxd_client_concurrency COMMAND test_knxd_client_concurrency)

add_executable(test_fork_workers
    test_fork_workers.cpp
)
target_link_libraries(test_fork_workers PRIVATE cvknxd_fcgi cvknxd_util GTest::gtest GTest::gtest_main pthread)
add_test(NAME test_fork_workers COMMAND test_fork_workers)
