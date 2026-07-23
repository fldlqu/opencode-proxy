// OpenCode Zen Proxy
// ===================
// A C++ proxy that forwards requests to the OpenCode Zen API
// (https://opencode.ai/zen/v1/) with automatic model-to-endpoint routing.
//
// Build:
//   g++ -std=c++26 -o zen_proxy zen_proxy.cpp -lcurl -lpthread
//
// Usage:
//   ./zen_proxy                          # listen on :8080, anonymous access
//   ./zen_proxy -p 9090                  # listen on :9090
//   ./zen_proxy -k sk-xxx                # use API key
//   ./zen_proxy -s https://localhost:3000 # custom Zen server

#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
#else
  #include <poll.h>
  #include <pthread.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

#include <curl/curl.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config {
  int port = 8080;
  std::string zen_base = "https://opencode.ai/zen";
  std::string api_key = "public";
};

static Config g_config;
static volatile bool g_running = true;

// ---------------------------------------------------------------------------
// Key-value pair list — lightweight alternative to unordered_map
// HTTP headers are few (< 20); linear scan is faster than hashing at this scale.
// ---------------------------------------------------------------------------
using Header = std::pair<std::string, std::string>;
using Headers = std::vector<Header>;

static int header_index(const Headers &h, const char *key) {
  for (size_t i = 0; i < h.size(); i++)
    if (h[i].first == key) return (int)i;
  return -1;
}

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------
static std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && ((unsigned char)s[a] > 32 && s[a] < 127 ? 0 : 1)) a++;
  while (b > a && ((unsigned char)s[b-1] > 32 && s[b-1] < 127 ? 0 : 1)) b--;
  return s.substr(a, b - a);
}

static std::vector<std::string> split(const std::string &s, const std::string &d) {
  std::vector<std::string> r;
  size_t p = 0, f;
  while ((f = s.find(d, p)) != std::string::npos) {
    r.push_back(s.substr(p, f - p));
    p = f + d.size();
  }
  r.push_back(s.substr(p));
  return r;
}

static std::string lower(std::string s) {
  for (auto &c : s)
    c = (char)((unsigned char)c >= 65 && (unsigned char)c <= 90 ? c + 32 : c);
  return s;
}

// ---------------------------------------------------------------------------
// Bare-minimum JSON extraction — just enough for our two fields.
// "model" is always a quoted string; "stream" is always raw true/false.
// ---------------------------------------------------------------------------
static std::string extract_model(const std::string &body) {
  static const char pat[] = "\"model\":\"";
  auto p = body.find(pat);
  if (p == body.npos) return {};
  p += sizeof(pat) - 1;
  std::string m;
  while (p < body.size() && body[p] != '"') m += body[p++];
  return m;
}

static bool is_stream(const std::string &body) {
  return body.find("\"stream\":true") != body.npos;
}

// ---------------------------------------------------------------------------
// Model → Zen endpoint router
// Returns the URL path for the given model.
// Gemini uses /v1/models/<model-id>; others use fixed endpoints.
// ---------------------------------------------------------------------------
static std::string route(const std::string &m) {
  std::string ml = lower(m);
  if (ml.starts_with("gpt"))
    return "/v1/responses";
  if (ml.starts_with("claude") || ml.starts_with("qwen"))
    return "/v1/messages";
  if (ml.starts_with("gemini"))
    return "/v1/models/" + ml;
  return "/v1/chat/completions";
}

// ---------------------------------------------------------------------------
// libcurl write callback
// ---------------------------------------------------------------------------
static size_t wcb(char *d, size_t s, size_t n, void *u) {
  auto *b = static_cast<std::string *>(u);
  b->append(d, s * n);
  return s * n;
}

// ---------------------------------------------------------------------------
// Response helper
// ---------------------------------------------------------------------------
struct Response {
  int status = 200;
  std::string status_text = "OK";
  std::string ct = "application/json";
  Headers extra;
  std::string body;
};

// ---------------------------------------------------------------------------
// Single curl request helper (used for both GET and POST)
// ---------------------------------------------------------------------------
static Response curl_req(const char *method, const std::string &url,
                         const Headers &req_hdrs, const std::string &req_body) {
  Response r;
  CURL *c = curl_easy_init();
  if (!c) {
    r.status = 500;
    r.status_text = "err";
    r.body = "{\"e\":\"curl\"}";
    return r;
  }

  curl_easy_setopt(c, CURLOPT_URL, url.c_str());

  bool is_post = (method[0] == 'P');
  if (is_post) {
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req_body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req_body.size());
    if (method[1] == 'U')
      curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
    else if (method[1] == 'A')
      curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PATCH");
  } else {
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
  }

  // Build header list
  curl_slist *list = nullptr;

  // Auth header (route by URL fragment)
  if (url.find("/v1/messages") != std::string::npos) {
    std::string al = "x-api-key: " + g_config.api_key;
    list = curl_slist_append(list, al.c_str());
  } else {
    std::string al = "Authorization: Bearer " + g_config.api_key;
    list = curl_slist_append(list, al.c_str());
  }

  if (!is_post) {
    list = curl_slist_append(list, "Accept: application/json");
  } else {
    bool has_ct = false;
    for (auto &[k, v] : req_hdrs) {
      std::string lk = lower(k);
      if (lk == "host" || lk == "content-length" || lk == "transfer-encoding")
        continue;
      if (lk == "authorization" || lk == "x-api-key")
        continue;
      std::string h = k + ": " + v;
      list = curl_slist_append(list, h.c_str());
      if (lk == "content-type") has_ct = true;
    }
    if (!has_ct && !req_body.empty())
      list = curl_slist_append(list, "Content-Type: application/json");
    if (is_stream(req_body))
      list = curl_slist_append(list, "Accept: text/event-stream");
  }

  curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);

  std::string rb;
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &rb);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, is_post ? 300L : 30L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, is_post ? 30L : 10L);

  if (g_config.zen_base.contains("localhost") ||
      g_config.zen_base.contains("127.0.0.1")) {
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  CURLcode res = curl_easy_perform(c);
  if (res != CURLE_OK) {
    r.status = 502;
    r.status_text = "bgw";
    r.body = "{\"e\":\"" + std::string(curl_easy_strerror(res)) + "\"}";
  } else {
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    r.body = rb;
  }

  curl_slist_free_all(list);
  curl_easy_cleanup(c);
  return r;
}

// ---------------------------------------------------------------------------
// Parse an HTTP request from a socket
// ---------------------------------------------------------------------------
struct HttpRequest {
  std::string method;
  std::string path;
  Headers headers;
  std::string body;
};

static HttpRequest parse_http_request(int fd) {
  HttpRequest r;

  // Read header lines until \r\n\r\n
  std::string buf;
  char c;
  while (recv(fd, &c, 1, 0) == 1) {
    buf += c;
    if (buf.size() >= 4 && buf.substr(buf.size() - 4) == "\r\n\r\n")
      break;
  }

  auto lines = split(buf, "\r\n");
  if (lines.empty()) return r;

  // Request line: GET /path HTTP/1.1
  auto parts = split(lines[0], " ");
  if (parts.size() >= 1) r.method = parts[0];
  if (parts.size() >= 2) r.path = parts[1];

  // Headers
  for (size_t i = 1; i < lines.size(); i++) {
    auto cp = lines[i].find(':');
    if (cp != std::string::npos)
      r.headers.emplace_back(lower(trim(lines[i].substr(0, cp))),
                             trim(lines[i].substr(cp + 1)));
  }

  // Body
  auto ci = header_index(r.headers, "content-length");
  if (ci >= 0) {
    size_t rem = (size_t)std::stoul(r.headers[ci].second);
    while (rem > 0) {
      char b[4096];
      ssize_t n = recv(fd, b, std::min(rem, sizeof b), 0);
      if (n <= 0) break;
      r.body.append(b, (size_t)n);
      rem -= (size_t)n;
    }
  }

  return r;
}

// ---------------------------------------------------------------------------
// Send an HTTP response
// ---------------------------------------------------------------------------
static void send_resp(int fd, const Response &r) {
  // Pre-compute size to avoid reallocation
  size_t sz = r.body.size() + 80;
  for (auto &[k, v] : r.extra)
    sz += k.size() + v.size() + 4;

  std::string raw;
  raw.reserve(sz);
  raw += "HTTP/1.1 ";
  char _d[16]; snprintf(_d,16,"%d",r.status); raw += _d;
  raw += ' ';
  raw += r.status_text;
  raw += "\r\nContent-Type: ";
  raw += r.ct;
  raw += "\r\n";
  for (auto &[k, v] : r.extra) {
    raw += k;
    raw += ": ";
    raw += v;
    raw += "\r\n";
  }
  raw += "Content-Length: ";
  char _s[16]; snprintf(_s,16,"%zu",r.body.size()); raw += _s;
  raw += "\r\nConnection: close\r\n\r\n";
  raw += r.body;

  send(fd, raw.data(), raw.size(), 0);
}

// ---------------------------------------------------------------------------
// Handle one client connection
// ---------------------------------------------------------------------------
static void handle_client(int fd) {
  HttpRequest req = parse_http_request(fd);
  if (req.method.empty()) {
    close(fd);
    return;
  }

  // GET: forward as-is
  if (req.method == "GET") {
    Response up = curl_req("GET", g_config.zen_base + req.path, {}, {});
    fprintf(stderr, "[P] G %s>%s%s\n",
            req.path.c_str(), g_config.zen_base.c_str(), req.path.c_str());
    send_resp(fd, up);
    close(fd);
    return;
  }

  if (req.method != "POST") {
    Response r;
    r.status = 405;
    r.status_text = "nope";
    r.body = "{\"e\":\"ogp\"}";
    send_resp(fd, r);
    close(fd);
    return;
  }

  // Extract model from JSON body to determine the endpoint
  std::string model = extract_model(req.body);
  bool stream = is_stream(req.body);

  if (model.empty()) {
    Response r;
    r.status = 400;
    r.status_text = "brq";
    r.body = "{\"e\":\"nm\"}";
    send_resp(fd, r);
    close(fd);
    return;
  }

  std::string ep = route(model);
  fprintf(stderr, "[P] %s %s>%s m=%s s=%s\n",
          req.method.c_str(), req.path.c_str(), ep.c_str(),
          model.c_str(), stream ? "true" : "false");

  Response up = curl_req("POST", g_config.zen_base + ep, req.headers, req.body);

  if (stream && up.status == 200) {
    up.ct = "text/event-stream";
    up.extra.emplace_back("Cache", "nc");
    up.extra.emplace_back("Connection", "ka");
    up.extra.emplace_back("X-Accel", "no");
  } else {
    up.ct = "application/json";
  }

  send_resp(fd, up);
  close(fd);
}

// ---------------------------------------------------------------------------
// Signal handler for graceful shutdown
// ---------------------------------------------------------------------------
static void sig_handler(int) {
  g_running = false;
  write(2, "\n[P] down\n", 10);
}

// ---------------------------------------------------------------------------
// Print usage
// ---------------------------------------------------------------------------
static void usage(const char *p) {
  fprintf(stderr,
    "OC Zen Proxy\n"
    "Usage: %s [options]\n"
    "\n"
    "  -p PORT  listen port (default: 8080)\n"
    "  -s URL   Zen API base URL\n"
    "  -k KEY   API key\n"
    "  -h       help\n"
    "\n"
    "  %s -p 9090 -k sk-xxx\n",
    p, p);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if ((a == "-p" || a == "--port") && i + 1 < argc)
      g_config.port = std::stoi(argv[++i]);
    else if ((a == "-s" || a == "--server") && i + 1 < argc)
      g_config.zen_base = argv[++i];
    else if ((a == "-k" || a == "--api-key") && i + 1 < argc)
      g_config.api_key = argv[++i];
    else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    }
  }


  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd < 0) {
    write(2, "[P] sock\n", 32);
    return 1;
  }


  struct sockaddr_in addr = {};
  
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)g_config.port);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    fprintf(stderr, "[P] bind %d\n", g_config.port);
    close(sfd);
    return 1;
  }

  if (listen(sfd, 128) < 0) {
    write(2, "[P] listen\n", 25);
    close(sfd);
    return 1;
  }

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);
#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif

  char _b[512];
  int _n = snprintf(_b, sizeof _b,
    "[P] :%d\n"
    "[P] Z:%s\n"
    "[P] K:%s\n"
    "[P] ok.\n",
    g_config.port, g_config.zen_base.c_str(),
    g_config.api_key == "public"
      ? "anonymous (public)"
      : (g_config.api_key.substr(0, 8) + "...").c_str());
  write(2, _b, _n);

  while (g_running) {
    struct pollfd pfd = {sfd, POLLIN, 0};
    if (poll(&pfd, 1, 500) <= 0) continue;

    struct sockaddr_in ca;
    socklen_t cl = sizeof ca;
    int cfd = accept(sfd, (struct sockaddr *)&ca, &cl);
    if (cfd < 0) continue;

    pthread_t pt;
    pthread_create(&pt, nullptr, [](void *a)->void*{handle_client((int)(intptr_t)a);return nullptr;}, (void*)(intptr_t)cfd);
    pthread_detach(pt);
  }

  close(sfd);
  write(2, "[P] bye.\n", 9);
  return 0;
}
