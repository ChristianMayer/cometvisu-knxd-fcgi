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

cmake_minimum_required(VERSION 3.20)
project(cometvisu-knxd-fcgi
    VERSION 0.1.0
    DESCRIPTION "FastCGI backend bridging CometVisu Protocol to knxd"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Options ----
option(BUILD_TESTING "Build tests" ON)
option(ENABLE_SANITIZERS "Enable address/undefined-behavior sanitizers" OFF)

# ---- Dependencies ----
include(FetchContent)

# Google Test
if(BUILD_TESTING)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# Find FastCGI library
find_library(FCGI_LIBRARY fcgi)
if(NOT FCGI_LIBRARY)
    message(FATAL_ERROR "FastCGI library (libfcgi) not found. "
            "Install with: sudo apt install libfcgi-dev")
endif()
find_path(FCGI_INCLUDE_DIR fcgi_stdio.h)
if(NOT FCGI_INCLUDE_DIR)
    message(FATAL_ERROR "FastCGI headers not found. "
            "Install with: sudo apt install libfcgi-dev")
endif()

# ---- Compiler Settings ----
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- Subdirectories ----
add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(mocks)
    add_subdirectory(tests)
endif()

# ---- Install ----
install(TARGETS cometvisu-knxd-fcgi RUNTIME DESTINATION bin)
