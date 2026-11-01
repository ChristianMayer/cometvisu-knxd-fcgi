// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

/**
 * @file version.h
 * @brief Compile-time version and build information.
 *
 * Values are injected by CMake via preprocessor definitions:
 *   - APP_VERSION from CMake PROJECT_VERSION
 *   - GIT_HASH from `git rev-parse --short HEAD`
 *   - KNXD_BUILD_VERSION / KNXD_BUILD_GIT_HASH from the knxd installation
 */

#pragma once

#include <string_view>

namespace cvknxd {

/// Returns the application version string (e.g., "1.1.0").
std::string_view version();

/// Returns the application name ("cometvisu-knxd-fcgi").
std::string_view application_name();

/// Returns the git hash at compile time (e.g., "357f004").
/// Returns "unknown" if the git hash could not be determined.
std::string_view git_hash();

/// Returns the knxd version at compile time (e.g., "0.14.57").
/// Returns an empty string if the knxd version could not be determined.
std::string_view knxd_build_version();

/// Returns the knxd git hash at compile time (e.g., "a1b2c3d").
/// Returns an empty string if the knxd git hash could not be determined.
std::string_view knxd_build_git_hash();

}  // namespace cvknxd
