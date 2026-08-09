#include "keylog.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <system_error>
#include <unordered_set>

#include "piece_doc.h"
#include "project.h"
#include "selection.h"
#include "sqlite.h"
#include "unicode.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

constexpr Index kContextLines = 10;

constexpr std::size_t kMaxContextBytes = 16 * 1024;

void TruncateOnCodePoint(std::string& text, std::size_t limit) {
  if (text.size() <= limit) return;
  std::size_t at = limit;
  while ((at > 0) && ((static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)) --at;
  text.resize(at);
}

constexpr std::size_t kMaxBuffered = 4096;

constexpr std::size_t kMaxSelectionsRecorded = 64;

constexpr std::size_t kMaxSeenContexts = 1u << 18;

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  started_ts REAL    NOT NULL,"
    "  pane       TEXT    NOT NULL,"
    "  keymap     TEXT    NOT NULL);"
    "CREATE TABLE IF NOT EXISTS contexts ("
    "  hash       INTEGER PRIMARY KEY,"
    "  file       TEXT    NOT NULL,"
    "  first_line INTEGER NOT NULL,"
    "  text       TEXT    NOT NULL);"
    "CREATE TABLE IF NOT EXISTS events ("
    "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session      INTEGER NOT NULL,"
    "  seq          INTEGER NOT NULL,"
    "  ts_ms        INTEGER NOT NULL,"
    "  mode         TEXT    NOT NULL,"
    "  outcome      TEXT    NOT NULL,"
    "  key          TEXT    NOT NULL,"
    "  keys         TEXT    NOT NULL,"
    "  commands     TEXT    NOT NULL,"
    "  count_prefix INTEGER NOT NULL,"
    "  file         TEXT    NOT NULL,"
    "  line         INTEGER NOT NULL,"
    "  col          INTEGER NOT NULL,"
    "  cursors      INTEGER NOT NULL,"
    "  selections   TEXT    NOT NULL,"
    "  prompt       TEXT    NOT NULL,"
    "  ctx_hash     INTEGER);"
    "CREATE INDEX IF NOT EXISTS events_session ON events(session, seq);";

std::uint64_t HashBytes(std::uint64_t seed, std::string_view text) {
  std::uint64_t h = seed;
  for (const char c : text) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

constexpr std::uint64_t kHashSeed = 1469598103934665603ull;

std::string HexOf(std::uint64_t value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return std::string{text};
}

double WallNow() {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration<double>(since).count();
}

std::string LowerOf(std::string_view text) {
  std::string out{text};
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(((c >= 'A') && (c <= 'Z')) ? (c - 'A' + 'a') : c);
  });
  return out;
}

std::string ProjectRelative(const fs::path& path) {
  if (path.empty()) return {};
  std::error_code ec;
  const fs::path absolute = fs::weakly_canonical(fs::absolute(path, ec), ec);
  if (ec) return path.string();
  const fs::path root = ProjectRoot();
  if (root.empty()) return absolute.string();
  const fs::path relative = fs::relative(absolute, root, ec);
  if (ec || relative.empty() || relative.string().starts_with("..")) return absolute.string();
  return relative.string();
}

struct Row {
  std::int64_t era{0};
  std::int64_t ts_ms{0};
  const char* mode{"normal"};
  KeyOutcome outcome{KeyOutcome::kOther};
  std::string key;
  std::string keys;
  std::string commands;
  std::int64_t count_prefix{0};
  std::string file;
  std::int64_t line{0};
  std::int64_t col{0};
  std::int64_t cursors{0};
  std::string selections;
  std::string prompt;

  bool has_context{false};
  std::uint64_t ctx_hash{0};

  bool ctx_is_new{false};
  std::int64_t ctx_first_line{0};
  std::string ctx_text;
};

struct Era {
  std::int64_t id{0};
  std::string keymap;
  double started{0};
  bool used{false};
  std::int64_t session{0};
};

class SqliteRecorder final : public KeyRecorder {
 public:
  sqlite3* db{nullptr};
  std::string pane;

  ~SqliteRecorder() override {

    SqliteRecorder::Flush();
    if (db != nullptr) sqlite3_close(db);
  }

  void Exec(const char* sql) { ExecSql(db, sql); }

  void SetKeyMap(std::string fingerprint) override {
    Era& current = eras_.back();
    if (current.keymap == fingerprint) return;

    if (!current.used) {
      current.keymap = std::move(fingerprint);
      return;
    }
    eras_.push_back(Era{.id = current.id + 1, .keymap = std::move(fingerprint),
                        .started = WallNow()});
  }

  void Begin(const Editor& ed, const Key& key, const std::vector<Key>& prefix) override {
    open_ = Row{};
    open_valid_ = true;

    Era& era = eras_.back();
    era.used = true;
    open_.era = era.id;
    open_.ts_ms = Millis();
    open_.mode = (ed.mode == Mode::kInsert) ? "insert" : "normal";
    open_.key = KeyToString(key);
    open_.keys = SequenceText(prefix, key);
    open_.count_prefix = ed.pending_count;
    if (ed.prompt_active) open_.prompt = ed.prompt_input;

    CaptureWhere(ed);
  }

  void Resolve(KeyOutcome outcome, const std::vector<std::string>* commands) override {
    if (!open_valid_) return;
    open_valid_ = false;
    open_.outcome = outcome;
    // In insert mode the key *is* the file's text, so recording it verbatim
    // rebuilds, one character per row, exactly the contents the sensitive-path
    // guard exists to keep out of the log -- the guard dropped the surrounding
    // context and then wrote the secret itself to the next column over. The row
    // still says a character was typed, and where: only the literal goes.
    if (path_sensitive_ && (outcome == KeyOutcome::kInsertText)) {
      open_.key.clear();
      open_.keys.clear();
    }
    if (commands != nullptr) {

      for (const std::string& name : *commands) {
        if (!open_.commands.empty()) open_.commands += ',';
        open_.commands += name;
      }
    }
    Push(std::move(open_));
  }

  void Abandon() override {
    open_valid_ = false;
    open_ = Row{};
  }

  void NoteChordTimeout(const Editor& ed, const std::vector<Key>& chord) override {
    open_ = Row{};
    Era& era = eras_.back();
    era.used = true;
    open_.era = era.id;
    open_.ts_ms = Millis();
    open_.mode = (ed.mode == Mode::kInsert) ? "insert" : "normal";
    open_.keys = SequenceText(chord, std::nullopt);
    open_.outcome = KeyOutcome::kChordTimeout;
    open_.count_prefix = ed.pending_count;
    CaptureWhere(ed);
    open_valid_ = false;
    Push(std::move(open_));
  }

  bool Buffered() const override { return !pending_.empty(); }

  void Flush() override {
    if (pending_.empty()) return;
    if (db == nullptr) {
      pending_.clear();
      return;
    }

    Exec("BEGIN IMMEDIATE;");
    Stmt context{db, "INSERT OR IGNORE INTO contexts(hash,file,first_line,text)"
                     " VALUES(?,?,?,?);"};
    Stmt event{db, "INSERT INTO events(session,seq,ts_ms,mode,outcome,key,keys,commands,"
                   "count_prefix,file,line,col,cursors,selections,prompt,ctx_hash)"
                   " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);"};

    for (const Row& row : pending_) {
      const std::int64_t session = SessionFor(row.era);
      if (session == 0) continue;

      if (row.ctx_is_new && context) {
        context.Int(1, static_cast<std::int64_t>(row.ctx_hash));
        context.Text(2, row.file);
        context.Int(3, row.ctx_first_line);
        context.Text(4, row.ctx_text);
        context.Run();
        context.Reset();
      }
      if (!event) continue;

      event.Int(1, session);
      event.Int(2, ++seq_);
      event.Int(3, row.ts_ms);
      event.Text(4, row.mode);
      event.Text(5, NameOfOutcome(row.outcome));
      event.Text(6, row.key);
      event.Text(7, row.keys);
      event.Text(8, row.commands);
      event.Int(9, row.count_prefix);
      event.Text(10, row.file);
      event.Int(11, row.line);
      event.Int(12, row.col);
      event.Int(13, row.cursors);
      event.Text(14, row.selections);
      event.Text(15, row.prompt);
      if (row.has_context) {
        event.Int(16, static_cast<std::int64_t>(row.ctx_hash));
      } else {
        event.Null(16);
      }
      event.Run();
      event.Reset();
    }
    Exec("COMMIT;");
    pending_.clear();
  }

 private:
  std::int64_t Millis() const {
    const auto since = std::chrono::steady_clock::now() - started_;
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
  }

  static std::string SequenceText(const std::vector<Key>& prefix, std::optional<Key> last) {
    std::string out;
    for (const Key& k : prefix) {
      if (!out.empty()) out += ' ';
      out += KeyToString(k);
    }
    if (last) {
      if (!out.empty()) out += ' ';
      out += KeyToString(*last);
    }
    return out;
  }

  void CaptureWhere(const Editor& ed) {
    const PieceTable& table = ed.doc.table;

    if (!path_valid_ || (path_in_ != ed.doc.file)) {
      path_in_ = ed.doc.file;
      path_out_ = ProjectRelative(ed.doc.file);
      path_sensitive_ = ContentsAreSensitive(path_out_);
      path_valid_ = true;
    }
    open_.file = path_out_;

    const Selection& primary = ed.doc.selections.Primary();
    const Index cursor = CursorOf(table, primary);
    Index line = 0;
    Index line_start = 0;
    LineAtAndStart(table, cursor, line, line_start);
    open_.line = line + 1;
    open_.col = ColumnForByteFrom(table, line_start, cursor, ed.doc.tab_width);

    const std::vector<Selection>& ranges = ed.doc.selections.Ranges();
    open_.cursors = static_cast<std::int64_t>(ranges.size());
    open_.selections = std::to_string(ed.doc.selections.PrimaryIndex());
    open_.selections += ';';
    const std::size_t shown = std::min(ranges.size(), kMaxSelectionsRecorded);
    for (std::size_t i = 0; i < shown; ++i) {
      if (i != 0) open_.selections += ',';
      open_.selections += std::to_string(ranges[i].anchor);
      open_.selections += ':';
      open_.selections += std::to_string(ranges[i].head);
    }

    if (path_sensitive_) return;

    const Index last_line = LineCount(table) - 1;
    const Index first = std::max<Index>(0, line - kContextLines);
    const Index last = std::min<Index>(last_line, line + kContextLines);

    const Index revision = table.revision;
    const Index length = DocLength(table);
    // `last` belongs in the key as much as `first` does. Near the top of a file
    // `first` is pinned to 0 while the cursor moves, so line 1 and line 6 share
    // a key but not a window -- the second reaches five lines further down. The
    // memo then handed back the shorter text, and since the hash is taken over
    // it, the two windows became one row in `contexts` that neither event's
    // cursor actually sat in.
    if (!memo_valid_ || (memo_revision_ != revision) || (memo_length_ != length) ||
        (memo_first_ != first) || (memo_last_ != last) || (memo_file_ != path_out_)) {
      const Index from = LineStart(table, first);
      const Index to = (last >= last_line) ? length : LineStart(table, last + 1);
      ReadDocRangeInto(table, Interval(from, to), memo_text_);
      TruncateOnCodePoint(memo_text_, kMaxContextBytes);
      memo_hash_ = HashBytes(HashBytes(HashBytes(kHashSeed, path_out_), std::to_string(first + 1)),
                             memo_text_);
      memo_revision_ = revision;
      memo_length_ = length;
      memo_first_ = first;
      memo_last_ = last;
      memo_file_ = path_out_;
      memo_valid_ = true;
    }

    open_.has_context = true;
    open_.ctx_hash = memo_hash_;
    open_.ctx_first_line = first + 1;
    if (seen_.size() >= kMaxSeenContexts) seen_.clear();
    if (seen_.insert(memo_hash_).second) {
      open_.ctx_is_new = true;
      open_.ctx_text = memo_text_;
    }
  }

  void Push(Row row) {

    if (pending_.capacity() == 0) pending_.reserve(512);
    pending_.push_back(std::move(row));
    if (pending_.size() >= kMaxBuffered) Flush();
  }

  std::int64_t SessionFor(std::int64_t era_id) {
    const auto it = std::ranges::find(eras_, era_id, &Era::id);
    if (it == eras_.end()) return 0;
    if (it->session != 0) return it->session;

    Stmt insert{db, "INSERT INTO sessions(started_ts,pane,keymap) VALUES(?,?,?);"};
    if (!insert) return 0;
    insert.Real(1, it->started);
    insert.Text(2, pane);
    insert.Text(3, it->keymap);
    insert.Run();
    it->session = sqlite3_last_insert_rowid(db);
    return it->session;
  }

  std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
  std::vector<Era> eras_{Era{.started = WallNow()}};
  std::vector<Row> pending_;
  std::unordered_set<std::uint64_t> seen_;
  std::int64_t seq_{0};

  Row open_;
  bool open_valid_{false};

  fs::path path_in_;
  std::string path_out_;
  bool path_sensitive_{false};
  bool path_valid_{false};

  std::string memo_file_;
  std::string memo_text_;
  std::uint64_t memo_hash_{0};
  Index memo_revision_{-1};
  Index memo_length_{-1};
  Index memo_first_{-1};
  Index memo_last_{-1};
  bool memo_valid_{false};
};

}

std::string_view NameOfOutcome(KeyOutcome outcome) {
  switch (outcome) {
    case KeyOutcome::kPending: return "pending";
    case KeyOutcome::kBinding: return "binding";
    case KeyOutcome::kInsertText: return "insert-text";
    case KeyOutcome::kUnbound: return "unbound";
    case KeyOutcome::kCount: return "count";
    case KeyOutcome::kPrompt: return "prompt";
    case KeyOutcome::kPendingChar: return "pending-char";
    case KeyOutcome::kChordTimeout: return "chord-timeout";
    case KeyOutcome::kOther: break;
  }
  return "other";
}

bool ContentsAreSensitive(std::string_view path) {
  const std::string lower = LowerOf(path);
  const std::size_t slash = lower.find_last_of('/');
  const std::string_view name =
      (slash == std::string::npos) ? std::string_view{lower}
                                   : std::string_view{lower}.substr(slash + 1);
  if (name.empty()) return false;

  if (name.starts_with(".env")) return true;
  if ((name == ".netrc") || (name == ".pgpass") || (name == ".htpasswd")) return true;
  if (name.starts_with("id_rsa") || name.starts_with("id_dsa") || name.starts_with("id_ecdsa") ||
      name.starts_with("id_ed25519")) {
    return true;
  }
  for (const std::string_view suffix : {".pem", ".key", ".p12", ".pfx", ".jks", ".keystore"}) {
    if (name.ends_with(suffix)) return true;
  }
  return false;
}

std::string KeyMapFingerprint(const KeyMaps& maps) {
  std::uint64_t hash = kHashSeed;
  const auto walk = [&hash](auto&& self, const KeyNode& node, const std::string& prefix) -> void {
    for (const auto& [key, child] : node.children) {
      const std::string here = prefix.empty() ? KeyToString(key) : (prefix + " " + KeyToString(key));
      hash = HashBytes(hash, here);
      for (const std::string& name : child.commands) hash = HashBytes(hash, name);
      self(self, child, here);
    }
  };
  hash = HashBytes(hash, "normal");
  walk(walk, maps.normal.Root(), {});
  hash = HashBytes(hash, "insert");
  walk(walk, maps.insert.Root(), {});
  return HexOf(hash);
}

std::shared_ptr<KeyRecorder> KeyRecorder::Open(const fs::path& path, std::string pane,
                                               std::string& error) {
  error.clear();
  if (path.empty()) {
    error = "no key log path";
    return nullptr;
  }
  auto store = std::make_shared<SqliteRecorder>();
  store->pane = std::move(pane);
  // Same order as ProjectStore::Open, for the same reason: the version read is
  // the first thing that touches the file, so it is what turns an unreadable or
  // unwritable database into a refused open instead of a recorder that silently
  // keeps nothing. As there, a true return with `error` set means the old file
  // was corrupt and this is a fresh, empty database in its place.
  if (!OpenAndCheckDatabase(path, store->db, 1, error)) return nullptr;
  store->Exec("PRAGMA synchronous = NORMAL;");
  std::string why;
  if (!ExecSql(store->db, kSchema, &why)) {
    error = "cannot create key log tables: " + why;
    return nullptr;
  }
  if (!StampSchemaVersion(store->db, 1, error)) return nullptr;
  return store;
}

}
