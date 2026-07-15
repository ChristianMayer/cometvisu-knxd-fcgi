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

#include "fcgi_server.h"

#include <fcgi_stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../util/debug_log.h"

// POSIX environment pointer — needed for direct socket mode to make
// request parameters available via getenv() after FCGX_Accept_r().
extern char** environ;

namespace cvknxd {

/// Maximum allowed request body size (64 KB).
/// Prevents OOM from a malicious CONTENT_LENGTH.
inline constexpr int kMaxContentLength = 64 * 1024;

FcgiServer::FcgiServer() = default;

FcgiServer::~FcgiServer() {
  shutdown();
  for (auto& t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

void FcgiServer::set_handler(RequestHandler handler) {
  handler_ = std::move(handler);
}

bool FcgiServer::listen(const std::string& socket_path, int backlog) {
  if (socket_path.empty()) {
    return false;
  }
  if (backlog < 1) {
    backlog = 128;
  }
  int fd = FCGX_OpenSocket(socket_path.c_str(), backlog);
  if (fd < 0) {
    return false;
  }
  listen_fd_ = fd;
  return true;
}

bool FcgiServer::is_listening() const {
  return listen_fd_ >= 0;
}

int FcgiServer::run() {
  if (!handler_) {
    std::cerr << "[ERROR] No request handler set\n";
    return 1;
  }

  if (listen_fd_ >= 0) {
    // ---- Direct socket mode ----
    // Accept FastCGI connections on our own Unix/TCP socket.
    // This is used when running standalone (e.g. via FCGI_SOCKET env var).

    // FCGX_Init() must be called before FCGX_Accept_r() — it sets the
    // libInitialized flag and performs platform-specific setup (OS_LibInit).
    // FCGX_InitRequest() alone does NOT mark the library as initialized.
    if (FCGX_Init() != 0) {
      std::cerr << "[ERROR] FCGX_Init failed\n";
      return 1;
    }

    if (FCGX_InitRequest(&request_, listen_fd_, 0) != 0) {
      std::cerr << "[ERROR] FCGX_InitRequest failed\n";
      return 1;
    }

    while (FCGX_Accept_r(&request_) >= 0) {
      // Make environment variables accessible via getenv() for read_request()
      environ = request_.envp;

      FcgiRequest req = read_request();
      DebugLog::http_request(req.request_method, req.request_uri);

      FcgiResponse resp = handler_(req);
      DebugLog::http_response(resp.status_code, resp.body);

      write_response(resp);

      FCGX_Finish_r(&request_);
    }
  } else {
    // ---- Spawn-fcgi mode ----
    // Accept FastCGI connections from stdin/stdout as set up by spawn-fcgi
    // or the web server.
    while (FCGI_Accept() >= 0) {
      FcgiRequest req = read_request();

      DebugLog::http_request(req.request_method, req.request_uri);

      FcgiResponse resp = handler_(req);

      DebugLog::http_response(resp.status_code, resp.body);

      write_response(resp);
    }
  }

  return 0;
}

void FcgiServer::shutdown() {
  shutdown_requested_.store(true, std::memory_order_relaxed);

  if (listen_fd_ >= 0) {
    // Step 1: shutdown(SHUT_RDWR) causes any worker blocked in accept()
    // on this fd to fail with EINVAL. This is reliable on Linux, unlike
    // close() which may leave accept() blocked indefinitely.
    ::shutdown(listen_fd_, SHUT_RDWR);

    // Step 2: Connect to unblock any workers that didn't respond to
    // shutdown(). Each accepted connection gets an immediate client EOF,
    // causing FCGX_Accept_r to return -1 and the worker to exit.
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0) {
      int wakeups = (num_workers_ > 0) ? num_workers_ : 64;
      for (int i = 0; i < wakeups; ++i) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0)
          break;
        int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
        if (ret == 0 || errno == EINPROGRESS) {
          // Connection queued — one worker will accept it
        } else {
          ::close(fd);
          break;
        }
        ::close(fd);
      }
    }

    // Step 3: Close the fd. By now all workers should have exited.
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

int FcgiServer::run_multithreaded(int num_threads) {
  if (!handler_) {
    std::cerr << "[ERROR] No request handler set\n";
    return 1;
  }

  if (listen_fd_ < 0) {
    std::cerr << "[ERROR] run_multithreaded() requires a listening socket (call listen() first)\n";
    return 1;
  }

  if (num_threads < 1) {
    num_threads = 1;
  }

  // FCGX_Init() must be called once before any FCGX_Accept_r().
  // It sets the libInitialized flag and performs platform-specific setup.
  if (FCGX_Init() != 0) {
    std::cerr << "[ERROR] FCGX_Init failed\n";
    return 1;
  }

  shutdown_requested_.store(false, std::memory_order_relaxed);
  num_workers_ = num_threads;

  // Spawn worker threads. Each thread creates its own FCGX_Request and
  // calls FCGX_Accept_r() on the shared listen socket. The OS serializes
  // accept() calls across threads — when one thread accepts a connection,
  // the next thread waiting on accept() gets the next connection.
  for (int i = 0; i < num_threads; ++i) {
    workers_.emplace_back([this]() {
      FCGX_Request request;
      if (FCGX_InitRequest(&request, listen_fd_, 0) != 0) {
        std::cerr << "[ERROR] FCGX_InitRequest failed in worker thread\n";
        return;
      }

      while (!shutdown_requested_.load(std::memory_order_relaxed)) {
        int rc = FCGX_Accept_r(&request);
        if (rc < 0) {
          // Accept failed — either shutdown (listen_fd_ closed) or error.
          break;
        }

        // Make environment variables accessible via getenv() for read_request()
        environ = request.envp;

        FcgiRequest req = read_request();
        DebugLog::http_request(req.request_method, req.request_uri);

        FcgiResponse resp = handler_(req);
        DebugLog::http_response(resp.status_code, resp.body);

        write_response_direct(request, resp);

        FCGX_Finish_r(&request);
      }
    });
  }

  // Wait for all worker threads to complete (triggered by shutdown()).
  for (auto& t : workers_) {
    t.join();
  }
  workers_.clear();

  return 0;
}

void FcgiServer::write_response_direct(FCGX_Request& request, const FcgiResponse& response) {
  // Build the full HTTP response as a single string.
  std::string output;
  output.reserve(256 + response.body.size());

  output += "Status: " + std::to_string(response.status_code) + "\r\n";
  output += "Content-Type: " + response.content_type + "; charset=utf-8\r\n";
  output += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  output += "\r\n";
  output += response.body;

  // Write to the FCGX request output stream (direct socket mode).
  FCGX_PutStr(output.data(), static_cast<int>(output.size()), request.out);
}

FcgiRequest FcgiServer::read_request() {
  FcgiRequest req;

  // Read FCGI environment variables
  auto get_env = [](const char* name) -> const char* {
    const char* val = getenv(name);
    return val ? val : "";
  };

  req.request_method = get_env("REQUEST_METHOD");
  req.request_uri = get_env("REQUEST_URI");
  req.query_string = get_env("QUERY_STRING");
  req.content_type = get_env("CONTENT_TYPE");
  req.script_name = get_env("SCRIPT_NAME");
  req.path_info = get_env("PATH_INFO");
  req.server_protocol = get_env("SERVER_PROTOCOL");

  // Read request body (for POST data, e.g., filter operations)
  if (req.request_method == "POST" || req.request_method == "PUT") {
    const char* content_length_str = getenv("CONTENT_LENGTH");
    if (content_length_str[0] != '\0') {
      int content_length = 0;
      auto [ptr, ec] = std::from_chars(
          content_length_str, content_length_str + std::strlen(content_length_str), content_length);
      if (ec != std::errc{} || content_length <= 0) {
        // Invalid or non-positive content length — skip body
        return req;
      }
      // Cap at maximum to prevent OOM on constrained systems
      if (content_length > kMaxContentLength) {
        std::cerr << "[WARN] CONTENT_LENGTH " << content_length << " exceeds maximum "
                  << kMaxContentLength << ", truncating\n";
        content_length = kMaxContentLength;
      }
      req.content.resize(static_cast<size_t>(content_length));
      // Read from FCGI stdin
      size_t total = 0;
      while (total < static_cast<size_t>(content_length)) {
        int n = FCGI_fread(req.content.data() + total, 1,
                           static_cast<int>(req.content.size() - total), stdin);
        if (n <= 0)
          break;
        total += static_cast<size_t>(n);
      }
      // Shrink to actual bytes read (in case of early EOF)
      req.content.resize(total);
    }
  }

  return req;
}

void FcgiServer::write_response(const FcgiResponse& response) {
  // Build the full HTTP response as a single string.
  std::string output;
  output.reserve(256 + response.body.size());

  output += "Status: " + std::to_string(response.status_code) + "\r\n";
  output += "Content-Type: " + response.content_type + "; charset=utf-8\r\n";
  output += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  output += "\r\n";
  output += response.body;

  if (listen_fd_ >= 0) {
    // Direct socket mode: write to FCGX request output stream.
    FCGX_PutStr(output.data(), static_cast<int>(output.size()), request_.out);
  } else {
    // Spawn-fcgi mode: write to FCGI stdout.
    // Note: FCGI_fwrite takes non-const void* (C API), but does not modify the buffer.
    // The const_cast is safe here as the library only reads from the buffer.
    FCGI_fwrite(const_cast<char*>(output.data()), 1, output.size(), stdout);
  }
}

}  // namespace cvknxd
