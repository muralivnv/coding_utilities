#include <memory>
#include <limits>
#include <filesystem>
#include <string_view>
#include <cstdint>
#include <charconv>
#include <atomic>
#include <vector>
#include <optional>

#include <sqlite3.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <pwd.h>
#include <csignal>
#include <cctype>
#include <fcntl.h>

#include "args.h"
#include "format.h"
#include "printx.hpp"
#include "bash.h"

namespace fs = std::filesystem;

constexpr const char* kVersion = PROJECT_VERSION; // defined in root CMakeLists.txt

namespace ghatothkacha {

constexpr static std::string_view kDBSchema = R"sql(
  CREATE TABLE IF NOT EXISTS History (
    id TEXT PRIMARY KEY,
    cmd TEXT NOT NULL,
    dir TEXT NOT NULL,
    start_timestamp_ns INTEGER,
    end_timestamp_ns INTEGER,
    retcode INTEGER
  );
  CREATE INDEX IF NOT EXISTS dir_idx ON History(dir);
  CREATE INDEX IF NOT EXISTS time_idx ON History(end_timestamp_ns);
)sql";

constexpr static std::string_view kDBFilename = "ghatothkacha_shell_history.db";
constexpr char kDelimiter = '\x1F';
constexpr const char* kSocketName = "/tmp/ghatothkacha_daemon";
std::atomic<bool> g_daemon_running{true};

auto kCloseDb = [](sqlite3* db) { sqlite3_close(db); };
auto kFinalizeStmt = [](sqlite3_stmt* stmt) { sqlite3_finalize(stmt); };

using SqliteDbPtr = std::unique_ptr<sqlite3, decltype(kCloseDb)>;
using SqliteStmtPtr = std::unique_ptr<sqlite3_stmt, decltype(kFinalizeStmt)>;

struct HistoryItem {
  std::string_view id{""};
  std::string_view cmd{""};
  std::string_view dir{""};
  uint64_t start_timestamp_ns{0};
  uint64_t end_timestamp_ns{0};
  int retcode{0};
};

static void SignalHandler(int /*signum*/) {
  g_daemon_running = false;
}

static fs::path GetDatabasePath() {
  std::string path = "~/.local/share/ronin/";
  path.append(kDBFilename);
  const char* home = getenv("HOME");
  if (!home) {
    home = getpwuid(getuid())->pw_dir;
  }
  path.replace(0, 1, home);
  return fs::path(path);
}

static void EnableWriteAheadLogging(sqlite3* db) {
  char* errmsg = nullptr;
  
  // Enable Write-Ahead Logging
  const char* wal_query = "PRAGMA journal_mode=WAL;";
  if (sqlite3_exec(db, wal_query, nullptr, nullptr, &errmsg) != SQLITE_OK) {
    rostd::printf<"Failed to enable WAL mode: %s\n">(errmsg);
    sqlite3_free(errmsg); 
  }

  // Optimize synchronization (Safe and fast when using WAL)
  const char* sync_query = "PRAGMA synchronous=NORMAL;";
  if (sqlite3_exec(db, sync_query, nullptr, nullptr, &errmsg) != SQLITE_OK) {
    rostd::printf<"Failed to set synchronous mode: %s\n">(errmsg);
    sqlite3_free(errmsg);
  }
}

static SqliteDbPtr CreateDatabaseWithSchema(const fs::path& database) {
  SqliteDbPtr db{nullptr};
  int ret = sqlite3_open(database.c_str(), std::out_ptr(db));
  if (ret != SQLITE_OK) {
    rostd::printf<"Unable to create database at path: %s\n">(database.string());
    db.reset(nullptr);
  } else {
    EnableWriteAheadLogging(db.get());
    char* errmsg;
    ret = sqlite3_exec(db.get(), kDBSchema.data(), nullptr, nullptr, &errmsg);
    if (ret != SQLITE_OK) {
      rostd::printf<"Unable to create schema, exec returned error: %s\n">(errmsg);
      db.reset(nullptr);
    }
  }
  return db;
}

static SqliteDbPtr OpenDatabase(const fs::path& database) {
  SqliteDbPtr db{nullptr};
  if (fs::exists(database)) {
    if (sqlite3_open(database.c_str(), std::out_ptr(db)) == SQLITE_OK) {
      EnableWriteAheadLogging(db.get());
    } else {
      db.reset(nullptr);
    }
  } else {
    db = CreateDatabaseWithSchema(database);
  }
  return db;
}

static SqliteStmtPtr PrepareInsertStmt(SqliteDbPtr& db) {
  std::string_view sql = "INSERT INTO History(id, cmd, dir, start_timestamp_ns) VALUES(?, ?, ?, ?);";
  SqliteStmtPtr stmt{nullptr};
  if (sqlite3_prepare_v2(db.get(), sql.data(), -1, std::out_ptr(stmt), nullptr) != SQLITE_OK) {
    rostd::printf<"Failed to prepare insert statement: %s\n">(sqlite3_errmsg(db.get()));
  }
  return stmt;
}

static SqliteStmtPtr PrepareUpdateStmt(SqliteDbPtr& db) {
  std::string_view sql = "UPDATE History SET end_timestamp_ns = ?, retcode = ? WHERE id = ?;";
  SqliteStmtPtr stmt{nullptr};
  if (sqlite3_prepare_v2(db.get(), sql.data(), -1, std::out_ptr(stmt), nullptr) != SQLITE_OK) {
    rostd::printf<"Failed to prepare update statement: %s\n">(sqlite3_errmsg(db.get()));
  }
  return stmt;
}

static void InitBash() {
  rostd::printf<"%s\n">(kBashPreExec);
}

static void InsertItem(const HistoryItem& item, SqliteStmtPtr& stmt, SqliteDbPtr& db) {
  if (!stmt || !db) return;
  sqlite3_bind_text(stmt.get(), 1, item.id.data(), item.id.size(), SQLITE_STATIC);
  sqlite3_bind_text(stmt.get(), 2, item.cmd.data(), item.cmd.size(), SQLITE_STATIC);
  sqlite3_bind_text(stmt.get(), 3, item.dir.data(), item.dir.size(), SQLITE_STATIC);
  sqlite3_bind_int64(stmt.get(), 4, item.start_timestamp_ns);
  
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    rostd::printf<"Failed to insert item: %s\n">(sqlite3_errmsg(db.get()));
  }
  sqlite3_reset(stmt.get());
  sqlite3_clear_bindings(stmt.get());
}

static void UpdateItem(const HistoryItem& item, SqliteStmtPtr& stmt, SqliteDbPtr& db) {
  if (!stmt || !db) return;
  sqlite3_bind_int64(stmt.get(), 1, item.end_timestamp_ns);
  sqlite3_bind_int(stmt.get(), 2, item.retcode);
  sqlite3_bind_text(stmt.get(), 3, item.id.data(), item.id.size(), SQLITE_STATIC);
  
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    rostd::printf<"Failed to update item: %s\n">(sqlite3_errmsg(db.get()));
  }
  sqlite3_reset(stmt.get());
  sqlite3_clear_bindings(stmt.get());
}

static std::string_view Strip(std::string_view v) {
  // remove leading whitespace
  size_t start = 0;
  while (start < v.size() && std::isspace(v[start])) ++start;
  v.remove_prefix(start);

  // remove trailing whitespace
  while (!v.empty() && std::isspace(v.back())) {
    v.remove_suffix(1);
  }

  return v;
}

template<std::integral T>
static std::optional<T> StringToInt(std::string_view input) {
  T result{};
  auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), result);
  if (ec == std::errc()) {
    return result;
  } else if (ec == std::errc::invalid_argument) {
    rostd::printf<"Input is not a number\n">();
  } else if (ec == std::errc::result_out_of_range) {
    rostd::printf<"Input is larger than the specified type\n">();
  }
  return std::nullopt;
}

static std::optional<HistoryItem> ToInsertItem(std::string_view id, std::string_view cmd,
                                               std::string_view dir, std::string_view start_time_ns) {
  if (Strip(id).empty()) {
    rostd::printf<"Cannot perform history insert, input 'id' is empty\n">();
    return std::nullopt;
  }
  if (Strip(cmd).empty()) {
    rostd::printf<"Cannot perform history insert, input 'cmd' is empty\n">();
    return std::nullopt;
  }
  if (Strip(dir).empty()) {
    rostd::printf<"Cannot perform history insert, input 'dir' is empty\n">();
    return std::nullopt;
  }
  if (Strip(start_time_ns).empty()) {
    rostd::printf<"Cannot perform history insert, input 'start_time_ns' is empty\n">();
    return std::nullopt;
  }
  HistoryItem item;
  item.id = id;
  item.cmd = cmd;
  item.dir = dir;

  { // decode start_time_ns
    auto result = StringToInt<uint64_t>(start_time_ns);
    if (!result) {
      rostd::printf<"Error while decoding 'start_time_ns'\nInput 'start_time_ns': %s">(start_time_ns);
      return std::nullopt;
    }
    item.start_timestamp_ns = result.value();
  }
  return item;
}

static std::optional<HistoryItem> ToUpdateItem(std::string_view id, std::string_view end_time_ns,
                                               std::string_view retcode) {
  if (Strip(id).empty()) {
    rostd::printf<"Cannot perform history update, input 'id' is empty\n">();
    return std::nullopt;
  }
  if (Strip(end_time_ns).empty()) {
    rostd::printf<"Cannot perform history update, input 'end_time_ns' is empty\n">();
    return std::nullopt;
  }
  if (Strip(retcode).empty()) {
    rostd::printf<"Cannot perform history update, input 'retcode' is empty\n">();
    return std::nullopt;
  }

  HistoryItem item;
  item.id = id;

  { // decode end_time_ns
    auto result = StringToInt<uint64_t>(end_time_ns);
    if (!result) {
      rostd::printf<"Error while decoding 'end_time_ns'\nInput 'end_time_ns': %s">(end_time_ns);
      return std::nullopt;
    }
    item.end_timestamp_ns = result.value();
  }

  { // decode retcode
    auto result = StringToInt<int>(retcode);
    if (!result) {
      rostd::printf<"Error while decoding 'retcode'\nInput 'retcode': %s">(retcode);
      return std::nullopt;
    }
    item.retcode = result.value();
  }
  return item;
}

static void SetupSocket(sockaddr_un& addr, socklen_t& addr_len) {
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;

  const char* socket_path = common::FormatIntoCString<"%s_%d.sock">(kSocketName, getuid());
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
  addr_len = sizeof(sa_family_t) + strlen(addr.sun_path);
}

static void Daemon() {
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return;

  struct sockaddr_un addr;
  socklen_t addr_len;
  SetupSocket(addr, addr_len);

  mode_t old_mask = umask(0177); 
  int bind_result = bind(fd, (struct sockaddr*)&addr, addr_len);

  // If the socket file exists, check if an active daemon is listening
  if (bind_result < 0 && errno == EADDRINUSE) {
    int test_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    char dummy = 0;
    
    // sendto returns -1 and sets errno to ECONNREFUSED if no one is listening on the other end
    if (sendto(test_fd, &dummy, 0, 0, (struct sockaddr*)&addr, addr_len) < 0 && errno == ECONNREFUSED) {
      // The socket is stale. Safe to unlink and re-bind.
      unlink(addr.sun_path);
      bind_result = bind(fd, (struct sockaddr*)&addr, addr_len);
    }
    close(test_fd);
  }
  umask(old_mask); 

  if (bind_result < 0) {
    close(fd);
    return; // Daemon is actively running (or another unrecoverable error). Exit silently!
  }

  // SECURE THE SOCKET: Only the owner can read/write
  chmod(addr.sun_path, 0600);

  SqliteDbPtr db_ptr = OpenDatabase(GetDatabasePath());
  if (!db_ptr) return;

  SqliteStmtPtr insert_stmt = PrepareInsertStmt(db_ptr);
  SqliteStmtPtr update_stmt = PrepareUpdateStmt(db_ptr);
  if (!insert_stmt || !update_stmt) return;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SignalHandler;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  char buffer[65536]; 
  while (g_daemon_running) {
    ssize_t bytes_read = recvfrom(fd, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr);
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue; // sigaction fired
      }
      break; // real socket error
    }

    if (bytes_read > 1) {
      buffer[bytes_read] = '\0';
      char action = buffer[0];
      std::string_view payload(buffer + 1, bytes_read - 1);
      
      if (payload.empty() || payload[0] != kDelimiter) continue;
      payload.remove_prefix(1); // Drop the leading delimiter

      std::vector<std::string_view> fields;
      size_t start = 0;
      size_t end = payload.find(kDelimiter);
      while (end != std::string_view::npos) {
        fields.push_back(payload.substr(start, end - start));
        start = end + 1;
        end = payload.find(kDelimiter, start);
      }
      fields.push_back(payload.substr(start));

      try {
        if (action == 'I' && fields.size() == 4) {
          const std::optional<HistoryItem> item =
            ToInsertItem(fields[0], fields[3], fields[2], fields[1]);
          if (item.has_value()) {
            InsertItem(item.value(), insert_stmt, db_ptr);
          }
        } else if (action == 'U' && fields.size() == 3) {
          const std::optional<HistoryItem> item =
            ToUpdateItem(fields[0], fields[1], fields[2]);
          if (item.has_value()) {
            UpdateItem(item.value(), update_stmt, db_ptr);
          }
        }
      } catch (...) {
        // Ignore malformed packets
      }
    }
  }

  close(fd);
  unlink(addr.sun_path);
}

static void SendToDaemon(const HistoryItem& item, bool is_update) {
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return;

  struct sockaddr_un addr;
  socklen_t addr_len;
  SetupSocket(addr, addr_len);

  std::string_view payload;
  if (is_update) {
    payload = common::FormatIntoStringView<"U%?%?%?%?%?%?">(kDelimiter, item.id, kDelimiter,
                                                            item.end_timestamp_ns, kDelimiter,
                                                            item.retcode);
  } else {
    payload = common::FormatIntoStringView<"I%?%?%?%?%?%?%?%?">(kDelimiter, item.id, kDelimiter,
                                                                item.start_timestamp_ns, kDelimiter,
                                                                item.dir, kDelimiter, item.cmd);
  }

  const auto payload_len = payload.size();

  // MSG_DONTWAIT ensures our shell prompt NEVER hangs, even if the daemon is frozen
  ssize_t res = sendto(fd, payload.data(), payload_len, MSG_DONTWAIT, (struct sockaddr*)&addr, addr_len);
  
  // Catch both dead processes (ECONNREFUSED) and missing socket files (ENOENT)
  if (res < 0 && (errno == ECONNREFUSED || errno == ENOENT)) {
    if (fork() == 0) {
      setsid(); // Detach from the terminal

      // Close the inherited socket file descriptor to prevent a leak
      close(fd);

      // Redirect standard streams to /dev/null so the daemon is completely detached
      int dev_null = open("/dev/null", O_RDWR);
      if (dev_null >= 0) {
        dup2(dev_null, STDIN_FILENO);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        if (dev_null > STDERR_FILENO) close(dev_null);
      }

      char exe_path[1024];
      ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
      if (len > 0) {
        exe_path[len] = '\0';
        execl(exe_path, "ghatothkacha", "--daemon", nullptr);
      }
      exit(EXIT_FAILURE);
    }

    // Parent process: give the daemon 10ms to bind, then retry ONCE
    usleep(10000); 
    sendto(fd, payload.data(), payload_len, MSG_DONTWAIT, (struct sockaddr*)&addr, addr_len);
  }

  // If res < 0 and errno == EAGAIN (buffer full / daemon stuck), we do nothing.
  // The command history is lost, but the user's terminal prompt remains fast and responsive. 
  close(fd);
}

static void Bootstrap() {
  try {
    const fs::path db_dir = GetDatabasePath().parent_path();
    std::ignore = fs::create_directories(db_dir);    
  } catch (const fs::filesystem_error& e) {
    std::ignore = e;
  }
}

}  // namespace ghatothkacha

int main(int argc, char** argv) {
  common::Args cli(argc, argv);
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: ghatothkacha [options]

Options:
      --init                Setup Bash
                                Usage: eval "$(ghatothkacha --init)"

      --import              Import bash history into database
                                Usage: eval "$(ghatothkacha --import)"

      --daemon              Launch Daemon (default: false)
      --insert              Insert history item into database on command start
      --update              Update history item on command end
  -i, --id                  Command UUID
  -s, --cmd-start-ns        Command start time in unix epoch time nanoseconds
  -e, --cmd-end-ns          Command end time in unix epoch time nanoseconds
  -c, --cmd                 Command to insert
  -d, --dir                 Command execution directory
  -r, --retcode             Command retcode
      --print-db-path       Print database path of the shell history
  -h, --help                Show this help message
      --version             Print version number
  )CLI";

  if (cli.Has("-h") || cli.Has("--help")) {
    rostd::printf<"%s">(kCliHelpMessage);
    return EXIT_SUCCESS;
  }
  if (cli.Has("--version")) {
    rostd::printf<"%s">(kVersion);
    return EXIT_SUCCESS;
  }

  try {
    if (cli.Has("--init")) {
      ghatothkacha::InitBash();

    } else if (cli.Has("--import")) {
      rostd::printf<"%s">(ghatothkacha::kHistoryImport);

    } else if (cli.Has("--daemon")) {
      ghatothkacha::Bootstrap();
      ghatothkacha::Daemon();

    } else if (cli.Has("--insert")) {
      std::string_view id       = cli.Value({"-i", "--id"}).value_or("");
      std::string_view cmd      = cli.Value({"-c", "--cmd"}).value_or("");
      std::string_view dir      = cli.Value({"-d", "--dir"}).value_or("");
      std::string_view start_ns = cli.Value({"-s", "--cmd-start-ns"}).value_or("");

      const std::optional<ghatothkacha::HistoryItem> item =
            ghatothkacha::ToInsertItem(id, cmd, dir, start_ns);

      if (item.has_value()) {
        ghatothkacha::SendToDaemon(item.value(), false /*is_update*/);
      } else {
        rostd::printf<"Error parsing insert inputs\n">();
      }

    } else if (cli.Has("--update")) {
      std::string_view id       = cli.Value({"-i", "--id"}).value_or("");
      std::string_view end_ns   = cli.Value({"-e", "--cmd-end-ns"}).value_or("");
      std::string_view retcode  = cli.Value({"-r", "--retcode"}).value_or("");

      const std::optional<ghatothkacha::HistoryItem> item =
            ghatothkacha::ToUpdateItem(id, end_ns, retcode);

      if (item.has_value()) {
        ghatothkacha::SendToDaemon(item.value(), true /*is_update*/);
      } else {
        rostd::printf<"Error parsing update inputs\n">();
      }

    } else if (cli.Has("--print-db-path")) {
      rostd::printf<"%s">(ghatothkacha::GetDatabasePath());
    }

  } catch (const std::exception& ex) {
    rostd::printf<"Exception raised!!\nException: %s\n">(ex.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
