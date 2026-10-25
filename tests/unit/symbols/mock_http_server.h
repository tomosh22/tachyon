// Tiny HTTP/1.1 server on 127.0.0.1 (random port) for symbol server tests: serves a directory, with
// programmable 5xx-then-200, slow responses and truncated bodies. One thread per connection (so the
// concurrency tests can observe overlapping requests); Connection: close.
#pragma once

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 4127 4548 4244)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma warning(pop)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tachyon::symbols::testutil {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kBadSocket = INVALID_SOCKET;
inline void closeSocket(SocketHandle s) {
  closesocket(s);
}
#else
using SocketHandle = int;
inline constexpr SocketHandle kBadSocket = -1;
inline void closeSocket(SocketHandle s) {
  ::close(s);
}
#endif

class MockHttpServer {
 public:
  explicit MockHttpServer(std::filesystem::path root) : m_root(std::move(root)) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (m_listen == kBadSocket || ::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(m_listen, 64) != 0) {
      return;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
    m_port = ntohs(addr.sin_port);
    m_acceptor = std::thread([this] { acceptLoop(); });
  }

  ~MockHttpServer() {
    m_stop = true;
    if (m_acceptor.joinable()) m_acceptor.join();
    {
      std::lock_guard lock(m_workersMutex);
      for (std::thread& t : m_workers) {
        if (t.joinable()) t.join();
      }
    }
    if (m_listen != kBadSocket) closeSocket(m_listen);
#ifdef _WIN32
    WSACleanup();
#endif
  }

  MockHttpServer(const MockHttpServer&) = delete;
  MockHttpServer& operator=(const MockHttpServer&) = delete;

  bool ok() const { return m_port != 0; }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(m_port); }

  // Next `count` requests for `path` ("/a/b") answer `status` (then normal service resumes).
  void failNext(const std::string& path, int count, int status = 500) {
    std::lock_guard lock(m_mutex);
    m_failures[path] = {count, status};
  }
  // Every request for `path` waits `ms` before answering (0 clears).
  void delay(const std::string& path, int ms) {
    std::lock_guard lock(m_mutex);
    m_delays[path] = ms;
  }
  // Requests for `path` answer 302 with this Location header.
  void redirect(const std::string& path, const std::string& location) {
    std::lock_guard lock(m_mutex);
    m_redirects[path] = location;
  }
  // Requests for `path` announce the full length but send only half of the body.
  void truncate(const std::string& path) {
    std::lock_guard lock(m_mutex);
    m_truncated.push_back(path);
  }
  // Number of requests seen for `path`.
  int requestCount(const std::string& path) const {
    std::lock_guard lock(m_mutex);
    auto it = m_counts.find(path);
    return it == m_counts.end() ? 0 : it->second;
  }
  int totalRequests() const {
    std::lock_guard lock(m_mutex);
    int n = 0;
    for (const auto& kv : m_counts) n += kv.second;
    return n;
  }
  int maxConcurrent() const { return m_maxConcurrent.load(); }
  // Last "Authorization" header seen (empty if none).
  std::string lastAuthorization() const {
    std::lock_guard lock(m_mutex);
    return m_lastAuth;
  }

 private:
  void acceptLoop() {
    while (!m_stop) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(m_listen, &set);
      timeval tv{0, 20000};
      if (::select(static_cast<int>(m_listen) + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
      SocketHandle c = ::accept(m_listen, nullptr, nullptr);
      if (c == kBadSocket) continue;
      std::lock_guard lock(m_workersMutex);
      m_workers.emplace_back([this, c] { serve(c); });
    }
  }

  static void sendAll(SocketHandle s, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
#ifdef _WIN32
      const int w = ::send(s, data + off, static_cast<int>(n - off), 0);
#else
      const auto w = ::send(s, data + off, n - off, MSG_NOSIGNAL);
#endif
      if (w <= 0) return;
      off += static_cast<size_t>(w);
    }
  }

  static std::string percentDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '%' && i + 2 < in.size() + 0 && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
          std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
        out += static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16));
        i += 2;
      } else {
        out += in[i];
      }
    }
    return out;
  }

  void respond(SocketHandle s, int status, const std::string& body, size_t announce, size_t sendBytes) {
    const char* reason = status == 200 ? "OK" : (status == 404 ? "Not Found" : "Error");
    std::ostringstream head;
    head << "HTTP/1.1 " << status << " " << reason << "\r\nContent-Length: " << announce
         << "\r\nConnection: close\r\n\r\n";
    const std::string h = head.str();
    sendAll(s, h.data(), h.size());
    sendAll(s, body.data(), sendBytes);
  }

  void serve(SocketHandle s) {
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
      const auto n = ::recv(s, buf, static_cast<int>(sizeof(buf)), 0);
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
    }
    std::string path;
    const size_t sp1 = req.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
    if (sp2 != std::string::npos) path = req.substr(sp1 + 1, sp2 - sp1 - 1);

    int delayMs = 0;
    int failStatus = 0;
    std::string location;
    bool truncated = false;
    {
      std::lock_guard lock(m_mutex);
      ++m_counts[path];
      const size_t auth = req.find("Authorization:");
      if (auth != std::string::npos) m_lastAuth = req.substr(auth, req.find("\r\n", auth) - auth);
      if (auto it = m_delays.find(path); it != m_delays.end()) delayMs = it->second;
      if (auto it = m_failures.find(path); it != m_failures.end() && it->second.first > 0) {
        --it->second.first;
        failStatus = it->second.second;
      }
      if (auto it = m_redirects.find(path); it != m_redirects.end()) location = it->second;
      for (const std::string& t : m_truncated) truncated = truncated || t == path;
    }
    const int now = ++m_concurrent;
    int prev = m_maxConcurrent.load();
    while (now > prev && !m_maxConcurrent.compare_exchange_weak(prev, now)) {
    }
    for (int waited = 0; waited < delayMs && !m_stop; waited += 10) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Stop counting before the response goes out: the client can only finish (and start its next request)
    // after receiving it, so the count never includes a request whose client has already moved on.
    --m_concurrent;
    if (!location.empty()) {
      const std::string head = "HTTP/1.1 302 Found\r\nLocation: " + location +
                               "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      sendAll(s, head.data(), head.size());
    } else if (failStatus != 0) {
      respond(s, failStatus, "server error", 12, 12);
    } else {
      std::filesystem::path file = m_root;
      std::string rel = percentDecode(path);
      if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
      file /= rel;
      std::error_code ec;
      if (rel.empty() || rel.find("..") != std::string::npos || !std::filesystem::is_regular_file(file, ec)) {
        respond(s, 404, "not found", 9, 9);
      } else {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string body = ss.str();
        respond(s, 200, body, body.size(), truncated ? body.size() / 2 : body.size());
      }
    }
    closeSocket(s);
  }

  std::filesystem::path m_root;
  SocketHandle m_listen = kBadSocket;
  uint16_t m_port = 0;
  std::atomic<bool> m_stop{false};
  std::thread m_acceptor;
  std::mutex m_workersMutex;
  std::vector<std::thread> m_workers;

  mutable std::mutex m_mutex;
  std::map<std::string, int> m_counts;
  std::map<std::string, std::pair<int, int>> m_failures;
  std::map<std::string, int> m_delays;
  std::vector<std::string> m_truncated;
  std::map<std::string, std::string> m_redirects;
  std::string m_lastAuth;
  std::atomic<int> m_concurrent{0};
  std::atomic<int> m_maxConcurrent{0};
};

}  // namespace tachyon::symbols::testutil
