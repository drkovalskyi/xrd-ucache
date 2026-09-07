#include "Publish.h"

#include "vendor/sha256.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

extern char** environ;

namespace ucache {

// ============================================================================
// JSON
// ============================================================================

Json Json::boolean(bool v) {
  Json j;
  j.type = Type::Bool;
  j.b = v;
  return j;
}
Json Json::numberText(const std::string& text) {
  Json j;
  j.type = Type::Number;
  j.s = text;
  return j;
}
Json Json::integer(long long v) { return numberText(std::to_string(v)); }
Json Json::real(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return numberText(buf);
}
Json Json::string(const std::string& v) {
  Json j;
  j.type = Type::String;
  j.s = v;
  return j;
}
Json Json::array() {
  Json j;
  j.type = Type::Array;
  return j;
}
Json Json::object() {
  Json j;
  j.type = Type::Object;
  return j;
}

const Json* Json::get(const std::string& key) const {
  if (type != Type::Object)
    return nullptr;
  for (const auto& kv : obj)
    if (kv.first == key)
      return &kv.second;
  return nullptr;
}
Json* Json::get(const std::string& key) {
  if (type != Type::Object)
    return nullptr;
  for (auto& kv : obj)
    if (kv.first == key)
      return &kv.second;
  return nullptr;
}
void Json::set(const std::string& key, Json value) {
  if (type != Type::Object) {
    type = Type::Object;
    obj.clear();
  }
  if (Json* existing = get(key)) {
    *existing = std::move(value);
    return;
  }
  obj.emplace_back(key, std::move(value));
}
void Json::erase(const std::string& key) {
  obj.erase(std::remove_if(obj.begin(), obj.end(),
                           [&](const std::pair<std::string, Json>& kv) { return kv.first == key; }),
            obj.end());
}
std::string Json::str(const std::string& key, const std::string& dflt) const {
  const Json* v = get(key);
  return v && v->type == Type::String ? v->s : dflt;
}
double Json::num(const std::string& key, double dflt) const {
  const Json* v = get(key);
  if (!v || v->type != Type::Number)
    return dflt;
  return std::strtod(v->s.c_str(), nullptr);
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  return out;
}

namespace {

// Nesting the parser will follow before giving up. The payloads it reads are
// two levels deep; a hostile record store line or reply must fail, not
// overflow the stack.
constexpr int kMaxDepth = 64;

struct Parser {
  const std::string& t;
  size_t i = 0;
  int depth = 0;
  std::string err;

  explicit Parser(const std::string& text) : t(text) {}

  struct Deeper {
    int& d;
    explicit Deeper(int& x) : d(x) { ++d; }
    ~Deeper() { --d; }
  };

  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r'))
      ++i;
  }
  bool fail(const char* what) {
    if (err.empty())
      err = std::string(what) + " at offset " + std::to_string(i);
    return false;
  }
  static void utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(uint32_t& v) {
    if (i + 4 > t.size())
      return fail("truncated \\u escape");
    v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = t[i++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<uint32_t>(c - 'A' + 10);
      else
        return fail("bad \\u escape");
    }
    return true;
  }
  bool string(std::string& out) {
    if (i >= t.size() || t[i] != '"')
      return fail("expected string");
    ++i;
    while (i < t.size()) {
      const char c = t[i++];
      if (c == '"')
        return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= t.size())
        return fail("truncated escape");
      const char e = t[i++];
      switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        uint32_t cp = 0;
        if (!hex4(cp))
          return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // A high surrogate needs a low one right behind it. Anything else is
          // a broken pair: the high half becomes U+FFFD and whatever follows
          // is read on its own, so nothing after it is lost.
          if (i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
            const size_t save = i;
            i += 2;
            uint32_t lo = 0;
            if (!hex4(lo))
              return false;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              cp = 0xFFFD;
              i = save;
            }
          } else {
            cp = 0xFFFD;
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD; // a low surrogate on its own
        }
        utf8(out, cp);
        break;
      }
      default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }
  bool value(Json& out) {
    ws();
    if (i >= t.size())
      return fail("unexpected end");
    const char c = t[i];
    if ((c == '{' || c == '[') && depth >= kMaxDepth)
      return fail("nesting too deep");
    if (c == '{') {
      Deeper deeper(depth);
      ++i;
      out = Json::object();
      ws();
      if (i < t.size() && t[i] == '}') {
        ++i;
        return true;
      }
      for (;;) {
        ws();
        std::string key;
        if (!string(key))
          return false;
        ws();
        if (i >= t.size() || t[i] != ':')
          return fail("expected ':'");
        ++i;
        Json v;
        if (!value(v))
          return false;
        out.obj.emplace_back(std::move(key), std::move(v));
        ws();
        if (i < t.size() && t[i] == ',') {
          ++i;
          continue;
        }
        if (i < t.size() && t[i] == '}') {
          ++i;
          return true;
        }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      Deeper deeper(depth);
      ++i;
      out = Json::array();
      ws();
      if (i < t.size() && t[i] == ']') {
        ++i;
        return true;
      }
      for (;;) {
        Json v;
        if (!value(v))
          return false;
        out.arr.push_back(std::move(v));
        ws();
        if (i < t.size() && t[i] == ',') {
          ++i;
          continue;
        }
        if (i < t.size() && t[i] == ']') {
          ++i;
          return true;
        }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      std::string s;
      if (!string(s))
        return false;
      out = Json::string(s);
      return true;
    }
    if (t.compare(i, 4, "true") == 0) {
      i += 4;
      out = Json::boolean(true);
      return true;
    }
    if (t.compare(i, 5, "false") == 0) {
      i += 5;
      out = Json::boolean(false);
      return true;
    }
    if (t.compare(i, 4, "null") == 0) {
      i += 4;
      out = Json::null();
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const size_t start = i;
      if (t[i] == '-')
        ++i;
      const size_t intStart = i;
      while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])))
        ++i;
      if (i == intStart)
        return fail("bad number");
      if (t[intStart] == '0' && i - intStart > 1)
        return fail("leading zero"); // JSON forbids 01
      if (i < t.size() && t[i] == '.') {
        ++i;
        const size_t fracStart = i;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])))
          ++i;
        if (i == fracStart)
          return fail("bad fraction"); // JSON forbids 1.
      }
      if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < t.size() && (t[i] == '+' || t[i] == '-'))
          ++i;
        bool ed = false;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
          ++i;
          ed = true;
        }
        if (!ed)
          return fail("bad exponent");
      }
      out = Json::numberText(t.substr(start, i - start));
      return true;
    }
    return fail("unexpected character");
  }
};

void dumpInto(const Json& j, std::string& out, int indent, int level) {
  auto nl = [&](int lvl) {
    if (indent < 0)
      return;
    out += '\n';
    out.append(static_cast<size_t>(indent * lvl), ' ');
  };
  switch (j.type) {
  case Json::Type::Null: out += "null"; break;
  case Json::Type::Bool: out += j.b ? "true" : "false"; break;
  case Json::Type::Number: out += j.s; break;
  case Json::Type::String:
    out += '"';
    out += jsonEscape(j.s);
    out += '"';
    break;
  case Json::Type::Array: {
    out += '[';
    // Arrays of scalars stay on one line even when pretty: a 40-point curve
    // one number per line is not more readable.
    const bool scalars = std::all_of(j.arr.begin(), j.arr.end(), [](const Json& e) {
      return e.type != Json::Type::Array && e.type != Json::Type::Object;
    });
    for (size_t k = 0; k < j.arr.size(); ++k) {
      if (k)
        out += ',';
      if (!scalars)
        nl(level + 1);
      dumpInto(j.arr[k], out, indent, level + 1);
    }
    if (!scalars && !j.arr.empty())
      nl(level);
    out += ']';
    break;
  }
  case Json::Type::Object:
    out += '{';
    for (size_t k = 0; k < j.obj.size(); ++k) {
      if (k)
        out += ',';
      nl(level + 1);
      out += '"';
      out += jsonEscape(j.obj[k].first);
      out += indent < 0 ? "\":" : "\": ";
      dumpInto(j.obj[k].second, out, indent, level + 1);
    }
    if (!j.obj.empty())
      nl(level);
    out += '}';
    break;
  }
}

} // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
  Parser p(text);
  Json v;
  if (!p.value(v)) {
    if (err)
      *err = p.err;
    return false;
  }
  p.ws();
  if (p.i != text.size()) {
    if (err)
      *err = "trailing characters at offset " + std::to_string(p.i);
    return false;
  }
  out = std::move(v);
  return true;
}

std::string Json::dump(int indent) const {
  std::string out;
  dumpInto(*this, out, indent, 0);
  return out;
}

// ============================================================================
// small file helpers
// ============================================================================

namespace {

std::string homeDir() {
  if (const char* h = ::getenv("HOME"); h && *h)
    return h;
  if (struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir)
    return pw->pw_dir;
  return ".";
}

// mkdir -p for the directories above a file. 0 or errno.
int makeParents(const std::string& file, mode_t mode) {
  const size_t slash = file.rfind('/');
  if (slash == std::string::npos || slash == 0)
    return 0;
  const std::string dir = file.substr(0, slash);
  struct ::stat st;
  if (::stat(dir.c_str(), &st) == 0)
    return S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
  if (int rc = makeParents(dir, mode))
    return rc;
  if (::mkdir(dir.c_str(), mode) != 0 && errno != EEXIST)
    return errno;
  return 0;
}

bool readWholeFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// Write to a sibling temp file, then rename: a reader never sees a half file.
int writeFileAtomic(const std::string& path, const std::string& content, mode_t mode) {
  if (int rc = makeParents(path, 0700))
    return rc;
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0)
    return errno;
  size_t off = 0;
  while (off < content.size()) {
    const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n < 0) {
      const int e = errno;
      ::close(fd);
      ::unlink(tmp.c_str());
      return e;
    }
    off += static_cast<size_t>(n);
  }
  ::fchmod(fd, mode); // umask may have narrowed it; the caller's mode is the contract
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    const int e = errno;
    ::unlink(tmp.c_str());
    return e;
  }
  return 0;
}

// Create a file, and fail rather than replace one that exists. Returns 0 when
// this call created it, EEXIST when somebody else got there first, else errno.
// The identity and the instance id are both "one per machine, minted once" and
// a rename-over would silently discard the winner's — with the identity that
// means discarding a salt, and a discarded salt cannot be recovered: the pages
// its hashes lead to can never be reached again.
int createFileExclusive(const std::string& path, const std::string& content, mode_t mode) {
  if (int rc = makeParents(path, 0700))
    return rc;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
  if (fd < 0)
    return errno;
  size_t off = 0;
  while (off < content.size()) {
    const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n < 0) {
      const int e = errno;
      ::close(fd);
      ::unlink(path.c_str());
      return e;
    }
    off += static_cast<size_t>(n);
  }
  ::fchmod(fd, mode); // umask may have narrowed it; the caller's mode is the contract
  ::close(fd);
  return 0;
}

int appendLine(const std::string& path, const std::string& line, mode_t mode) {
  if (int rc = makeParents(path, 0700))
    return rc;
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, mode);
  if (fd < 0)
    return errno;
  const std::string text = line + "\n";
  size_t off = 0;
  while (off < text.size()) {
    const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
    if (n < 0) {
      const int e = errno;
      ::close(fd);
      return e;
    }
    off += static_cast<size_t>(n);
  }
  ::close(fd);
  return 0;
}

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string toLower(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool isHex(const std::string& s, size_t n) {
  if (s.size() != n)
    return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  return true;
}

std::string randomBytesHex(size_t n, unsigned char* raw = nullptr) {
  std::vector<unsigned char> buf(n);
  bool ok = false;
  if (const int fd = ::open("/dev/urandom", O_RDONLY); fd >= 0) {
    size_t off = 0;
    while (off < n) {
      const ssize_t r = ::read(fd, buf.data() + off, n - off);
      if (r <= 0)
        break;
      off += static_cast<size_t>(r);
    }
    ::close(fd);
    ok = off == n;
  }
  if (!ok) {
    std::random_device rd;
    for (auto& b : buf)
      b = static_cast<unsigned char>(rd() & 0xFF);
  }
  if (raw)
    std::memcpy(raw, buf.data(), n);
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (unsigned char b : buf) {
    out += hex[b >> 4];
    out += hex[b & 0xF];
  }
  return out;
}

std::string xdgOr(const char* env, const std::string& fallback) {
  if (const char* v = ::getenv(env); v && *v)
    return v;
  return fallback;
}

} // namespace

// ============================================================================
// identity
// ============================================================================

bool looksLikeUuid(const std::string& s) {
  if (s.size() != 36)
    return false;
  for (size_t k = 0; k < s.size(); ++k) {
    const char c = s[k];
    if (k == 8 || k == 13 || k == 18 || k == 23) {
      if (c != '-')
        return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string newUuid4() {
  unsigned char raw[16];
  randomBytesHex(16, raw);
  raw[6] = static_cast<unsigned char>(0x40 | (raw[6] & 0x0F)); // version 4
  raw[8] = static_cast<unsigned char>(0x80 | (raw[8] & 0x3F)); // RFC 4122 variant
  char buf[40];
  std::snprintf(buf, sizeof buf,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", raw[0],
                raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
                raw[11], raw[12], raw[13], raw[14], raw[15]);
  return buf;
}

bool parseIdentity(const std::string& text, Identity& out) {
  const std::string t = trim(text);
  static const std::string prefix = "ucache-id:";
  if (t.compare(0, prefix.size(), prefix) != 0)
    return false;
  const std::string rest = t.substr(prefix.size());
  const size_t colon = rest.find(':');
  if (colon == std::string::npos)
    return false;
  const std::string owner = rest.substr(0, colon), salt = rest.substr(colon + 1);
  if (!looksLikeUuid(owner) || !isHex(salt, 32))
    return false;
  out.owner = owner;
  out.salt = salt;
  return true;
}

Identity newIdentity() {
  Identity id;
  id.owner = newUuid4();
  id.salt = randomBytesHex(16);
  return id;
}

std::string identityPath() {
  if (const char* p = ::getenv("UCACHE_IDENTITY_FILE"); p && *p)
    return p;
  return xdgOr("XDG_CONFIG_HOME", homeDir() + "/.config") + "/ucache/identity";
}

std::optional<Identity> loadIdentity(std::string* err) {
  std::string text;
  if (!readWholeFile(identityPath(), text)) {
    if (err)
      *err = errno == ENOENT ? "" : std::strerror(errno);
    return std::nullopt;
  }
  Identity id;
  if (!parseIdentity(text, id)) {
    if (err)
      *err = "not an identity string (expected ucache-id:<owner>:<salt>)";
    return std::nullopt;
  }
  return id;
}

int saveIdentity(const Identity& id, std::string* err) {
  const int rc = writeFileAtomic(identityPath(), id.text() + "\n", 0600);
  if (rc && err)
    *err = std::strerror(rc);
  return rc;
}

std::optional<Identity> ensureIdentity(bool create, bool* created) {
  if (created)
    *created = false;
  std::string err;
  if (auto id = loadIdentity(&err))
    return id;
  if (!err.empty()) {
    std::fprintf(stderr,
                 "publish: %s is unreadable (%s) — records go out without an owner; fix or "
                 "replace it with `ucache identity --new`\n",
                 identityPath().c_str(), err.c_str());
    return std::nullopt;
  }
  if (!create)
    return std::nullopt;
  Identity id = newIdentity();
  const int rc = createFileExclusive(identityPath(), id.text() + "\n", 0600);
  if (rc == EEXIST) {
    // Another publish minted one between our read and our write. That one is
    // the machine's identity: keeping ours would open a second owner page and
    // throw away the salt of one of them, and the records already sent under
    // the discarded salt could never be found again.
    if (auto other = loadIdentity(&err))
      return other;
    std::fprintf(stderr, "publish: %s appeared but cannot be read (%s) — records go out without an owner\n",
                 identityPath().c_str(), err.empty() ? "unreadable" : err.c_str());
    return std::nullopt;
  }
  if (rc) {
    std::fprintf(stderr,
                 "publish: could not write %s (%s) — this record goes out without an owner, so "
                 "it will not appear on an owner page\n",
                 identityPath().c_str(), std::strerror(rc));
    return std::nullopt;
  }
  if (created)
    *created = true;
  return id;
}

// ============================================================================
// hashes and path normalization
// ============================================================================

std::string lowerTrim(const std::string& s) { return toLower(trim(s)); }

std::string saltedHash(const std::string& salt, const std::string& value) {
  std::string in = salt;
  in += '\0';
  in += value;
  return sha256Hex(in).substr(0, 32);
}

std::string machineHash(const std::string& salt, const std::string& host) {
  return saltedHash(salt, "machine:" + lowerTrim(host));
}

std::string locationHash(const std::string& salt, const std::string& host, const std::string& path) {
  std::string v = "location:" + lowerTrim(host);
  v += '\0';
  v += normPath(path);
  return saltedHash(salt, v);
}

std::string volumeHash(const std::string& salt, const std::string& host, const std::string& mountpoint) {
  std::string v = "volume:" + lowerTrim(host);
  v += '\0';
  v += normPath(mountpoint);
  return saltedHash(salt, v);
}

// Python's posixpath.normpath, including its two oddities: an empty path is
// ".", and exactly two leading slashes are kept (POSIX allows them to mean
// something) while three or more collapse to one.
std::string normPath(const std::string& path) {
  if (path.empty())
    return ".";
  int initialSlashes = 0;
  if (path[0] == '/') {
    initialSlashes = 1;
    if (path.compare(0, 2, "//") == 0 && path.compare(0, 3, "///") != 0)
      initialSlashes = 2;
  }
  std::vector<std::string> comps;
  size_t at = 0;
  while (at <= path.size()) {
    const size_t slash = path.find('/', at);
    const std::string comp = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
    if (!comp.empty() && comp != ".") {
      if (comp != ".." || (initialSlashes == 0 && comps.empty()) ||
          (!comps.empty() && comps.back() == ".."))
        comps.push_back(comp);
      else if (!comps.empty())
        comps.pop_back();
    }
    if (slash == std::string::npos)
      break;
    at = slash + 1;
  }
  std::string out(static_cast<size_t>(initialSlashes), '/');
  for (size_t k = 0; k < comps.size(); ++k) {
    if (k)
      out += '/';
    out += comps[k];
  }
  return out.empty() ? "." : out;
}

std::string hostName() {
  char host[256] = "";
  if (::gethostname(host, sizeof host - 1) != 0)
    return "";
  host[sizeof host - 1] = '\0';
  return host;
}

// ============================================================================
// field-level redaction
// ============================================================================

namespace {

// shlex.split for the shapes a recorded command line has: whitespace splits,
// quotes group, a backslash escapes the next character outside single quotes.
std::vector<std::string> splitCmd(const std::string& cmd) {
  std::vector<std::string> out;
  std::string cur;
  bool have = false;
  char quote = 0;
  for (size_t k = 0; k < cmd.size(); ++k) {
    const char c = cmd[k];
    if (quote) {
      if (c == quote) {
        quote = 0;
      } else if (c == '\\' && quote == '"' && k + 1 < cmd.size()) {
        cur += cmd[++k];
      } else {
        cur += c;
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      have = true;
    } else if (c == '\\' && k + 1 < cmd.size()) {
      cur += cmd[++k];
      have = true;
    } else if (c == ' ' || c == '\t' || c == '\n') {
      if (have)
        out.push_back(cur);
      cur.clear();
      have = false;
    } else {
      cur += c;
      have = true;
    }
  }
  if (have)
    out.push_back(cur);
  return out;
}

bool namesAPlace(const std::string& tok) { return tok.find('/') != std::string::npos || (!tok.empty() && tok[0] == '~'); }

} // namespace

std::string normalizeCmd(const std::string& cmd) {
  const std::vector<std::string> toks = splitCmd(cmd);
  std::string out;
  bool skip = false;
  for (size_t k = 0; k < toks.size(); ++k) {
    const std::string& t = toks[k];
    if (skip) {
      skip = false;
      continue;
    }
    std::string piece;
    if (k == 0) {
      const size_t slash = t.rfind('/');
      const std::string base = slash == std::string::npos ? t : t.substr(slash + 1);
      piece = base.compare(0, 6, "ucache") == 0 ? "ucache" : "<exe>";
    } else if (t == "--log") {
      skip = true;
      continue;
    } else if (t.compare(0, 6, "--log=") == 0) {
      continue;
    } else if (!t.empty() && t[0] == '-' && t.find('=') != std::string::npos) {
      const size_t eq = t.find('=');
      const std::string val = t.substr(eq + 1);
      piece = namesAPlace(val) ? t.substr(0, eq) + "=<path>" : t;
    } else if (namesAPlace(t)) {
      piece = "<path>";
    } else {
      piece = t;
    }
    if (!out.empty())
      out += ' ';
    out += piece;
  }
  return out;
}

std::string registrableDomain(const std::string& hostIn) {
  std::string host = lowerTrim(hostIn);
  while (!host.empty() && host.back() == '.')
    host.pop_back();
  if (host.empty())
    return "unknown";
  const bool numeric = host.find_first_not_of("0123456789.") == std::string::npos;
  const bool ipv6ish = host.find(':') != std::string::npos && host.find('.') == std::string::npos;
  if (numeric || ipv6ish)
    return "unknown";
  std::vector<std::string> labels;
  size_t at = 0;
  for (;;) {
    const size_t dot = host.find('.', at);
    labels.push_back(host.substr(at, dot == std::string::npos ? std::string::npos : dot - at));
    if (dot == std::string::npos)
      break;
    at = dot + 1;
  }
  static const char* const second[] = {"ac", "co", "edu", "gov", "org", "com", "net", "or", "ne", "go"};
  const size_t n = labels.size();
  if (n >= 3 && labels[n - 1].size() == 2) {
    for (const char* s : second)
      if (labels[n - 2] == s)
        return labels[n - 3] + "." + labels[n - 2] + "." + labels[n - 1];
  }
  if (n >= 2)
    return labels[n - 2] + "." + labels[n - 1];
  return host;
}

// Mirrors the service: `urlsplit(url if "//" in url else "root://" + url)`,
// then `scheme://<registrable domain of hostname>`. A bare host gets the root
// scheme; a path with no authority ("/data/x", "host//x") has no hostname at
// all and coarsens to `unknown`, which is the point — it named a place.
std::string coarsenRootUrl(const std::string& urlIn) {
  std::string url = trim(urlIn);
  std::string scheme = "root";
  std::string rest;
  if (const size_t p = url.find("://"); p != std::string::npos) {
    scheme = toLower(url.substr(0, p));
    rest = url.substr(p + 3);
  } else if (url.compare(0, 2, "//") == 0) {
    rest = url.substr(2);
  } else if (url.find("//") != std::string::npos) {
    return "root://unknown"; // urlsplit sees a path, no netloc
  } else {
    rest = url;
  }
  const size_t hostEnd = rest.find_first_of("/?#");
  std::string authority = rest.substr(0, hostEnd);
  // userinfo@host:port, with a bracketed IPv6 literal allowed
  if (const size_t at = authority.rfind('@'); at != std::string::npos)
    authority = authority.substr(at + 1);
  std::string host;
  if (!authority.empty() && authority[0] == '[') {
    const size_t close = authority.find(']');
    host = authority.substr(1, close == std::string::npos ? std::string::npos : close - 1);
  } else {
    const size_t colon = authority.find(':');
    host = colon == std::string::npos ? authority : authority.substr(0, colon);
  }
  if (scheme.empty())
    scheme = "root";
  return scheme + "://" + registrableDomain(host);
}

std::string dateOnlyUtc(uint64_t epochS) {
  const time_t t = static_cast<time_t>(epochS);
  struct tm tmv;
  // BOTH calls have to be checked, and the buffer initialised, because an
  // absurd timestamp is reachable: a run-file name is parsed into a uint64
  // with no range check, so a corrupt stats directory reaches this with a
  // year of ten digits or more. gmtime_r then returns null leaving tmv part
  // uninitialised, and strftime returns 0 leaving the buffer UNTERMINATED —
  // after which returning it read past the array and spliced whatever stack
  // bytes followed into the payload's date field.
  if (::gmtime_r(&t, &tmv) == nullptr)
    return "";
  // A four-digit year or no date at all. Past that the value is not a time
  // the run could have started at, and a date the service cannot read is
  // worse than an absent one.
  if (tmv.tm_year < 0 || tmv.tm_year > 8099)
    return "";
  char buf[32] = {0};
  const size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%d", &tmv);
  if (n == 0)
    return "";
  return std::string(buf, n);
}

const char* const kBenchDropKeys[5] = {"path", "mount", "mount_source", "mount_opts", "mount_super_opts"};

// The string-valued keys of a bench record that may leave the machine. Every
// leak is a string — a path, a host, a name — so numbers, booleans and
// collections of them pass unexamined, while a string under a key not named
// here is DROPPED.
//
// A denylist cannot hold this line. The record is open-ended and grows as the
// tool learns to measure more, and each new field arrives already published:
// `cachepath_error` carried the benchmarked directory and the process id in
// its text, was never on the drop list, and reached a public page for every
// cache directory the service's own place-patterns did not happen to match.
// An allowlist fails the other way — a new measurement's units or shape is
// withheld until it is named here, which costs a line and no privacy.
const char* const kBenchKeepStrings[] = {
    "fs",         "mode",   "error",     "build_write_shape", "time", "version",
    "build_id",   "kernel", "arch",      "cpu_model",         "dev",  "mount_fstype",
    "dev_name",   "dev_model",           "dev_sched"};

// Does this value contain a string anywhere? A record is flat today; a nested
// object of numbers is still safe, a nested string is not.
bool hasAnyString(const Json& v) {
  if (v.isString())
    return true;
  if (v.isArray()) {
    for (const auto& e : v.arr)
      if (hasAnyString(e))
        return true;
    return false;
  }
  if (v.isObject()) {
    for (const auto& kv : v.obj)
      if (hasAnyString(kv.second))
        return true;
    return false;
  }
  return false;
}

bool keepsItsText(const std::string& key) {
  for (const char* k : kBenchKeepStrings)
    if (key == k)
      return true;
  return false;
}

std::string labelProblem(const std::string& labelIn) {
  const std::string label = trim(labelIn);
  if (label.empty())
    return "";
  if (label[0] == '-')
    return "a label may not start with '-' (that reads as a flag)";
  if (label.find_first_of("/\\@~") != std::string::npos)
    return "a label is a name, not a location or an address: no '/', '\\', '@' or '~'";
  if (label.size() > kLabelMax)
    return "a label is at most " + std::to_string(kLabelMax) + " characters";
  return "";
}

Json redactBench(const Json& record, const Identity* id, const std::string& installIdValue) {
  Json out = Json::object();
  for (const auto& kv : record.obj) {
    bool drop = false;
    for (const char* k : kBenchDropKeys)
      if (kv.first == k)
        drop = true;
    if (drop)
      continue;
    if (kv.first == "host") {
      out.obj.emplace_back(kv.first, Json::string(""));
    } else if (kv.first == "cmd" && kv.second.isString()) {
      out.obj.emplace_back(kv.first, Json::string(normalizeCmd(kv.second.s)));
    } else if (hasAnyString(kv.second) && !keepsItsText(kv.first)) {
      continue; // text under a key nobody vouched for
    } else {
      out.obj.push_back(kv);
    }
  }
  const std::string host = record.str("host");
  if (id && !host.empty()) {
    if (const std::string p = record.str("path"); !p.empty())
      out.set("location", Json::string(locationHash(id->salt, host, p)));
    if (const std::string m = record.str("mount"); !m.empty())
      out.set("volume", Json::string(volumeHash(id->salt, host, m)));
  }
  if (!installIdValue.empty())
    out.set("install_id", Json::string(installIdValue));
  return out;
}

Json redactNetbench(const Json& record) {
  Json out = record;
  if (out.has("host"))
    out.set("host", Json::string(""));
  if (const Json* u = out.get("url"); u && u->isString())
    out.set("url", Json::string(coarsenRootUrl(u->s)));
  return out;
}

// ============================================================================
// the machine block
// ============================================================================

#if !defined(__APPLE__)
namespace {

// /etc/os-release values are quoted; macOS reads its version another way and
// has no caller for this, and AppleClang refuses an unused function.
std::string unquote(std::string v) {
  v = trim(v);
  if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
    v = v.substr(1, v.size() - 2);
  return v;
}

} // namespace
#endif

Json machineBlock(const Identity* id, const std::string& xrootdClient, const std::string& ucacheVersion) {
  Json m = Json::object();
  const std::string host = hostName();
  if (id && !host.empty())
    m.set("id", Json::string(machineHash(id->salt, host)));
  std::string os, osRelease, arch, kernel, cpu;
  long ncpu = ::sysconf(_SC_NPROCESSORS_ONLN);
  double memGb = 0;
  struct ::utsname u;
  if (::uname(&u) == 0) {
    kernel = u.release;
    arch = u.machine;
  }
#if defined(__APPLE__)
  os = "macos";
  char buf[256];
  size_t sz = sizeof buf;
  if (::sysctlbyname("kern.osproductversion", buf, &sz, nullptr, 0) == 0)
    osRelease = std::string(buf, std::strlen(buf));
  sz = sizeof buf;
  if (::sysctlbyname("machdep.cpu.brand_string", buf, &sz, nullptr, 0) == 0)
    cpu = std::string(buf, std::strlen(buf));
  uint64_t mem = 0;
  size_t msz = sizeof mem;
  if (::sysctlbyname("hw.memsize", &mem, &msz, nullptr, 0) == 0)
    memGb = static_cast<double>(mem) / (1024.0 * 1024.0 * 1024.0);
#else
  {
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
      if (line.compare(0, 3, "ID=") == 0)
        os = unquote(line.substr(3));
      else if (line.compare(0, 11, "VERSION_ID=") == 0)
        osRelease = unquote(line.substr(11));
    }
    if (os.empty())
      os = "linux";
  }
  {
    std::ifstream ci("/proc/cpuinfo");
    std::string line;
    while (std::getline(ci, line)) {
      if (line.compare(0, 10, "model name") == 0 || line.compare(0, 8, "Hardware") == 0) {
        const size_t c = line.find(':');
        if (c != std::string::npos)
          cpu = trim(line.substr(c + 1));
        break;
      }
    }
    std::ifstream mi("/proc/meminfo");
    while (std::getline(mi, line)) {
      if (line.compare(0, 9, "MemTotal:") == 0) {
        memGb = std::strtod(line.c_str() + 9, nullptr) / (1024.0 * 1024.0);
        break;
      }
    }
  }
#endif
  if (!os.empty())
    m.set("os", Json::string(os));
  if (!osRelease.empty())
    m.set("os_release", Json::string(osRelease));
  if (!arch.empty())
    m.set("arch", Json::string(arch));
  if (!kernel.empty())
    m.set("kernel", Json::string(kernel));
  if (!cpu.empty())
    m.set("cpu_model", Json::string(cpu));
  if (ncpu > 0)
    m.set("ncpu", Json::integer(ncpu));
  if (memGb > 0)
    m.set("mem_gb", Json::real(memGb, 1));
  if (!xrootdClient.empty())
    m.set("xrootd_client", Json::string(xrootdClient));
  if (!ucacheVersion.empty())
    m.set("ucache_version", Json::string(ucacheVersion));
  return m;
}

// ============================================================================
// the record store
// ============================================================================

std::string recordsPath() {
  if (const char* p = ::getenv("UCACHE_RECORDS_FILE"); p && *p)
    return p;
  return xdgOr("XDG_DATA_HOME", homeDir() + "/.local/share") + "/ucache/records.jsonl";
}

int appendRecordLine(const std::string& line, std::string* err) {
  const int rc = appendLine(recordsPath(), line, 0600);
  if (rc && err)
    *err = std::strerror(rc);
  return rc;
}

std::vector<StoredRecord> loadRecords() {
  std::vector<StoredRecord> out;
  std::ifstream f(recordsPath());
  std::string line;
  static const std::pair<const char*, const char*> prefixes[] = {
      {"ucache-bench-json: ", "bench"},
      {"ucache-netbench-json: ", "netbench"},
      {"ucache-label: ", "label"},
      {"ucache-published: ", "published"},
  };
  while (std::getline(f, line)) {
    for (const auto& [prefix, kind] : prefixes) {
      const size_t n = std::strlen(prefix);
      if (line.compare(0, n, prefix) != 0)
        continue;
      Json body;
      if (Json::parse(line.substr(n), body) && body.isObject())
        out.push_back({kind, std::move(body)});
      break;
    }
  }
  return out;
}

int rememberLabel(const std::string& host, const std::string& path, const std::string& label) {
  Json j = Json::object();
  j.set("host", Json::string(host));
  j.set("path", Json::string(path));
  j.set("label", Json::string(label));
  return appendRecordLine("ucache-label: " + j.dump());
}

std::string knownLabel(const std::vector<StoredRecord>& records, const std::string& host,
                       const std::string& path) {
  std::string found;
  const std::string h = lowerTrim(host), p = normPath(path);
  for (const auto& r : records)
    if (r.kind == "label" && lowerTrim(r.body.str("host")) == h && normPath(r.body.str("path")) == p)
      found = r.body.str("label"); // last one written wins
  return found;
}

int markPublished(const std::string& kind, const std::string& reportUrl) {
  Json j = Json::object();
  j.set("at", Json::string(dateOnlyUtc(static_cast<uint64_t>(::time(nullptr)))));
  j.set("kind", Json::string(kind));
  j.set("url", Json::string(reportUrl));
  return appendRecordLine("ucache-published: " + j.dump());
}

// ============================================================================
// the cache instance id
// ============================================================================

std::string installId(const std::string& cacheDir, bool create, bool* fresh) {
  if (fresh)
    *fresh = false;
  struct ::stat st;
  if (cacheDir.empty() || ::stat(cacheDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    return "";
  const std::string path = cacheDir + "/install-id";
  std::string text;
  bool replacing = false;
  if (readWholeFile(path, text)) {
    const std::string id = trim(text);
    if (looksLikeUuid(id))
      return id;
    std::fprintf(stderr, "publish: %s is not a UUID — replacing it\n", path.c_str());
    replacing = true; // a deliberate replacement, not a race
  } else {
    struct ::stat ist;
    if (::stat(path.c_str(), &ist) == 0) {
      // Present but unreadable. Minting a replacement would silently orphan
      // the page this cache already publishes to, so say so and publish
      // without an instance id instead.
      std::fprintf(stderr,
                   "publish: %s exists but cannot be read — this cache publishes without an "
                   "instance id; fix its permissions to keep its page\n",
                   path.c_str());
      return "";
    }
  }
  if (!create)
    return ""; // asked only whether one exists
  const std::string id = newUuid4();
  // World-readable on purpose: a second user reading this cache read-only
  // publishes the same instance, and the id is not a secret (the report URL
  // it leads to shows nothing that was not published).
  int rc;
  if (replacing) {
    rc = writeFileAtomic(path, id + "\n", 0644); // the file is there and is not an id
  } else {
    rc = createFileExclusive(path, id + "\n", 0644);
    if (rc == EEXIST) {
      // Another publish minted one between our read and our write. One cache
      // directory is one instance, so adopt theirs rather than have this cache
      // appear twice on the owner page under two ids.
      std::string other;
      if (readWholeFile(path, other)) {
        const std::string got = trim(other);
        if (looksLikeUuid(got))
          return got;
      }
      return "";
    }
  }
  if (rc) {
    std::fprintf(stderr,
                 "publish: could not write %s (%s) — this cache gets a fresh instance id on "
                 "every publish until it can be written\n",
                 path.c_str(), std::strerror(rc));
    if (fresh)
      *fresh = true;
  }
  return id;
}

// ============================================================================
// the payload
// ============================================================================

std::string serviceUrl(const std::string& flagValue) {
  std::string u = flagValue;
  if (u.empty())
    if (const char* e = ::getenv("UCACHE_PUBLISH_URL"); e && *e)
      u = e;
  if (u.empty())
    u = kDefaultServiceUrl;
  while (!u.empty() && u.back() == '/')
    u.pop_back();
  return u;
}

Json buildPayload(const PayloadParts& parts) {
  Json p = Json::object();
  p.set("schema", Json::integer(kPayloadSchema));
  p.set("kind", Json::string("publish"));
  if (!parts.ucacheVersion.empty())
    p.set("ucache_version", Json::string(parts.ucacheVersion));
  p.set("owner_id", parts.identity ? Json::string(parts.identity->owner) : Json::null());
  if (!parts.installId.empty())
    p.set("install_id", Json::string(parts.installId));
  if (!parts.location.empty())
    p.set("location", Json::string(parts.location));
  if (!parts.volume.empty())
    p.set("volume", Json::string(parts.volume));
  if (!parts.label.empty())
    p.set("label", Json::string(parts.label));
  if (parts.machine.isObject())
    p.set("machine", parts.machine);
  Json bench = Json::array();
  for (const auto& r : parts.bench)
    bench.push(r);
  p.set("bench", bench);
  Json net = Json::array();
  for (const auto& r : parts.netbench)
    net.push(r);
  p.set("netbench", net);
  if (parts.history.isObject())
    p.set("history", parts.history);
  return p;
}

Json withPlaceholderIds(Json payload) {
  static const char* kOwner = "00000000-0000-4000-8000-000000000000";
  static const char* kInstall = "00000000-0000-4000-8000-000000000001";
  const std::string kMachine(32, '0'), kLocation(32, '1'), kVolume(32, '2');
  if (payload.get("owner_id") && payload.get("owner_id")->isString())
    payload.set("owner_id", Json::string(kOwner));
  if (payload.has("install_id"))
    payload.set("install_id", Json::string(kInstall));
  if (payload.has("location"))
    payload.set("location", Json::string(kLocation));
  if (payload.has("volume"))
    payload.set("volume", Json::string(kVolume));
  if (Json* m = payload.get("machine"); m && m->has("id"))
    m->set("id", Json::string(kMachine));
  if (Json* b = payload.get("bench"); b && b->isArray())
    for (auto& r : b->arr) {
      if (r.has("location"))
        r.set("location", Json::string(kLocation));
      if (r.has("volume"))
        r.set("volume", Json::string(kVolume));
      if (r.has("install_id"))
        r.set("install_id", Json::string(kInstall));
    }
  return payload;
}

std::string describePayload(const Json& payload) {
  std::string s;
  const Json* m = payload.get("machine");
  if (m && m->isObject()) {
    s += "  machine   ";
    s += m->str("os");
    if (m->has("os_release"))
      s += " " + m->str("os_release");
    if (m->has("kernel"))
      s += ", kernel " + m->str("kernel");
    if (m->has("cpu_model"))
      s += ", " + m->str("cpu_model");
    if (m->has("ncpu"))
      s += ", " + m->get("ncpu")->s + " cpu";
    if (m->has("mem_gb"))
      s += ", " + m->get("mem_gb")->s + " GiB RAM";
    s += m->has("id") ? "   (hostname replaced by a salted hash)\n" : "   (no identity: unlinked)\n";
  }
  const Json* b = payload.get("bench");
  if (b && b->isArray() && !b->arr.empty()) {
    s += "  storage   " + std::to_string(b->arr.size()) + " benchmark record(s):";
    for (const auto& r : b->arr) {
      s += " " + r.str("time").substr(0, 10);
      if (r.has("cmd"))
        s += " [" + r.str("cmd") + "]";
    }
    s += "\n            paths and mount points replaced by salted hashes, hostname blanked\n";
  }
  const Json* n = payload.get("netbench");
  if (n && n->isArray() && !n->arr.empty()) {
    s += "  origin    " + std::to_string(n->arr.size()) + " origin benchmark record(s):";
    for (const auto& r : n->arr)
      s += " " + r.str("url") + " (" + r.str("mode") + ")";
    s += "\n            origin URL reduced to its domain, hostname blanked\n";
  }
  const Json* h = payload.get("history");
  if (h && h->isObject()) {
    const Json* runs = h->get("runs");
    const size_t k = runs && runs->isArray() ? runs->arr.size() : 0;
    s += "  history   " + std::to_string(k) + " run(s)";
    if (k) {
      s += " from " + runs->arr.front().str("start") + " to " + runs->arr.back().str("start");
    }
    s += "\n            per-run byte counts and measured gains; times reduced to dates, hostname\n"
         "            blanked, process ids dropped, origins reduced to domains; no file names\n";
  }
  if (payload.has("label"))
    s += "  label     \"" + payload.str("label") + "\"\n";
  return s;
}

// ============================================================================
// sending
// ============================================================================

namespace {

bool isExecutable(const std::string& path) {
  struct ::stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

std::string findCurl() {
  if (const char* c = ::getenv("UCACHE_CURL"); c && *c)
    return isExecutable(c) ? std::string(c) : std::string();
  std::string p;
  if (const char* env = ::getenv("PATH"); env && *env)
    p = env;
  else
    p = "/usr/local/bin:/usr/bin:/bin";
  for (size_t at = 0;;) {
    const size_t colon = p.find(':', at);
    std::string dir = p.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
    if (dir.empty())
      dir = ".";
    if (isExecutable(dir + "/curl"))
      return dir + "/curl";
    if (colon == std::string::npos)
      break;
    at = colon + 1;
  }
  return "";
}

struct CurlResult {
  int exitCode = -1; // curl's own; -1 = could not run it
  int httpStatus = 0;
  std::string body, headers, spawnError;
};

CurlResult runCurl(const std::string& curl, const std::string& url, const std::string& payloadFile,
                   const std::string& workDir, const std::string& version) {
  CurlResult r;
  const std::string bodyFile = workDir + "/body", headFile = workDir + "/headers",
                    codeFile = workDir + "/code";
  const std::string ua = "ucache/" + version;
  std::vector<std::string> args = {curl, "-sS", "-o", bodyFile, "-D", headFile, "-w", "%{http_code}",
                                   "-H", "Content-Type: application/json", "-H", "User-Agent: " + ua,
                                   "--max-time", "120", "--data-binary", "@" + payloadFile, url};
  std::vector<char*> argv;
  for (auto& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, codeFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  pid_t pid = 0;
  const int rc = ::posix_spawn(&pid, curl.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    r.spawnError = std::strerror(rc);
    return r;
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  std::string code;
  readWholeFile(codeFile, code);
  r.httpStatus = static_cast<int>(std::strtol(trim(code).c_str(), nullptr, 10));
  readWholeFile(bodyFile, r.body);
  readWholeFile(headFile, r.headers);
  return r;
}

int retryAfterSeconds(const std::string& headers) {
  std::istringstream is(headers);
  std::string line;
  while (std::getline(is, line)) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    if (toLower(trim(line.substr(0, colon))) == "retry-after")
      return static_cast<int>(std::strtol(trim(line.substr(colon + 1)).c_str(), nullptr, 10));
  }
  return -1;
}

void removeWorkDir(const std::string& dir) {
  for (const char* f : {"body", "headers", "code", "payload.json"})
    ::unlink((dir + "/" + f).c_str());
  ::rmdir(dir.c_str());
}

// A network-level failure curl reports through its exit code, where trying
// again is reasonable: name resolution, connection refused, timeouts, resets.
bool retryableCurlExit(int code) {
  switch (code) {
  case 6: case 7: case 18: case 28: case 35: case 52: case 55: case 56: return true;
  default: return false;
  }
}

} // namespace

int sendPayload(const Json& payload, const std::string& baseUrl, PublishOutcome& out) {
  const std::string version = payload.str("ucache_version", "unknown");
  const std::string endpoint = baseUrl + "/v1/publish";
  out.endpoint = endpoint;
  const std::string text = payload.dump();

  auto saveForRetry = [&](const std::string& why) {
    const std::string rp = recordsPath();
    const size_t slash = rp.rfind('/');
    const std::string saved = (slash == std::string::npos ? "." : rp.substr(0, slash)) + "/last-payload.json";
    if (writeFileAtomic(saved, text + "\n", 0600) == 0)
      out.savedPayload = saved;
    out.failure = why;
    return 1;
  };

  const std::string curl = findCurl();
  if (curl.empty())
    return saveForRetry("`curl` was not found on PATH (UCACHE_CURL names one explicitly)");

  const char* tmpEnv = ::getenv("TMPDIR");
  std::string tmpl = std::string(tmpEnv && *tmpEnv ? tmpEnv : "/tmp") + "/ucache-publish-XXXXXX";
  std::vector<char> tbuf(tmpl.begin(), tmpl.end());
  tbuf.push_back('\0');
  if (!::mkdtemp(tbuf.data()))
    return saveForRetry(std::string("could not create a temporary directory: ") + std::strerror(errno));
  const std::string workDir = tbuf.data();
  const std::string payloadFile = workDir + "/payload.json";
  if (int rc = writeFileAtomic(payloadFile, text, 0600); rc) {
    removeWorkDir(workDir);
    return saveForRetry(std::string("could not write the payload: ") + std::strerror(rc));
  }

  static const int backoff[] = {2, 8, 30};
  const int attempts = 1 + static_cast<int>(sizeof backoff / sizeof backoff[0]);
  int result = 1;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    CurlResult r = runCurl(curl, endpoint, payloadFile, workDir, version);
    std::string reason;
    int wait = attempt < attempts ? backoff[attempt - 1] : 0;
    if (!r.spawnError.empty()) {
      result = saveForRetry("could not run " + curl + ": " + r.spawnError);
      break;
    }
    if (r.exitCode != 0 && r.httpStatus == 0) {
      reason = "connection failed (curl exit " + std::to_string(r.exitCode) + ")";
      if (!retryableCurlExit(r.exitCode) || attempt == attempts) {
        result = saveForRetry(reason);
        break;
      }
    } else {
      out.httpStatus = r.httpStatus;
      Json reply;
      const bool parsed = Json::parse(r.body, reply) && reply.isObject();
      if (r.httpStatus == 201 || r.httpStatus == 200) {
        if (parsed) {
          out.reportUrl = reply.str("report_url");
          out.ownerUrl = reply.str("owner_url");
          out.installUrl = reply.str("install_url");
          if (const Json* d = reply.get("duplicate"); d && d->type == Json::Type::Bool)
            out.duplicate = d->b;
          if (const Json* f = reply.get("findings"); f && f->isArray())
            for (const auto& x : f->arr)
              out.findings.push_back({x.str("code"), x.str("severity"), x.str("text"), x.str("doc_url")});
        }
        // Accepted, but a reply without a report URL is not a success the user
        // can act on. Not saved for retry: the service has it.
        if (out.reportUrl.empty()) {
          out.failure = "the service accepted the payload (HTTP " + std::to_string(r.httpStatus) +
                        ") but its reply carried no report URL";
          result = 1;
        } else {
          result = 0;
        }
        break;
      }
      if (parsed) {
        out.error = reply.str("error");
        out.detail = reply.str("detail");
        out.field = reply.str("field");
      }
      if (r.httpStatus == 429) {
        const int ra = retryAfterSeconds(r.headers);
        if (ra > 0 && ra <= 60 && attempt < attempts) {
          wait = ra;
          reason = "the service asks to wait (rate limit)";
        } else {
          // The reply's own words follow in printOutcome; do not repeat them here.
          result = saveForRetry(ra > 0 ? "the service asks to retry in " + std::to_string(ra) + " s"
                                       : "rate limited");
          break;
        }
      } else if (r.httpStatus == 502 || r.httpStatus == 503 || r.httpStatus == 504) {
        reason = "the service is restarting (HTTP " + std::to_string(r.httpStatus) + ")";
        if (attempt == attempts) {
          result = saveForRetry(reason);
          break;
        }
      } else {
        // 4xx/5xx that trying again would not change: the reply says why.
        result = saveForRetry("refused (HTTP " + std::to_string(r.httpStatus) + ")");
        break;
      }
    }
    std::fprintf(stderr, "publish: %s; retrying in %d s (%d of %d)\n", reason.c_str(), wait, attempt + 1,
                 attempts);
    ::sleep(static_cast<unsigned>(wait));
  }
  removeWorkDir(workDir);
  return result;
}

void printOutcome(const PublishOutcome& out) {
  if ((out.httpStatus == 201 || out.httpStatus == 200) && !out.reportUrl.empty()) {
    if (out.duplicate)
      std::printf("already published — nothing new in this payload\n");
    std::printf("report   %s\n", out.reportUrl.c_str());
    if (!out.installUrl.empty())
      std::printf("cache    %s\n", out.installUrl.c_str());
    if (!out.ownerUrl.empty())
      std::printf("owner    %s   (everything you published; the URL is the key — treat it like one)\n",
                  out.ownerUrl.c_str());
    if (!out.findings.empty()) {
      std::printf("findings\n");
      for (const auto& f : out.findings) {
        std::printf("  [%s] %s\n", f.severity.c_str(), f.text.c_str());
        if (!f.docUrl.empty())
          std::printf("         %s\n", f.docUrl.c_str());
      }
    }
    return;
  }
  if (out.httpStatus) {
    std::fprintf(stderr, "publish: %s", out.failure.empty() ? "refused" : out.failure.c_str());
    if (!out.detail.empty())
      std::fprintf(stderr, ": %s", out.detail.c_str());
    if (!out.field.empty())
      std::fprintf(stderr, " (field %s)", out.field.c_str());
    std::fputc('\n', stderr);
  } else {
    std::fprintf(stderr, "publish: %s\n", out.failure.c_str());
  }
  if (!out.savedPayload.empty())
    std::fprintf(stderr,
                 "  the redacted payload is kept at %s — inspect it, and retry by hand with\n"
                 "  curl -H 'Content-Type: application/json' --data-binary @%s %s\n",
                 out.savedPayload.c_str(), out.savedPayload.c_str(),
                 out.endpoint.empty() ? "<service>/v1/publish" : out.endpoint.c_str());
}

bool confirmPublish(const std::string& summary, bool yes) {
  std::fputs(summary.c_str(), stdout);
  std::fflush(stdout);
  if (yes)
    return true;
  if (!::isatty(STDIN_FILENO)) {
    std::fputs("publish: not sent — stdin is not a terminal; add --yes to confirm without a prompt, or\n"
               "         --dry-run to see the exact payload\n",
               stderr);
    return false;
  }
  // Whatever is already in the terminal's buffer was typed or pasted BEFORE
  // this question appeared, so it is not an answer to it. The label prompt
  // reads one line with fgets and leaves the rest of a pasted block behind;
  // without this, pasting a label followed by a line starting with "y" sent
  // the payload without the user ever seeing the summary above.
  ::tcflush(STDIN_FILENO, TCIFLUSH);
  std::fputs("Send it? [y/N] ", stdout);
  std::fflush(stdout);
  char buf[16] = {0};
  if (!std::fgets(buf, sizeof buf, stdin) || (buf[0] != 'y' && buf[0] != 'Y')) {
    std::puts("not sent");
    return false;
  }
  return true;
}

} // namespace ucache
