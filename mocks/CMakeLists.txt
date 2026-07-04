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

add_library(cvknxd_mocks STATIC
    mock_knxd_socket.cpp
)

target_include_directories(cvknxd_mocks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknxd_mocks PUBLIC cvknxd_knxd cvknxd_util)
