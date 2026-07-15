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

# ---- Utility library ----
add_library(cvknxd_util STATIC
    util/query_string.cpp
    util/json_builder.cpp
    util/hex.cpp
    util/debug_log.cpp
)

target_include_directories(cvknxd_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknxd_util PUBLIC)

# ---- Version library ----
add_library(cvknxd_version STATIC
    version.cpp
)

target_include_directories(cvknxd_version PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cvknxd_version PUBLIC APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(cvknxd_version PUBLIC)

# ---- knxd client library ----
add_library(cvknxd_knxd STATIC
    knxd/knxd_protocol.cpp
    knxd/knxd_client.cpp
    knxd/knxd_connector.cpp
)

target_include_directories(cvknxd_knxd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${KNXD_INCLUDE_DIR})
target_link_libraries(cvknxd_knxd PUBLIC cvknxd_util)

# ---- State library (sessions only — knxd handles its own cache) ----
add_library(cvknxd_state STATIC
    state/session_store.cpp
)

target_include_directories(cvknxd_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknxd_state PUBLIC cvknxd_util)

# ---- FCGI server library ----
add_library(cvknxd_fcgi STATIC
    fcgi/fcgi_server.cpp
    fcgi/fcgi_request.cpp
)

target_include_directories(cvknxd_fcgi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknxd_fcgi PUBLIC ${FCGI_LIBRARY})

# ---- Router library ----
add_library(cvknxd_router STATIC
    router/router.cpp
)

target_include_directories(cvknxd_router PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknxd_router PUBLIC cvknxd_handlers cvknxd_fcgi)

# ---- Handlers library ----
add_library(cvknxd_handlers STATIC
    handlers/login_handler.cpp
    handlers/read_handler.cpp
    handlers/write_handler.cpp
)

target_include_directories(cvknxd_handlers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cvknxd_handlers PUBLIC cvknxd_knxd cvknxd_state cvknxd_util)

# ---- Main executable ----
add_executable(cometvisu-knxd-fcgi
    main.cpp
)

target_link_libraries(cometvisu-knxd-fcgi PRIVATE
    cvknxd_version
    cvknxd_fcgi
    cvknxd_router
    cvknxd_handlers
    cvknxd_knxd
    cvknxd_state
    cvknxd_util
    ${FCGI_LIBRARY}
)
