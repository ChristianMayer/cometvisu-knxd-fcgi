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

// POSIX environment pointer — used by the no-arg read_request() overload
// via getenv().  The direct-socket and multithreaded paths pass the
// FCGX_Request::envp array directly instead, avoiding races on environ.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-redundant-declaration)
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

void FcgiServer::set_load_shed_semaphore(sem_t* sem) {
  load_shed_sem_ = sem;
}

void FcgiServer::set_concurrency_semaphore(sem_t* sem) {
  concurrency_sem_ = sem;
}

bool FcgiServer::listen(const std::string& socket_path, int backlog) {
  if (socket_path.empty()) {
    return false;
  }
  if (backlog < 1) {
    backlog = 128;
  }
  const int fd = FCGX_OpenSocket(socket_path.c_str(), backlog);
  if (fd < 0) {
    return false;
  }
  listen_fd_ = fd;
  return true;
}

bool FcgiServer::is_listening() const {
  return listen_fd_ >= 0;
}

void FcgiServer::close_listen_socket() {
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
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
      // Read request parameters directly from the FCGI envp array.
      // This avoids setting the global environ pointer, making it safe
      // for use in both single-threaded and multithreaded contexts.
      const FcgiRequest req = read_request(request_.envp);

      // ---- General concurrency limiting ----
      // Before processing any request, check if we have capacity.  If all
      // workers are busy (semaphore exhausted), return HTTP 503 immediately.
      // This prevents the listen backlog from filling up and causing the
      // reverse proxy to return 502 Bad Gateway.  Write requests are NOT
      // exempt — they complete quickly, so they won't hold the semaphore
      // for long, and this ensures we never accept more connections than
      // we can handle.
      bool acquired_conc = false;
      if (concurrency_sem_ != nullptr) {
        if (::sem_trywait(concurrency_sem_) == 0) {
          acquired_conc = true;
        } else {
          // All workers busy — return 503 immediately
          FcgiResponse busy_resp;
          busy_resp.status_code = 503;
          busy_resp.body = R"({"error":"all workers busy"})";
          write_response(busy_resp);
          FCGX_Finish_r(&request_);
          continue;
        }
      }

      // ---- Load shedding for long-poll requests ----
      // When a shared semaphore is configured, try to acquire it before
      // dispatching read (long-poll) requests.  If the semaphore is
      // exhausted (all workers busy with long-polls), return an empty
      // response immediately.  Write requests bypass this check so they
      // are always served, preventing head-of-line blocking from
      // long-poll reads.
      bool acquired_sem = false;
      if (load_shed_sem_ != nullptr && req.path() == "/r") {
        // sem_trywait returns 0 on success, -1 with EAGAIN if exhausted
        if (::sem_trywait(load_shed_sem_) == 0) {
          acquired_sem = true;
        } else {
          // All workers busy with long-polls — return empty immediately.
          // The client will retry, and this frees up the accept queue for
          // write requests which complete quickly.
          FcgiResponse empty_resp;
          empty_resp.status_code = 503;
          empty_resp.body = R"({"error":"too many long-poll requests"})";
          write_response(empty_resp);
          FCGX_Finish_r(&request_);
          continue;
        }
      }

      DebugLog::http_request(req.request_method, req.request_uri);

      const FcgiResponse resp = handler_(req);

      DebugLog::http_response(resp.status_code, resp.body);

      write_response(resp);

      FCGX_Finish_r(&request_);

      // Release the semaphores after the request completes
      if (acquired_sem) {
        ::sem_post(load_shed_sem_);
      }
      if (acquired_conc) {
        ::sem_post(concurrency_sem_);
      }
    }
  } else {
    // ---- Spawn-fcgi mode ----
    // Accept FastCGI connections from stdin/stdout as set up by spawn-fcgi
    // or the web server.
    while (FCGI_Accept() >= 0) {
      const FcgiRequest req = read_request();

      DebugLog::http_request(req.request_method, req.request_uri);

      const FcgiResponse resp = handler_(req);

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
    struct sockaddr_un addr {};
    socklen_t addr_len = sizeof(addr);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0) {
      const int wakeups = (num_workers_ > 0) ? num_workers_ : 64;
      for (int i = 0; i < wakeups; ++i) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) {
          break;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
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

  num_threads = std::max(num_threads, 1);

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
        const int rc = FCGX_Accept_r(&request);
        if (rc < 0) {
          // Accept failed — either shutdown (listen_fd_ closed) or error.
          break;
        }

        // Read request parameters directly from the FCGI envp array.
        // This avoids setting the global environ pointer, which would
        // race with other worker threads doing the same concurrently.
        const FcgiRequest req = read_request(request.envp);
        DebugLog::http_request(req.request_method, req.request_uri);

        const FcgiResponse resp = handler_(req);
        DebugLog::http_response(resp.status_code, resp.body);

        // Serialize FCGI output calls with a mutex.  libfcgi's internal
        // stream and connection state is not fully thread-safe, so
        // concurrent FCGX_PutStr / FCGX_Finish_r calls from different
        // worker threads can corrupt shared buffers.
        {
          const std::scoped_lock lock(fcgi_mutex_);
          write_response_direct(request, resp);
          FCGX_Finish_r(&request);
        }
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (content_length_str[0] != '\0') {
      int content_length = 0;
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const char* cl_end = content_length_str + std::strlen(content_length_str);
      auto [ptr, ec] = std::from_chars(content_length_str, cl_end, content_length);
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
      const auto max_len = static_cast<size_t>(content_length);
      req.content.resize(max_len);
      // Read from FCGI stdin
      size_t total = 0;
      while (total < max_len) {
        const size_t remaining = max_len - total;
        if (remaining == 0) {
          break;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const size_t nread = FCGI_fread(req.content.data() + total, 1, remaining, stdin);
        if (nread == 0) {
          break;
        }
        total += nread;
      }
      // Shrink to actual bytes read (in case of early EOF)
      req.content.resize(total);
    }
  }

  return req;
}

FcgiRequest FcgiServer::read_request(char** envp) {
  FcgiRequest req;

  if (envp == nullptr) {
    return req;
  }

  // Helper: find a value by name in a null-terminated envp array.
  // Each entry is "KEY=VALUE". Returns pointer to VALUE part, or nullptr.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  auto get_env = [](char** env_array, const char* name) -> const char* {
    if (env_array == nullptr || name == nullptr) {
      return nullptr;
    }
    const size_t name_len = std::strlen(name);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    for (const char* const* e = const_cast<const char* const*>(env_array); *e != nullptr; ++e) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      if (std::strncmp(*e, name, name_len) == 0 && (*e)[name_len] == '=') {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return *e + name_len + 1;
      }
    }
    return nullptr;
  };

  auto get_env_or_empty = [&](const char* name) -> const char* {
    const char* val = get_env(envp, name);
    return val ? val : "";
  };

  req.request_method = get_env_or_empty("REQUEST_METHOD");
  req.request_uri = get_env_or_empty("REQUEST_URI");
  req.query_string = get_env_or_empty("QUERY_STRING");
  req.content_type = get_env_or_empty("CONTENT_TYPE");
  req.script_name = get_env_or_empty("SCRIPT_NAME");
  req.path_info = get_env_or_empty("PATH_INFO");
  req.server_protocol = get_env_or_empty("SERVER_PROTOCOL");

  // Read request body (for POST data, e.g., filter operations)
  if (req.request_method == "POST" || req.request_method == "PUT") {
    const char* content_length_str = get_env(envp, "CONTENT_LENGTH");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (content_length_str != nullptr && content_length_str[0] != '\0') {
      int content_length = 0;
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const char* cl_end = content_length_str + std::strlen(content_length_str);
      auto [ptr, ec] = std::from_chars(content_length_str, cl_end, content_length);
      if (ec != std::errc{} || content_length <= 0) {
        return req;
      }
      if (content_length > kMaxContentLength) {
        std::cerr << "[WARN] CONTENT_LENGTH " << content_length << " exceeds maximum "
                  << kMaxContentLength << ", truncating\n";
        content_length = kMaxContentLength;
      }
      const auto max_len = static_cast<size_t>(content_length);
      req.content.resize(max_len);
      size_t total = 0;
      while (total < max_len) {
        const size_t remaining = max_len - total;
        if (remaining == 0) {
          break;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const size_t nread = FCGI_fread(req.content.data() + total, 1, remaining, stdin);
        if (nread == 0) {
          break;
        }
        total += nread;
      }
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
