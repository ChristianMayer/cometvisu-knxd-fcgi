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

#include "write_handler.h"

#include "knxd/knxd_client.h"
#include "knxd/knxd_protocol.h"
#include "state/address_cache.h"
#include "state/session_store.h"
#include "util/hex.h"
#include "util/query_string.h"

namespace cvknxd {

WriteHandler::WriteHandler(KnxdClientInterface& knxd, AddressCache& cache,
                           SessionStore& sessions)
    : knxd_(knxd), cache_(cache), sessions_(sessions) {}

WriteResult WriteHandler::handle(std::string_view query_string) {
  QueryString params{query_string};
  WriteResult result;

  // ---- Parameter validation first (400 takes priority over 401) ----
  auto addresses = params.get_all("a");
  if (addresses.empty()) {
    result.http_status = 400;
    return result;
  }

  auto value_opt = params.get("v");
  if (!value_opt) {
    result.http_status = 400;
    return result;
  }

  // ---- Session validation ----
  if (auto s_opt = params.get("s")) {
    if (!sessions_.is_valid(*s_opt)) {
      result.http_status = 401;
      return result;
    }
  }

  // Decode hex value
  auto data = hex_decode(*value_opt);
  if (data.empty() && !value_opt->empty()) {
    result.http_status = 400;  // invalid hex
    return result;
  }

  // Build APDU for write
  auto apdu = build_apdu(ApduType::Write, data);

  // Write to each address
  bool any_success = false;
  for (auto addr_str : addresses) {
    auto parsed = KnxAddress::from_cometvisu(addr_str);
    if (!parsed)
      continue;

    uint16_t eibaddr = parsed->group.to_eibaddr();
    if (knxd_.send_group_packet(eibaddr, apdu)) {
      // Update cache optimistically
      cache_.update(eibaddr, data);
      any_success = true;
    }
  }

  if (!any_success) {
    result.http_status = 404;
  }

  return result;
}

}  // namespace cvknxd
