#include "query.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <unordered_set>

#include "regex.h"

#ifndef KOI_GRAMMAR_DIR
#define KOI_GRAMMAR_DIR ""
#endif
#ifndef KOI_RUNTIME_DIR
#define KOI_RUNTIME_DIR ""
#endif

namespace koi {
namespace {

namespace fs = std::filesystem;

struct LanguageDef {
  std::string_view name;
  std::string_view extensions;
  std::string_view filenames;
  std::string_view comment;
};

constexpr std::array kLanguages{
    LanguageDef{"bash", ".sh .bash .zsh",
                ".bashrc .bash_profile .bash_aliases .bash_logout .profile .zshrc .zshenv PKGBUILD",
                "#"},
    LanguageDef{"c", ".c", "", "//"},
    LanguageDef{"cmake", ".cmake", "CMakeLists.txt", "#"},
    LanguageDef{"cpp", ".cpp .cc .cxx .c++ .hpp .hh .hxx .h++ .h .ipp .tpp .inl", "", "//"},
    LanguageDef{"css", ".css", "", "/*"},
    LanguageDef{"dart", ".dart", "", "//"},
    LanguageDef{"diff", ".diff .patch", "", ""},
    LanguageDef{"go", ".go", "go.mod go.sum", "//"},
    LanguageDef{"html", ".html .htm .xhtml", "", ""},
    LanguageDef{"javascript", ".js .mjs .cjs .jsx", "", "//"},
    LanguageDef{"json", ".json .jsonc", "", "//"},
    LanguageDef{"make", ".mk .make", "Makefile makefile GNUmakefile", "#"},
    LanguageDef{"markdown", ".md .markdown .mdown .mkd", "", ""},
    LanguageDef{"nix", ".nix", "", "#"},
    LanguageDef{"python", ".py .pyi .pyw", "", "#"},
    LanguageDef{"rust", ".rs", "", "//"},
    LanguageDef{"toml", ".toml", "Cargo.lock", "#"},
    LanguageDef{"tsx", ".tsx", "", "//"},
    LanguageDef{"typescript", ".ts .mts .cts", "", "//"},
    LanguageDef{"yaml", ".yaml .yml", "", "#"},
};

bool ListContains(std::string_view list, std::string_view needle) {
  if (needle.empty()) return false;
  size_t at = 0;
  while (at < list.size()) {
    const size_t end = std::min(list.find(' ', at), list.size());
    if (list.substr(at, end - at) == needle) return true;
    at = end + 1;
  }
  return false;
}

std::string Lowered(std::string_view s) {
  std::string out{s};
  for (char& c : out) {
    if ((c >= 'A') && (c <= 'Z')) c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

fs::path ExecutableDir() {
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::path{} : exe.parent_path();
}

// `read_error` names the first runtime query file that was on disk but could
// not be read. A query that silently loses one of its inherited files compiles
// into a query that matches less, which shows up as an editor that has stopped
// finding half its symbols and nothing to say why.
std::string LoadQuerySource(std::string_view language, std::string_view file,
                            std::unordered_set<std::string>& seen, std::string& read_error) {
  if (!seen.insert(std::string{language} + "/" + std::string{file}).second) return {};
  const fs::path found = FindRuntimeFile(fs::path{"queries"} / language / file);
  if (found.empty()) return {};

  std::error_code read_ec;
  std::string source = ReadWholeFile(found, read_ec);
  if (read_ec) {
    if (read_error.empty()) {
      read_error = "cannot read " + found.string() + ": " + read_ec.message();
    }
    return {};
  }
  std::string out;
  out.reserve(source.size());

  size_t at = 0;
  while (at <= source.size()) {
    const size_t eol = std::min(source.find('\n', at), source.size());
    std::string_view line{source.data() + at, eol - at};

    std::string_view rest = line;
    while (!rest.empty() && ((rest.front() == ' ') || (rest.front() == '\t'))) rest.remove_prefix(1);
    size_t semis = 0;
    while ((semis < rest.size()) && (rest[semis] == ';')) ++semis;
    bool handled = false;
    if (semis > 0) {
      std::string_view tail = rest.substr(semis);
      while (!tail.empty() && (tail.front() == ' ')) tail.remove_prefix(1);
      if (tail.starts_with("inherits")) {
        tail.remove_prefix(std::string_view{"inherits"}.size());
        while (!tail.empty() && ((tail.front() == ' ') || (tail.front() == ':'))) {
          tail.remove_prefix(1);
        }
        while (!tail.empty() && ((tail.back() == ' ') || (tail.back() == '\r'))) {
          tail.remove_suffix(1);
        }
        size_t from = 0;
        while (from <= tail.size()) {
          const size_t comma = std::min(tail.find(',', from), tail.size());
          const std::string_view parent = tail.substr(from, comma - from);
          if (!parent.empty()) {
            out += '\n';
            out += LoadQuerySource(parent, file, seen, read_error);
            out += '\n';
          }
          from = comma + 1;
        }
        handled = true;
      }
    }
    if (!handled) {
      out.append(line);
      out += '\n';
    }
    if (eol == source.size()) break;
    at = eol + 1;
  }
  return out;
}

struct Predicate {
  enum class Op : std::uint8_t {
    kEq,
    kAnyOf,
    kMatch,
    kKindEq,
    kSameLine,
    kOneLine,
    kAlways,
  };
  Op op{Op::kAlways};
  bool negate{false};
  std::uint32_t capture{0};
  bool against_capture{false};
  std::uint32_t other_capture{0};
  std::vector<std::string> literals;
  int regex{-1};
};

std::string QueryErrorText(TSQueryError error, uint32_t offset, std::string_view source) {
  std::string_view what = "syntax";
  switch (error) {
    case TSQueryErrorNone: what = "none"; break;
    case TSQueryErrorSyntax: what = "syntax"; break;
    case TSQueryErrorNodeType: what = "unknown node type"; break;
    case TSQueryErrorField: what = "unknown field"; break;
    case TSQueryErrorCapture: what = "unknown capture"; break;
    case TSQueryErrorStructure: what = "bad structure"; break;
    case TSQueryErrorLanguage: what = "language mismatch"; break;
  }
  Index line = 1;
  size_t line_start = 0;
  for (size_t i = 0; (i < offset) && (i < source.size()); ++i) {
    if (source[i] == '\n') {
      ++line;
      line_start = i + 1;
    }
  }
  const size_t line_end = std::min(source.find('\n', line_start), source.size());
  return std::string{what} + " at line " + std::to_string(line) + ": " +
         std::string{source.substr(line_start, line_end - line_start)};
}

}

struct CompiledQuery {
  const TSLanguage* language{nullptr};
  TSQuery* query{nullptr};
  std::vector<std::string> capture_names;
  std::vector<std::vector<Predicate>> predicates;
  std::vector<std::vector<QueryProperty>> properties;
  // Parallel to `properties`: whether that pattern set `scope` to `header`.
  std::vector<bool> header_scope;
  std::vector<gai::Pcre2Regex> regexes;

  ~CompiledQuery() {
    if (query != nullptr) ts_query_delete(query);
  }
};

const TSLanguage* LanguageOf(const CompiledQuery& compiled) { return compiled.language; }
TSQuery* QueryOf(const CompiledQuery& compiled) { return compiled.query; }
std::span<const std::string> CaptureNamesOf(const CompiledQuery& compiled) {
  return compiled.capture_names;
}

std::span<const QueryProperty> PropertiesFor(const CompiledQuery& compiled,
                                             std::uint32_t pattern_index) {
  if (pattern_index >= compiled.properties.size()) return {};
  return compiled.properties[pattern_index];
}

bool ScopeIsHeader(const CompiledQuery& compiled, std::uint32_t pattern_index) {
  if (pattern_index >= compiled.header_scope.size()) return false;
  return compiled.header_scope[pattern_index];
}

namespace {

void ParsePredicates(CompiledQuery& compiled) {
  const uint32_t patterns = ts_query_pattern_count(compiled.query);
  compiled.predicates.resize(patterns);
  compiled.properties.resize(patterns);
  compiled.header_scope.assign(patterns, false);

  for (uint32_t pattern = 0; pattern < patterns; ++pattern) {
    uint32_t steps = 0;
    const TSQueryPredicateStep* step = ts_query_predicates_for_pattern(compiled.query, pattern,
                                                                      &steps);
    uint32_t at = 0;
    while (at < steps) {
      uint32_t end = at;
      while ((end < steps) && (step[end].type != TSQueryPredicateStepTypeDone)) ++end;
      if ((end > at) && (step[at].type == TSQueryPredicateStepTypeString)) {
        uint32_t length = 0;
        const char* raw = ts_query_string_value_for_id(compiled.query, step[at].value_id, &length);
        const std::string_view name{raw, length};

        // `#set!` is not a predicate and must not end up in the predicate list
        // even as a no-op: it carries the scope an indent capture applies at,
        // which is a property of the pattern, and a pattern whose only
        // parenthesis is a `#set!` still matches everything it names. Both
        // shapes of it reach here as the same flat run of steps -- the key and
        // the value are strings either way, and the capture-scoped form simply
        // has a capture step in front of them.
        if (name == "set!") {
          QueryProperty property;
          uint32_t strings = 0;
          for (uint32_t i = at + 1; i < end; ++i) {
            // Skipped rather than stored: the capture-scoped shape is read
            // pattern-wide by every consumer -- see QueryProperty -- and what a
            // capture step must not do is count as the key. `(#set! @x "a")` and
            // `(#set! "a")` set the same thing here.
            if (step[i].type == TSQueryPredicateStepTypeCapture) continue;
            uint32_t value_length = 0;
            const char* value =
                ts_query_string_value_for_id(compiled.query, step[i].value_id, &value_length);
            if (strings == 0) {
              property.key.assign(value, value_length);
            } else if (strings == 1) {
              property.value.assign(value, value_length);
            }
            ++strings;
          }
          // A `#set!` with no key at all names nothing and is dropped rather
          // than stored as an empty-keyed property no lookup can ask for.
          if (strings > 0) {
            if ((property.key == "scope") && (property.value == "header")) {
              compiled.header_scope[pattern] = true;
            }
            compiled.properties[pattern].push_back(std::move(property));
          }
          at = end + 1;
          continue;
        }

        Predicate predicate;
        std::string_view base = name;
        if (base.starts_with("not-")) {
          predicate.negate = true;
          base.remove_prefix(4);
        }
        if (base == "eq?") {
          predicate.op = Predicate::Op::kEq;
        } else if (base == "any-of?") {
          predicate.op = Predicate::Op::kAnyOf;
        } else if (base == "match?") {
          predicate.op = Predicate::Op::kMatch;
        } else if (base == "kind-eq?") {
          predicate.op = Predicate::Op::kKindEq;
        } else if (base == "same-line?") {
          predicate.op = Predicate::Op::kSameLine;
        } else if (base == "one-line?") {
          predicate.op = Predicate::Op::kOneLine;
        } else {
          predicate.op = Predicate::Op::kAlways;
        }

        bool first_capture = true;
        for (uint32_t i = at + 1; i < end; ++i) {
          if (step[i].type == TSQueryPredicateStepTypeCapture) {
            if (first_capture) {
              predicate.capture = step[i].value_id;
              first_capture = false;
            } else {
              predicate.against_capture = true;
              predicate.other_capture = step[i].value_id;
            }
          } else {
            uint32_t value_length = 0;
            const char* value =
                ts_query_string_value_for_id(compiled.query, step[i].value_id, &value_length);
            predicate.literals.emplace_back(value, value_length);
          }
        }

        if ((predicate.op == Predicate::Op::kMatch) && !predicate.literals.empty()) {

          try {
            gai::Pcre2Compiled code = gai::Compile(predicate.literals.front(), true, true);
            if (code.p != nullptr) {
              compiled.regexes.push_back(gai::Regex(std::move(code)));
              predicate.regex = static_cast<int>(compiled.regexes.size()) - 1;
            } else {
              predicate.op = Predicate::Op::kAlways;
            }
          } catch (const std::exception&) {
            predicate.op = Predicate::Op::kAlways;
          }
        }
        if (first_capture) predicate.op = Predicate::Op::kAlways;
        // `(#kind-eq? @x)` names no kind to be equal to, so every node fails
        // it and the negated spelling passes everything -- neither of which is
        // what a query file that wrote that meant. Ignored like any other
        // predicate that cannot be evaluated.
        if ((predicate.op == Predicate::Op::kKindEq) && predicate.literals.empty()) {
          predicate.op = Predicate::Op::kAlways;
        }
        // `#same-line?` is about two nodes and has nothing to compare with one.
        if ((predicate.op == Predicate::Op::kSameLine) && !predicate.against_capture) {
          predicate.op = Predicate::Op::kAlways;
        }

        compiled.predicates[pattern].push_back(std::move(predicate));
      }
      at = end + 1;
    }
  }
}

}

std::string ReadWholeFile(const fs::path& path, std::error_code& error) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    error.assign(errno, std::generic_category());
    return {};
  }
  struct stat meta{};
  if (fstat(fd, &meta) != 0) {
    error.assign(errno, std::generic_category());
    close(fd);
    return {};
  }
  if (!S_ISREG(meta.st_mode)) {
    error = std::make_error_code(S_ISDIR(meta.st_mode) ? std::errc::is_a_directory
                                                       : std::errc::not_supported);
    close(fd);
    return {};
  }
  error.clear();

  // A read that fails part-way used to stop and hand back the bytes it already
  // had, with `error` still clear. Nothing downstream could tell that from a
  // file that is genuinely that short -- and the shortest such lie, the empty
  // string, is the one every caller reads as "there is nothing here". The
  // excerpt writeback took it literally: it spliced the edited hunk into the
  // truncated image and wrote that back, deleting every byte the failed read
  // never reached. So a failed read is now an error and no bytes at all;
  // partial content never leaves this function as success.
  int failed = 0;
  const auto fill = [fd, &failed](char* into, std::size_t want) {
    // EINTR is the transient a raw read has to survive. Bounded, and reset by
    // any byte of progress, so a signal storm cannot spin here forever.
    constexpr int kMaxInterrupts = 64;
    int interrupts = 0;
    std::size_t done = 0;
    while (done < want) {
      const ssize_t n = read(fd, into + done, want - done);
      if (n < 0) {
        if ((errno == EINTR) && (++interrupts <= kMaxInterrupts)) continue;
        failed = errno;
        break;
      }
      if (n == 0) break;  // EOF: the file is shorter than fstat said it was.
      done += static_cast<std::size_t>(n);
      interrupts = 0;
    }
    return done;
  };

  std::string out;
  if (meta.st_size > 0) {
    out.resize_and_overwrite(static_cast<std::size_t>(meta.st_size), fill);
  }
  // Past st_size: a file that grew between the fstat and here, and /proc-like
  // files that report zero. Skipped once a read has failed -- there is nothing
  // left to build.
  char tail[8192];
  while (failed == 0) {
    const std::size_t got = fill(tail, sizeof(tail));
    if (got == 0) break;
    out.append(tail, got);
  }
  close(fd);
  if (failed != 0) {
    error.assign(failed, std::generic_category());
    return {};
  }
  return out;
}

std::string_view LanguageForPath(const fs::path& path) {
  const std::string filename = path.filename().string();
  for (const LanguageDef& def : kLanguages) {
    if (ListContains(def.filenames, filename)) return def.name;
  }
  const std::string extension = Lowered(path.extension().string());
  if (extension.empty()) return {};
  for (const LanguageDef& def : kLanguages) {
    if (ListContains(def.extensions, extension)) return def.name;
  }
  return {};
}

std::string_view CommentTokenFor(std::string_view language) {
  for (const LanguageDef& def : kLanguages) {
    if (def.name == language) return def.comment;
  }
  return {};
}

std::span<const std::string_view> KnownLanguages() {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> out;
    for (const LanguageDef& def : kLanguages) out.push_back(def.name);
    return out;
  }();
  return names;
}

std::vector<fs::path> RuntimeRoots() {
  std::vector<fs::path> roots;
  if (const char* home = std::getenv("HOME"); (home != nullptr) && (*home != '\0')) {
    roots.push_back(fs::path{home} / ".config" / "ronin" / "koi");
  }
  if (const fs::path exe = ExecutableDir(); !exe.empty()) {
    roots.push_back(exe / ".." / "share" / "koi");
  }
  if (std::string_view{KOI_RUNTIME_DIR}.size() > 0) roots.emplace_back(KOI_RUNTIME_DIR);
  return roots;
}

fs::path FindRuntimeFile(const fs::path& relative) {
  for (const fs::path& root : RuntimeRoots()) {
    const fs::path candidate = root / relative;
    std::error_code ec;
    if (fs::exists(candidate, ec)) return candidate;
  }
  return {};
}

const TSLanguage* LoadGrammar(std::string_view name, std::string& error) {
  thread_local std::vector<std::pair<std::string, const TSLanguage*>> loaded;
  for (const auto& [known, language] : loaded) {
    if (known == name) return language;
  }

  const std::string file = "libtree-sitter-" + std::string{name} + ".so";
  std::vector<fs::path> tried;
  for (const fs::path& root : RuntimeRoots()) tried.push_back(root / "grammars" / file);
  if (const fs::path exe = ExecutableDir(); !exe.empty()) {
    tried.push_back(exe / ".." / "lib" / file);
    tried.push_back(exe / file);
  }
  if (std::string_view{KOI_GRAMMAR_DIR}.size() > 0) tried.push_back(fs::path{KOI_GRAMMAR_DIR} / file);

  for (const fs::path& candidate : tried) {
    std::error_code ec;
    if (!fs::exists(candidate, ec)) continue;
    void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      error = dlerror();
      continue;
    }
    const std::string symbol = "tree_sitter_" + std::string{name};
    auto fn = reinterpret_cast<const TSLanguage* (*)()>(dlsym(handle, symbol.c_str()));
    if (fn == nullptr) {
      error = "no " + symbol + " in " + candidate.string();
      dlclose(handle);
      continue;
    }
    const TSLanguage* language = fn();
    loaded.emplace_back(name, language);
    return language;
  }
  if (error.empty()) error = "no " + file + " on the runtime path";
  return nullptr;
}

std::shared_ptr<CompiledQuery> CompileQuery(std::string_view language,
                                            std::span<const std::string_view> files,
                                            std::string& error) {
  std::string key{language};
  for (const std::string_view file : files) {
    key += '\0';
    key += file;
  }
  // Failures are cached as hard as successes, and carry the message that
  // explains them. A query that does not compile does not start compiling
  // halfway through a project scan: without a negative entry every scanned file
  // repeated the whole failing attempt -- read the .scm off disk, splice its
  // inherits, hand it to ts_query_new, watch it fail -- so one malformed user
  // query cost a file read and a query build per source in the project, and the
  // bigger the project the more it cost. Replaying the stored message rather
  // than returning a bare nullptr is what keeps that honest: callers that clear
  // their error string and ask again -- the highlighter does, per keystroke --
  // must not get "no" with nothing to show the user the second time.
  //
  // Sticky until restart, like the grammar and query-source caches around it: a
  // .scm fixed under a running koi is not picked up. That is the existing
  // contract for a query that compiled once, and the failing one now matches it.
  struct Entry {
    std::string key;
    std::shared_ptr<CompiledQuery> compiled;
    std::string error;
  };
  thread_local std::vector<Entry> cache;
  for (const Entry& entry : cache) {
    if (entry.key != key) continue;
    if (entry.compiled == nullptr) error = entry.error;
    return entry.compiled;
  }
  const auto remember_failure = [&key, &error] {
    cache.emplace_back(key, nullptr, error);
    return nullptr;
  };

  const TSLanguage* grammar = LoadGrammar(language, error);
  if (grammar == nullptr) return remember_failure();

  std::string source;
  std::string read_error;
  for (const std::string_view file : files) {
    std::unordered_set<std::string> seen;
    source += LoadQuerySource(language, file, seen, read_error);
    source += '\n';
  }
  if (!read_error.empty()) {
    error = read_error;
    // Deliberately not remembered: the cache above never expires, so caching a
    // read that failed once would keep the language broken for the life of the
    // thread. The next compile of the same key reads the file again.
    return nullptr;
  }
  if (source.find_first_not_of(" \t\r\n") == std::string::npos) {
    error = "no queries/" + std::string{language} + "/" +
            std::string{files.empty() ? std::string_view{"*"} : files.front()} +
            " on the runtime path";
    return remember_failure();
  }

  uint32_t error_offset = 0;
  TSQueryError error_type = TSQueryErrorNone;
  TSQuery* query = ts_query_new(grammar, source.data(), static_cast<uint32_t>(source.size()),
                                &error_offset, &error_type);
  if (query == nullptr) {
    error = std::string{language} + " " +
            std::string{files.empty() ? std::string_view{"query"} : files.front()} + ": " +
            QueryErrorText(error_type, error_offset, source);
    return remember_failure();
  }

  auto compiled = std::make_shared<CompiledQuery>();
  compiled->language = grammar;
  compiled->query = query;
  const uint32_t captures = ts_query_capture_count(query);
  compiled->capture_names.reserve(captures);
  for (uint32_t i = 0; i < captures; ++i) {
    uint32_t length = 0;
    const char* name = ts_query_capture_name_for_id(query, i, &length);
    compiled->capture_names.emplace_back(name, length);
  }
  ParsePredicates(*compiled);

  cache.emplace_back(key, compiled, std::string{});
  return compiled;
}

bool PredicatesHold(const CompiledQuery& compiled, const TSQueryMatch& match, NodeText text,
                    const void* ctx) {
  if (match.pattern_index >= compiled.predicates.size()) return true;
  const std::vector<Predicate>& list = compiled.predicates[match.pattern_index];
  if (list.empty()) return true;

  std::string scratch;
  std::string other_scratch;

  const auto captured = [&match](std::uint32_t index) -> const TSQueryCapture* {
    for (uint16_t i = 0; i < match.capture_count; ++i) {
      if (match.captures[i].index == index) return &match.captures[i];
    }
    return nullptr;
  };

  for (const Predicate& predicate : list) {
    if (predicate.op == Predicate::Op::kAlways) continue;

    // Answered from the tree alone, before anything asks for text: a node's
    // kind is in the grammar, not in the buffer, which is what lets an indent
    // query -- one byte at the caret, once per cursor per keystroke -- read
    // `(#not-kind-eq? @indent "compound_statement")` without a read of the
    // document behind it.
    //
    // Every node captured under that name has to agree, where helix looks at
    // the first one only. The two answers differ only for a quantified
    // capture, which the indent queries do not have, and requiring all of them
    // is the reading that does not silently ignore the rest.
    if (predicate.op == Predicate::Op::kKindEq) {
      bool captured_any = false;
      bool ok = true;
      for (uint16_t i = 0; (i < match.capture_count) && ok; ++i) {
        if (match.captures[i].index != predicate.capture) continue;
        captured_any = true;
        const char* kind = ts_node_type(match.captures[i].node);
        ok = (kind != nullptr) &&
             std::ranges::any_of(predicate.literals,
                                 [kind](const std::string& one) { return one == kind; });
      }
      // A capture the match never produced -- an optional one that was not
      // there -- leaves the predicate with nothing to judge, and is skipped
      // rather than being made to decide the match either way.
      if (captured_any && (ok == predicate.negate)) return false;
      continue;
    }

    // `#not-same-line? @indent @expr-start` and `#not-one-line? @item`: the two
    // other things an indent query asks that the tree already knows. Rows come
    // off the node's own points, so these cost no document read either -- and
    // the row is the tree's, which is the line the node was parsed on and not a
    // display line.
    //
    // A named capture that the match did not produce fails the predicate
    // whatever its spelling, including the negated one, which is helix's
    // reading: `#not-same-line?` asserts something about two nodes, and with
    // one of them missing there is nothing for it to be true of. That is the
    // opposite of `#not-kind-eq?` above, deliberately, and both halves are
    // helix's.
    if ((predicate.op == Predicate::Op::kSameLine) || (predicate.op == Predicate::Op::kOneLine)) {
      const bool pairwise = (predicate.op == Predicate::Op::kSameLine);
      const TSQueryCapture* one = captured(predicate.capture);
      const TSQueryCapture* two = pairwise ? captured(predicate.other_capture) : one;
      if ((one == nullptr) || (two == nullptr)) return false;
      const uint32_t from = ts_node_start_point(one->node).row;
      const uint32_t to =
          pairwise ? ts_node_start_point(two->node).row : ts_node_end_point(two->node).row;
      if ((from == to) == predicate.negate) return false;
      continue;
    }

    const TSQueryCapture* found = captured(predicate.capture);
    if (found == nullptr) continue;

    const std::string_view value = text(ctx, found->node, scratch);
    bool ok = false;
    switch (predicate.op) {
      case Predicate::Op::kEq:
        if (predicate.against_capture) {
          const TSQueryCapture* other = captured(predicate.other_capture);
          ok = (other != nullptr) && (value == text(ctx, other->node, other_scratch));
        } else {
          ok = !predicate.literals.empty() && (value == predicate.literals.front());
        }
        break;
      case Predicate::Op::kAnyOf:
        ok = std::ranges::any_of(predicate.literals,
                                 [value](const std::string& one) { return value == one; });
        break;
      case Predicate::Op::kMatch:
        ok = (predicate.regex >= 0) &&
             gai::Find(compiled.regexes[static_cast<size_t>(predicate.regex)], value);
        break;
      case Predicate::Op::kKindEq:
      case Predicate::Op::kSameLine:
      case Predicate::Op::kOneLine:
      case Predicate::Op::kAlways:
        ok = true;
        break;
    }
    if (ok == predicate.negate) return false;
  }
  return true;
}

namespace {

// One rule for both callbacks below. tree-sitter already calls a progress
// callback only every so many operations; `mask` is a second layer on top of
// that, so the clock is read once per many calls and not once per call.
// `expired` latches, so a caller that keeps going after a timeout is stopped
// again without another clock read -- and a cancel is *not* a timeout, which is
// why it returns true without setting the flag.
//
// The mask has to be chosen against how much work one call stands for, which is
// why it is an argument and not a constant: a parse step is bounded, but a
// query-cursor operation carries every match still in progress with it, so on a
// deep tree one of them costs orders of magnitude more than the other and the
// same mask would overshoot the deadline threefold.
bool DeadlinePassed(Deadline& deadline, std::uint32_t mask) {
  if (deadline.expired) return true;

  if ((++deadline.steps & mask) != 0) return false;

  if ((deadline.cancel != nullptr) && deadline.cancel->load(std::memory_order_relaxed)) {
    return true;
  }

  if (std::chrono::steady_clock::now() < deadline.until) return false;
  deadline.expired = true;
  return true;
}

}

bool StopAtDeadline(TSParseState* state) {
  return DeadlinePassed(*static_cast<Deadline*>(state->payload), 0x3F);
}

// The query-cursor twin of StopAtDeadline. Running a query is not free just
// because the parse was: matching is quadratic in the depth of the tree over a
// byte range that every one of those ancestors contains, so a generated file
// that parses well inside its budget can spend a second matching highlights
// over one viewport -- synchronously, inside the draw.
bool StopQueryAtDeadline(TSQueryCursorState* state) {
  return DeadlinePassed(*static_cast<Deadline*>(state->payload), 0x03);
}

bool TreeSitterByteRange(std::size_t bytes, std::uint32_t& end) {
  if (bytes > std::numeric_limits<std::uint32_t>::max()) return false;
  end = static_cast<std::uint32_t>(bytes);
  return true;
}

std::string_view NodeTextIn(const void* ctx, TSNode node, std::string&) {
  const auto* text = static_cast<const std::string_view*>(ctx);
  const auto from = static_cast<size_t>(ts_node_start_byte(node));
  const auto to = static_cast<size_t>(ts_node_end_byte(node));
  if ((to <= from) || (to > text->size())) return {};
  return text->substr(from, to - from);
}

ParsedBuffer ParseBuffer(TSParser* parser, const CompiledQuery& compiled,
                         std::string_view language, std::string_view text,
                         std::chrono::milliseconds budget,
                         const std::atomic<bool>* cancel) {
  ParsedBuffer out;
  if (parser == nullptr) return out;

  ts_parser_reset(parser);
  if (!ts_parser_set_language(parser, LanguageOf(compiled))) {
    out.grammar_error = "grammar rejected by this tree-sitter: " + std::string{language};
    return out;
  }

  Deadline deadline{std::chrono::steady_clock::now() + budget, cancel};
  TSParseOptions options{};
  options.payload = &deadline;
  options.progress_callback = StopAtDeadline;

  TSInput input{};
  input.payload = const_cast<std::string_view*>(&text);
  input.encoding = TSInputEncodingUTF8;
  input.read = [](void* payload, uint32_t byte_index, TSPoint, uint32_t* bytes_read) -> const char* {
    const auto* contents = static_cast<const std::string_view*>(payload);
    if (byte_index >= contents->size()) {
      *bytes_read = 0;
      return "";
    }
    *bytes_read = static_cast<uint32_t>(contents->size() - byte_index);
    return contents->data() + byte_index;
  };

  out.tree = TreePtr{ts_parser_parse_with_options(parser, nullptr, input, options), ts_tree_delete};
  out.timed_out = (out.tree == nullptr) && deadline.expired;
  return out;
}

}
