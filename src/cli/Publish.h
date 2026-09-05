// `ucache publish`, `bench --publish`, `netbench --publish`, `ucache identity`:
// send a measurement to the report service and get a report back.
//
// Nothing here runs in the plugin or in a data path. Publishing is an explicit
// foreground command; a physics job cannot be affected because nothing that
// serves reads knows this module exists.
//
// What leaves the machine, and what never does, is decided HERE, before any
// byte reaches the network, and docs/PUBLISH.md is the plain-language copy of
// these rules. The server checks again and refuses a payload that names a
// place, but that is a backstop: the redaction is the client's job.
//
//   - Paths never leave. A benchmarked directory becomes a salted hash (its
//     "location"), its mount point another ("volume"), and the hostname a third
//     ("machine"). The salt is generated once per user, stored in the identity
//     file, and NEVER sent, so nobody holding the hashes can test a guess.
//   - The hostname is blanked, the process id dropped, run timestamps reduced
//     to dates, origin URLs to their registrable domain (`root://cern.ch`).
//   - The benchmark's command line is kept because its parameters make two
//     runs comparable, but every path in it becomes `<path>`.
//
// The identity string `ucache-id:<owner-uuid>:<salt-hex>` is the key to the
// owner's pages and the only thing that links records across machines and
// browsers. It lives in one file and is printed once, with a warning.
//
// Thread-safety: single-caller (CLI); no shared state.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ucache {

// ---- a small JSON value, enough for records and the service's replies -------
// Numbers keep their ORIGINAL TEXT: a record re-emitted here must be the record
// the tool printed, digit for digit, not a re-rounding of it.
struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool b = false;
  std::string s; // Number: the source text; String: the decoded value
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj; // insertion order kept

  static Json null() { return Json(); }
  static Json boolean(bool v);
  static Json numberText(const std::string& text);
  static Json integer(long long v);
  static Json real(double v, int decimals);
  static Json string(const std::string& v);
  static Json array();
  static Json object();

  bool isNull() const { return type == Type::Null; }
  bool isObject() const { return type == Type::Object; }
  bool isArray() const { return type == Type::Array; }
  bool isString() const { return type == Type::String; }
  bool isNumber() const { return type == Type::Number; }
  const Json* get(const std::string& key) const;
  Json* get(const std::string& key);
  bool has(const std::string& key) const { return get(key) != nullptr; }
  // Replaces an existing key in place (order kept) or appends.
  void set(const std::string& key, Json value);
  void erase(const std::string& key);
  void push(Json value) { arr.push_back(std::move(value)); }
  std::string str(const std::string& key, const std::string& dflt = "") const;
  double num(const std::string& key, double dflt = 0.0) const;

  // Strict-enough parser: objects, arrays, strings with escapes (\uXXXX
  // included, surrogate pairs combined), numbers, true/false/null. `err`
  // receives a one-line reason on failure.
  static bool parse(const std::string& text, Json& out, std::string* err = nullptr);
  // Compact (indent < 0) or pretty (indent >= 0 spaces per level).
  std::string dump(int indent = -1) const;
};

std::string jsonEscape(const std::string& s);

// ---- identity ----------------------------------------------------------------
struct Identity {
  std::string owner; // UUID4, lowercase
  std::string salt;  // 32 hex characters; never sent
  std::string text() const { return "ucache-id:" + owner + ":" + salt; }
};

// `ucache-id:<owner-uuid>:<salt-hex>` -> Identity; false when malformed.
bool parseIdentity(const std::string& text, Identity& out);
Identity newIdentity();
// UCACHE_IDENTITY_FILE, else $XDG_CONFIG_HOME/ucache/identity, else
// ~/.config/ucache/identity.
std::string identityPath();
std::optional<Identity> loadIdentity(std::string* err = nullptr);
// Writes the file with mode 0600, creating the directory. Returns 0 or errno.
int saveIdentity(const Identity& id, std::string* err = nullptr);
// The fail-soft rule: an existing identity is used; none and `create` -> a
// fresh one is written and `created` set; a home that cannot be written ->
// nullopt and a one-line warning on stderr, and the records go out unlinked.
std::optional<Identity> ensureIdentity(bool create, bool* created = nullptr);

// ---- the hashes, byte for byte the service's own -----------------------------
// sha256(salt + NUL + value), first 32 hex characters.
std::string saltedHash(const std::string& salt, const std::string& value);
std::string machineHash(const std::string& salt, const std::string& host);
std::string locationHash(const std::string& salt, const std::string& host, const std::string& path);
std::string volumeHash(const std::string& salt, const std::string& host, const std::string& mountpoint);
// Python's os.path.normpath for POSIX paths: the form both ends hash.
std::string normPath(const std::string& path);
std::string hostName();
std::string lowerTrim(const std::string& s);

// ---- field-level redaction ---------------------------------------------------
// Keep the parameters, lose everything that names a place: executable ->
// `ucache`, `--log X` removed, any path argument -> `<path>`.
std::string normalizeCmd(const std::string& cmd);
// `eoscms.cern.ch` -> `cern.ch`; `x.y.ac.uk` -> `y.ac.uk`; addresses -> `unknown`.
std::string registrableDomain(const std::string& host);
// `root://host:1094//eos/...` -> `root://cern.ch`.
std::string coarsenRootUrl(const std::string& url);
// Unix seconds -> `YYYY-MM-DD` in UTC.
std::string dateOnlyUtc(uint64_t epochS);

// Keys dropped from a bench record, and the ones blanked, in the order the
// service lists them (the service refuses a record where any is still set).
extern const char* const kBenchDropKeys[6];

// A raw `ucache-bench-json` record -> what may be sent. With an identity the
// location and volume hashes are added from the record's own host, path and
// mount before those are removed. `installId`, when non-empty, is written in
// (a disk that runs a cache links to it).
Json redactBench(const Json& record, const Identity* id, const std::string& installId = "");
// A raw `ucache-netbench-json` record -> host blanked, url coarsened.
Json redactNetbench(const Json& record);

// ---- the machine block -------------------------------------------------------
// CPU model, cores, RAM, kernel, OS name and release, architecture, the XRootD
// client version if known, this tool's version; `id` when an identity exists.
// Gathered from /proc, /etc/os-release and uname on Linux, sysctl on macOS.
Json machineBlock(const Identity* id, const std::string& xrootdClient, const std::string& ucacheVersion);

// ---- the per-user record store -----------------------------------------------
// Every `bench` and `netbench` run appends its raw record line here so a later
// `ucache publish` can send the disk's measurements with the cache's history.
// UCACHE_RECORDS_FILE, else $XDG_DATA_HOME/ucache/records.jsonl, else
// ~/.local/share/ucache/records.jsonl. Private to the user (0600); it holds the
// raw, unredacted records.
std::string recordsPath();
// Appends one line verbatim (`ucache-bench-json: {...}`); 0 or errno.
int appendRecordLine(const std::string& line, std::string* err = nullptr);
struct StoredRecord {
  std::string kind; // bench | netbench | label | published
  Json body;
};
std::vector<StoredRecord> loadRecords();
// Remembers a disk's label so the prompt is asked once per location.
int rememberLabel(const std::string& host, const std::string& path, const std::string& label);
std::string knownLabel(const std::vector<StoredRecord>& records, const std::string& host,
                       const std::string& path);
int markPublished(const std::string& kind, const std::string& reportUrl);

// ---- the cache instance id ----------------------------------------------------
// `<cacheDir>/install-id`: a UUID4 minted once, kept across `clear` (which
// removes entries, never this file). Unwritable -> a fresh id per invocation
// and a warning, never a failure. Empty when the directory does not exist.
std::string installId(const std::string& cacheDir, bool create, bool* fresh = nullptr);
bool looksLikeUuid(const std::string& s);
std::string newUuid4();

// ---- the payload -------------------------------------------------------------
inline constexpr int kPayloadSchema = 1;
inline constexpr const char* kDefaultServiceUrl = "https://ucache.web.cern.ch";
// UCACHE_PUBLISH_URL, else the default.
std::string serviceUrl(const std::string& flagValue);

struct PayloadParts {
  std::optional<Identity> identity;
  std::string installId;   // the cache instance, when publishing one
  std::string location;    // the cache directory's location hash
  std::string volume;      // and its mount point's
  std::string label;       // free text, at most 80 characters
  Json machine;            // machineBlock()
  std::vector<Json> bench; // already redacted
  std::vector<Json> netbench;
  Json history;            // null when none
  std::string ucacheVersion;
};
Json buildPayload(const PayloadParts& parts);
// The same payload with every identifier replaced by an obvious placeholder:
// dry-run output gets pasted into tickets, and the ids are keys to pages.
Json withPlaceholderIds(Json payload);
// One paragraph naming what is about to be sent, for the confirmation.
std::string describePayload(const Json& payload);

// ---- sending --------------------------------------------------------------------
struct Finding {
  std::string code, severity, text, docUrl;
};
struct PublishOutcome {
  int httpStatus = 0;       // 0 = never reached the service
  std::string reportUrl, ownerUrl, installUrl;
  bool duplicate = false;
  std::vector<Finding> findings;
  std::string error, detail, field; // from an error reply
  std::string failure;              // local reason when httpStatus == 0
  std::string savedPayload;         // where the payload was left on failure
};
// POST via the system's `curl` (UCACHE_CURL overrides the executable), with
// three retries at 2/8/30 s on connection failures and 503, honouring a short
// Retry-After on 429. Prints progress to stderr. Returns the process exit code
// to use: 0 accepted (201 or 200 duplicate), 1 refused or unreachable.
int sendPayload(const Json& payload, const std::string& baseUrl, PublishOutcome& out);
// The report, the findings and the owner page, for a terminal.
void printOutcome(const PublishOutcome& out);
// tty: summary + [y/N]. Not a tty: `yes` decides; without it the refusal names
// the flag. Returns true to proceed.
bool confirmPublish(const std::string& summary, bool yes);

} // namespace ucache
