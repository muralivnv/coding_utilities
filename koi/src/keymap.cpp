#include "keymap.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <tuple>
#include <unordered_map>

#include <toml++/toml.hpp>

#include "commands.h"
#include "unicode.h"

namespace koi {
namespace {

const std::unordered_map<std::string, NamedKey>& NamedKeyTable() {
  static const std::unordered_map<std::string, NamedKey> table = {
      {"esc", NamedKey::kEsc},         {"ret", NamedKey::kRet},
      {"enter", NamedKey::kRet},
      {"tab", NamedKey::kTab},         {"backspace", NamedKey::kBackspace},
      {"del", NamedKey::kDelete},      {"delete", NamedKey::kDelete},
      {"ins", NamedKey::kInsert},      {"insert", NamedKey::kInsert},
      {"left", NamedKey::kLeft},       {"right", NamedKey::kRight},
      {"up", NamedKey::kUp},           {"down", NamedKey::kDown},
      {"home", NamedKey::kHome},       {"end", NamedKey::kEnd},
      {"pageup", NamedKey::kPageUp},   {"pagedown", NamedKey::kPageDown},
      {"F1", NamedKey::kF1},           {"F2", NamedKey::kF2},
      {"F3", NamedKey::kF3},           {"F4", NamedKey::kF4},
      {"F5", NamedKey::kF5},           {"F6", NamedKey::kF6},
      {"F7", NamedKey::kF7},           {"F8", NamedKey::kF8},
      {"F9", NamedKey::kF9},           {"F10", NamedKey::kF10},
      {"F11", NamedKey::kF11},         {"F12", NamedKey::kF12},
  };
  return table;
}

std::string NamedKeyName(NamedKey key) {
  for (const auto& [name, value] : NamedKeyTable()) {
    if (value == key) {
      static thread_local std::string best;
      best.clear();
      for (const auto& [n2, v2] : NamedKeyTable()) {
        if ((v2 == key) && (best.empty() || (n2.size() < best.size()))) best = n2;
      }
      return best;
    }
  }
  return {};
}

}

bool ParseKey(std::string_view text, Key& out) {
  if (text.empty()) return false;

  Key key;
  for (;;) {
    if (text.size() < 2 || text[1] != '-') break;
    const char m = text[0];
    if (m == 'C') {
      key.mods |= kModCtrl;
    } else if (m == 'A' || m == 'M') {
      key.mods |= kModAlt;
    } else if (m == 'S') {
      key.mods |= kModShift;
    } else {
      break;
    }
    text.remove_prefix(2);
    if (text.empty()) return false;
  }

  const std::string name{text};
  if (const auto it = NamedKeyTable().find(name); it != NamedKeyTable().end()) {
    key.named = it->second;
    out = key;
    return true;
  }
  if (name == "minus") {
    key.code = '-';
    out = key;
    return true;
  }
  if (name == "space") {
    key.code = ' ';
    out = key;
    return true;
  }

  if (NextGraphemeInString(text, 0) != text.size()) return false;
  std::uint32_t cp = 0;
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  size_t len = text.size();
  if (len == 1) {
    cp = bytes[0];
  } else {
    if ((bytes[0] & 0xE0) == 0xC0 && len == 2) {
      cp = ((bytes[0] & 0x1Fu) << 6) | (bytes[1] & 0x3Fu);
    } else if ((bytes[0] & 0xF0) == 0xE0 && len == 3) {
      cp = ((bytes[0] & 0x0Fu) << 12) | ((bytes[1] & 0x3Fu) << 6) | (bytes[2] & 0x3Fu);
    } else if ((bytes[0] & 0xF8) == 0xF0 && len == 4) {
      cp = ((bytes[0] & 0x07u) << 18) | ((bytes[1] & 0x3Fu) << 12) | ((bytes[2] & 0x3Fu) << 6) |
           (bytes[3] & 0x3Fu);
    } else {
      return false;
    }
  }

  if ((key.mods & kModShift) && (cp < 128) && (std::islower(static_cast<int>(cp)) != 0)) {
    cp = static_cast<std::uint32_t>(std::toupper(static_cast<int>(cp)));
  }
  if (key.named == NamedKey::kNone) key.mods &= static_cast<std::uint8_t>(~kModShift);
  key.code = cp;
  out = key;
  return true;
}

std::string KeyToString(const Key& key) {
  std::string out;
  if (key.mods & kModCtrl) out += "C-";
  if (key.mods & kModAlt) out += "A-";
  if (key.mods & kModShift) out += "S-";
  if (key.named != NamedKey::kNone) {
    out += NamedKeyName(key.named);
    return out;
  }
  if (key.code == '-') {
    out += "minus";
    return out;
  }
  if (key.code == ' ') {
    out += "space";
    return out;
  }
  AppendUtf8(out, key.code);
  return out;
}

bool DecodeCsiKey(std::string_view body, Key& out) {
  if (body.size() < 2) return false;
  const char final = body.back();
  if ((final != 'u') && (final != '~')) return false;
  body.remove_suffix(1);

  std::array<std::uint32_t, 3> fields{0, 0, 0};
  std::size_t count = 0;
  std::uint32_t value = 0;
  bool has_digit = false;
  bool in_subparam = false;
  for (const char c : body) {
    if (c == ';') {
      if (!has_digit || (count >= fields.size())) return false;
      fields[count++] = value;
      value = 0;
      has_digit = false;
      in_subparam = false;
      continue;
    }
    if (c == ':') {
      in_subparam = true;
      continue;
    }
    if ((c < '0') || (c > '9')) return false;
    if (in_subparam) continue;
    value = (value * 10) + static_cast<std::uint32_t>(c - '0');
    has_digit = true;
    if (value > 0x10FFFF) return false;
  }
  if (!has_digit || (count >= fields.size())) return false;
  fields[count++] = value;

  std::uint32_t cp = 0;
  std::uint32_t mods = 1;
  if (final == 'u') {
    if ((count < 1) || (count > 2)) return false;
    cp = fields[0];
    if (count == 2) mods = fields[1];
  } else {
    if ((count != 3) || (fields[0] != 27)) return false;
    mods = fields[1];
    cp = fields[2];
  }
  if ((cp > 0x10FFFF) || (mods < 1)) return false;

  const std::uint32_t bits = mods - 1;
  Key key;
  switch (cp) {
    case 8:
    case 127: key.named = NamedKey::kBackspace; break;
    case 9: key.named = NamedKey::kTab; break;
    case 13: key.named = NamedKey::kRet; break;
    case 27: key.named = NamedKey::kEsc; break;
    default:
      if (cp < 32) return false;
      key.code = cp;
      break;
  }
  if (bits & 1u) key.mods |= kModShift;
  if (bits & 2u) key.mods |= kModAlt;
  if (bits & 4u) key.mods |= kModCtrl;
  if ((key.mods & kModShift) && (cp < 128) && (std::islower(static_cast<int>(cp)) != 0)) {
    key.code = static_cast<std::uint32_t>(std::toupper(static_cast<int>(cp)));
  }
  if (key.named == NamedKey::kNone) key.mods &= static_cast<std::uint8_t>(~kModShift);
  out = key;
  return true;
}

bool ParseKeySequence(std::string_view text, std::vector<Key>& out) {
  out.clear();
  size_t i = 0;
  while (i < text.size()) {
    while ((i < text.size()) && (text[i] == ' ')) ++i;
    if (i >= text.size()) break;
    size_t end = text.find(' ', i);
    if (end == std::string_view::npos) end = text.size();
    std::string_view token = text.substr(i, end - i);

    Key key;
    if (ParseKey(token, key)) {
      out.push_back(key);
    } else {
      size_t j = 0;
      while (j < token.size()) {
        const size_t next = NextGraphemeInString(token, j);
        if (!ParseKey(token.substr(j, next - j), key)) {
          out.clear();
          return false;
        }
        out.push_back(key);
        j = next;
      }
    }
    i = end;
  }
  return !out.empty();
}

ErrorCtx KeyMap::Bind(const std::vector<Key>& sequence, std::vector<std::string> commands,
                      std::string* error_detail) {
  if (sequence.empty() || commands.empty()) {
    if (error_detail) *error_detail = "empty key sequence or command list";
    return MakeErrorCtx(PieceTableErrorCode::kEmptyInputString);
  }

  std::string rendered;
  KeyNode* node = &root_;
  for (size_t i = 0; i < sequence.size(); ++i) {
    if (!rendered.empty()) rendered += ' ';
    rendered += KeyToString(sequence[i]);

    if (node->IsLeaf()) {
      if (error_detail) {
        *error_detail = "\"" + rendered + "\" is unreachable: a shorter binding already ends here";
      }
      return MakeErrorCtx(PieceTableErrorCode::kUnknownCmdInfo);
    }
    auto it = std::ranges::find_if(node->children,
                                   [&](const auto& child) { return child.first == sequence[i]; });
    if (it == node->children.end()) {
      node->children.emplace_back(sequence[i], KeyNode{});
      node = &node->children.back().second;
    } else {
      node = &it->second;
    }
  }

  if (!node->children.empty()) {
    if (error_detail) {
      *error_detail = "\"" + rendered + "\" is a prefix of longer bindings and cannot also run a command";
    }
    return MakeErrorCtx(PieceTableErrorCode::kUnknownCmdInfo);
  }
  node->commands = std::move(commands);
  return Success();
}

KeyMap::Lookup KeyMap::Find(const std::vector<Key>& sequence,
                            const std::vector<std::string>** commands) const {
  const KeyNode* node = &root_;
  for (const Key& key : sequence) {
    auto it = std::ranges::find_if(node->children,
                                   [&](const auto& child) { return child.first == key; });
    if (it == node->children.end()) return Lookup::kNoMatch;
    node = &it->second;
  }
  if (node->IsLeaf()) {
    if (commands) *commands = &node->commands;
    return Lookup::kMatched;
  }
  return node->children.empty() ? Lookup::kNoMatch : Lookup::kPending;
}

namespace {

KeyMap* MapForMode(KeyMaps& maps, std::string_view mode) {
  if (mode == "normal") return &maps.normal;
  if (mode == "insert") return &maps.insert;
  return nullptr;
}

std::string At(const toml::node& node) {
  return "line " + std::to_string(node.source().begin.line) + ": ";
}

struct Reader {
  const toml::table& table;
  std::string_view where;
  std::vector<std::string>& errors;

  void Complain(const toml::node& node, std::string_view name, std::string_view expected) const {
    errors.push_back(At(node) + std::string{where} + std::string{name} + " must be " +
                     std::string{expected});
  }

  template <typename T, typename Apply>
  void Get(std::string_view name, std::string_view expected, Apply apply) const {
    const toml::node* node = table.get(name);
    if (node == nullptr) return;
    if (const auto v = node->value<T>()) {
      apply(*v);
    } else {
      Complain(*node, name, expected);
    }
  }

  void Flag(std::string_view name, bool& out) const {
    Get<bool>(name, "true or false", [&out](bool v) { out = v; });
  }

  void Text(std::string_view name, std::string& out,
            std::string_view expected = "a string") const {
    Get<std::string>(name, expected, [&out](const std::string& v) { out = v; });
  }

  void Number(std::string_view name, Index& out, Index lo, Index hi) const {
    Get<std::int64_t>(name, "a whole number", [&out, lo, hi](std::int64_t v) {
      out = std::clamp<Index>(static_cast<Index>(v), lo, hi);
    });
  }
};

std::string UnreachableKeyReason(const Key& key, std::string_view spelling) {
  if ((key.mods & kModCtrl) == 0) return {};
  if (key.named != NamedKey::kNone) return {};
  const std::uint32_t cp = key.code;
  if ((cp == 'i') || (cp == 'm') || (cp == 'h')) {
    const std::string_view alias = (cp == 'i') ? "tab" : (cp == 'm') ? "ret" : "backspace";
    return "\"" + std::string{spelling} + "\" can never fire: the terminal sends the same byte as " +
           std::string{alias} + " -- bind " + std::string{alias} + " instead";
  }
  if ((cp >= 'a') && (cp <= 'z')) return {};
  if ((cp >= 'A') && (cp <= 'Z')) {
    return "\"" + std::string{spelling} +
           "\" can never fire: a terminal sends the same byte for C-" +
           static_cast<char>(cp + ('a' - 'A')) + " and for shift with it";
  }
  return "\"" + std::string{spelling} +
         "\" can never fire: a terminal only sends Ctrl with a letter";
}

void Walk(KeyMap& map, const std::vector<Key>& prefix, const toml::table& table,
          std::vector<std::string>& errors, bool normal_mode) {
  for (auto&& [raw_key, value] : table) {
    Key key;
    if (!ParseKey(raw_key.str(), key)) {
      errors.push_back(At(value) + "unknown key name \"" + std::string{raw_key.str()} + "\"");
      continue;
    }
    if (const std::string why = UnreachableKeyReason(key, raw_key.str()); !why.empty()) {
      errors.push_back(At(value) + why);
      continue;
    }
    if (normal_mode && prefix.empty() && (key.mods == kModNone) &&
        (key.named == NamedKey::kNone) && (key.code >= '1') && (key.code <= '9')) {
      errors.push_back(At(value) + "\"" + std::string{raw_key.str()} +
                       "\" can never fire in normal mode: digits start a count");
      continue;
    }
    std::vector<Key> sequence = prefix;
    sequence.push_back(key);

    if (const toml::table* sub = value.as_table()) {
      Walk(map, sequence, *sub, errors, normal_mode);
      continue;
    }

    std::vector<std::string> commands;
    if (const toml::value<std::string>* str = value.as_string()) {
      commands.push_back(str->get());
    } else if (const toml::array* arr = value.as_array()) {
      bool ok = true;
      for (const toml::node& element : *arr) {
        const toml::value<std::string>* element_str = element.as_string();
        if (element_str == nullptr) {
          errors.push_back(At(element) + "command lists may only contain strings");
          ok = false;
          break;
        }
        commands.push_back(element_str->get());
      }
      if (!ok) continue;
      if (commands.empty()) {
        errors.push_back(At(value) + "empty command list");
        continue;
      }
    } else {
      errors.push_back(At(value) +
                       "a binding must be a command, a list of commands, or a table of keys");
      continue;
    }

    bool known = true;
    for (const std::string& name : commands) {
      if (!name.empty() && (name.front() == ':')) continue;
      if (FindCommand(name) != nullptr) continue;
      known = false;
      errors.push_back(At(value) + (IsKnownUnimplemented(name)
                                        ? ("\"" + name + "\" is a helix command koi does not "
                                                         "implement yet")
                                        : ("unknown command \"" + name + "\"")));
    }
    if (!known) continue;

    std::string detail;
    if (map.Bind(sequence, std::move(commands), &detail)) {
      errors.push_back(At(value) + (detail.empty() ? "could not bind key" : detail));
    }
  }
}

}

namespace {

void ReadSettings(const toml::table& root, Settings& settings, std::vector<std::string>& errors) {
  Reader{root, "", errors}.Text("theme", settings.theme, "a theme name");

  const toml::table* editor = root["editor"].as_table();
  if (editor == nullptr) return;

  const Reader ed{*editor, "", errors};
  ed.Number("tab-width", settings.tab_width, 1, 16);
  ed.Number("scrolloff", settings.scrolloff, 0, 100);
  ed.Flag("indent-spaces", settings.insert_spaces);
  ed.Flag("auto-pairs", settings.auto_pairs);
  ed.Flag("cursorline", settings.cursorline);
  ed.Number("excerpt-context", settings.excerpt_context, 0, 100);
  ed.Number("scan-workers", settings.scan_workers, 1, 32);

  if (const toml::node* node = editor->get("mode-indicator")) {
    if (const auto v = node->value<std::string>(); v && ((*v == "bar") || (*v == "block"))) {
      settings.mode_indicator = *v;
    } else {
      ed.Complain(*node, "mode-indicator", "\"bar\" or \"block\"");
    }
  }

  if (const toml::node* node = editor->get("multi-cursor-paste")) {
    if (const auto v = node->value<std::string>(); v && ((*v == "spread") || (*v == "full"))) {
      settings.multi_cursor_paste_spread = (*v == "spread");
    } else {
      ed.Complain(*node, "multi-cursor-paste", "\"spread\" or \"full\"");
    }
  }

  ed.Text("mode-normal", settings.mode_normal);
  ed.Text("mode-insert", settings.mode_insert);

  if (const toml::node* node = editor->get("icons")) {
    if (const auto v = node->value<bool>()) {
      settings.icons = *v;
    } else if (!node->is_table()) {
      errors.push_back(At(*node) + "icons must be true or false, or an [editor.icons] table");
    }
  }

  ed.Flag("record", settings.record);
  ed.Flag("trim_trailing_whitespace_on_save", settings.trim_trailing_whitespace_on_save);

  ed.Get<std::string>("line-number", "\"relative\" or \"absolute\"",
                      [&settings](const std::string& v) {
                        settings.relative_line_numbers = (v != "absolute");
                      });

  ed.Text("file-filter", settings.file_filter, "a command string");

  if (const toml::table* soft = editor->get_as<toml::table>("soft-wrap")) {
    const Reader wrap{*soft, "soft-wrap.", errors};
    wrap.Flag("enable", settings.soft_wrap);
    wrap.Text("wrap-indicator", settings.wrap_indicator);
    wrap.Number("max-wrap", settings.max_wrap, 0, 1000);
  }

  if (const toml::table* icons = editor->get_as<toml::table>("icons")) {
    const Reader icon{*icons, "icons.", errors};
    icon.Flag("enable", settings.icons);
    icon.Text("error", settings.icon_error);
    icon.Text("warning", settings.icon_warning);
    icon.Text("info", settings.icon_info);
    icon.Text("readonly", settings.icon_readonly);
    icon.Text("modified", settings.icon_modified);
    icon.Text("file", settings.icon_file);
  }

  if (const toml::table* ws = editor->get_as<toml::table>("whitespace")) {
    bool render_named = false;
    const auto render_value = [&](const toml::node& node) {
      const auto v = node.value<std::string>();
      if (!v) {
        errors.push_back(At(node) + "whitespace.render must be \"all\" or \"none\"");
        return;
      }
      render_named = true;
      settings.render_tabs = (*v != "none");
    };
    if (const toml::node* node = ws->get("render")) {
      if (const toml::table* per_kind = node->as_table()) {
        if (const toml::node* tab = per_kind->get("tab")) render_value(*tab);
      } else {
        render_value(*node);
      }
    }
    if (const toml::table* chars = ws->get_as<toml::table>("characters")) {
      const Reader glyphs{*chars, "whitespace.characters.", errors};
      const auto glyph = [&](std::string_view name, std::string& out) {
        const toml::node* node = chars->get(name);
        if (node == nullptr) return false;
        const auto v = node->value<std::string>();
        if (!v || v->empty()) {
          glyphs.Complain(*node, name, "a non-empty string");
          return false;
        }
        out = *v;
        return true;
      };
      const bool named = glyph("tab", settings.tab_glyph);
      std::ignore = glyph("tabpad", settings.tab_pad);
      if (named && !render_named) settings.render_tabs = true;
    }
  }
}

}

ErrorCtx ParseKeyMapConfig(std::string_view text, KeyMaps& maps, Settings& settings,
                           std::vector<std::string>& errors) {
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& err) {
    errors.push_back("line " + std::to_string(err.source().begin.line) + ", column " +
                     std::to_string(err.source().begin.column) + ": " +
                     std::string{err.description()});
    return MakeErrorCtx(PieceTableErrorCode::kUnknownCmdInfo);
  }

  ReadSettings(root, settings, errors);

  const toml::table* keys = root["keys"].as_table();
  if (keys == nullptr) {

    if (root.contains("keys")) {
      errors.push_back(At(*root.get("keys")) + "[keys] must be a table");
    }
    return errors.empty() ? Success() : MakeErrorCtx(PieceTableErrorCode::kUnknownCmdInfo);
  }

  for (auto&& [mode_name, mode_node] : *keys) {
    if (mode_name.str() == "select") {
      errors.push_back(At(mode_node) +
                       "[keys.select] does nothing -- koi has no select mode; extend with the "
                       "capitals in [keys.normal]");
      continue;
    }
    KeyMap* map = MapForMode(maps, mode_name.str());
    if (map == nullptr) {
      errors.push_back(At(mode_node) + "unknown mode \"" + std::string{mode_name.str()} +
                       "\" (normal or insert)");
      continue;
    }
    const toml::table* mode_table = mode_node.as_table();
    if (mode_table == nullptr) {
      errors.push_back(At(mode_node) + "[keys." + std::string{mode_name.str()} +
                       "] must be a table");
      continue;
    }
    Walk(*map, {}, *mode_table, errors, mode_name.str() == "normal");
  }

  return errors.empty() ? Success() : MakeErrorCtx(PieceTableErrorCode::kUnknownCmdInfo);
}

KeyMaps DefaultKeyMaps() {
  static constexpr std::string_view kDefaults = R"TOML(
[keys.normal]
i = "move_line_up"
k = "move_line_down"
u = "move_char_left"
o = "move_char_right"
j = "move_prev_word_start"
l = "move_next_word_start"
I = "extend_line_up"
K = "extend_line_down"
U = "extend_char_left"
O = "extend_char_right"
J = "extend_prev_word_start"
L = "extend_next_long_word_start"
"A-i" = "move_line_up"
"A-k" = "move_line_down"
"A-u" = "move_char_left"
"A-o" = "move_char_right"
"A-j" = "move_prev_long_word_start"
"A-l" = "move_next_long_word_end"
"A-I" = "extend_line_up"
"A-K" = "extend_line_down"
"A-U" = "extend_char_left"
"A-O" = "extend_char_right"
"A-J" = "extend_prev_word_end"
"A-L" = "extend_next_word_start"
p = ["page_cursor_half_up", "align_view_center"]
n = ["page_cursor_half_down", "align_view_center"]

"," = ["goto_line_end", "move_char_right"]
";" = "goto_first_nonwhitespace"
"<" = "extend_to_line_end"
V = ["goto_first_nonwhitespace", "extend_to_line_end"]

f = "extend_next_char"
F = "extend_prev_char"
t = "extend_till_char"
T = "extend_till_prev_char"
y = "leap"
a = "leap"

"\\" = ["vsplit", "align_view_center"]
"|" = ["hsplit", "align_view_center"]
"/" = "search"
N = ["search_next", "align_view_center"]
P = ["search_prev", "align_view_center"]
s = "select_regex"

q = "collapse_selection"
x = "extend_line"
D = "expand_selection"
w = "select_textobject_inner"
W = "select_textobject_around"
"%" = "select_all"
"A-;" = "flip_selections"
C = "copy_selection_on_next_line"
"A-C" = "copy_selection_on_prev_line"
"A-," = "remove_primary_selection"
"_" = "trim_selections"
"A-s" = "split_selection_on_newline"
S = "split_selection"
"(" = "rotate_selections_backward"
")" = "rotate_selections_forward"
"A-(" = "rotate_selection_contents_backward"
"A-)" = "rotate_selection_contents_forward"
"&" = "align_selections"

"A-e" = ["collapse_selection", "insert_mode"]
c = "change_selection"
d = "delete_selection"
r = "replace"
R = ["delete_selection", "paste_clipboard_before"]
tab = "indent"
"S-tab" = "unindent"
"`" = "switch_to_lowercase"
"~" = "switch_to_uppercase"
"C-z" = "undo"
"C-y" = "redo"
"C-c" = "yank_to_clipboard"
"C-x" = ["yank_to_clipboard", "delete_selection"]
"C-v" = "paste_clipboard_before"

"A-v" = "paste_clipboard_after"
"C-s" = ":w"
"C-r" = ":reload"
"A-z" = "toggle_soft_wrap"

"A-q" = "toggle_comments"
"'" = ["file_picker", "align_view_center"]
":" = "command_mode"

esc = ["clear_search_highlight", "keep_primary_selection"]
ret = ["insert_newline", "normal_mode"]
backspace = "delete_char_backward"
del = "delete_char_forward"
up = "move_line_up"
down = "move_line_down"
left = "move_char_left"
right = "move_char_right"
home = "goto_line_start"
end = "goto_line_end"
pageup = ["page_up", "align_view_center"]
pagedown = ["page_down", "align_view_center"]

"+" = "increment_excerpt_context"
"=" = "increment_excerpt_context"
"-" = "decrement_excerpt_context"

[keys.normal."Z"]
i = "scroll_up"
k = "scroll_down"
c = "align_view_center"

[keys.normal.g]
t = "goto_file_start"
b = "goto_last_line"
T = "extend_to_file_start"
B = "extend_to_file_end"
h = "goto_line_start"
l = "goto_line_end"
H = "extend_to_line_start"

d = ["collapse_selection", "expand_selection", "trim_selections", "goto_definition", "align_view_center"]
r = ["collapse_selection", "expand_selection", "trim_selections", "show_references", "align_view_center"]
R = ["collapse_selection", "expand_selection", "trim_selections", "show_reference_excerpts"]
D = ["collapse_selection", "expand_selection", "trim_selections", "show_definition_excerpts"]
o = ["goto_excerpt_source", "align_view_center"]
x = "excerpt_drop"
"." = ["goto_last_edit", "align_view_center"]

[keys.normal."space"]
space = ["last_picker", "align_view_center"]
b = "buffer_picker"
"/" = ["content_picker", "align_view_center"]
"?" = "search_excerpts"
s = ["buffer_symbol_picker", "align_view_center"]
S = ["symbol_picker", "align_view_center"]
"1" = ":jump-pin 1"
"2" = ":jump-pin 2"
"3" = ":jump-pin 3"
"4" = ":jump-pin 4"
i = ":jump-symbol 0"
j = ":jump-symbol 1"
k = ":jump-symbol 2"
l = ":jump-symbol 3"
a = ":jump-symbol 4"
e = ":jump-symbol 5"
o = ":jump-symbol 6"

[keys.normal.e]
l = "jump_view_right"
k = "jump_view_down"
j = "jump_view_left"
i = "jump_view_up"

L = "swap_view_right"
K = "swap_view_down"
J = "swap_view_left"
I = "swap_view_up"

x = ":wclose"
X = ":buffer-close"
t = "transpose_view"

">" = "expand_width"
"<" = "shrink_width"
"+" = "expand_height"
"-" = "shrink_height"

[keys.normal."space"."p"]
"1" = ":pin 1"
"2" = ":pin 2"
"3" = ":pin 3"
"4" = ":pin 4"

[keys.normal."space"."x"]
"1" = ":clear-pin 1"
"2" = ":clear-pin 2"
"3" = ":clear-pin 3"
"4" = ":clear-pin 4"

[keys.normal.m]
m = "match_brackets"
i = "select_textobject_inner"
a = "select_textobject_around"
s = "surround_add"
r = "surround_replace"
d = "surround_delete"

[keys.normal."["]
space = "add_newline_above"
"[" = "jump_backward"
f = "goto_prev_function"
t = "goto_prev_class"
a = "goto_prev_parameter"
c = "goto_prev_comment"
T = "goto_prev_test"
p = "goto_prev_paragraph"

[keys.normal."]"]
space = "add_newline_below"
"]" = "jump_forward"
f = "goto_next_function"
t = "goto_next_class"
a = "goto_next_parameter"
c = "goto_next_comment"
T = "goto_next_test"
p = "goto_next_paragraph"

[keys.insert]
esc = "normal_mode"
"A-e" = "normal_mode"
j = { k = "normal_mode" }
ret = "insert_newline"
tab = ["normal_mode", "indent"]
"S-tab" = ["normal_mode", "unindent"]
backspace = "delete_char_backward"
del = "delete_char_forward"

"A-i" = "move_line_up"
"A-k" = "move_line_down"
"A-u" = "move_char_left"
"A-o" = "move_char_right"
"A-j" = ["move_prev_word_start", "collapse_selection"]
"A-l" = ["move_next_word_start", "collapse_selection"]
"A-I" = ["extend_line_up", "normal_mode"]
"A-K" = ["extend_line_down", "normal_mode"]
"A-U" = ["move_char_left", "normal_mode"]
"A-O" = ["move_char_right", "normal_mode"]
"A-J" = ["move_prev_word_start", "normal_mode"]
"A-L" = ["move_next_word_start", "normal_mode"]
"A-," = ["goto_line_end", "move_char_right"]
"A-;" = "goto_first_nonwhitespace"

"A-." = "no_op"
"A-h" = "no_op"
"A-H" = "no_op"
"A-m" = "no_op"
"A-M" = "no_op"
"A-n" = "no_op"
"A-p" = "no_op"
"C-z" = ["normal_mode", "undo"]
"C-y" = ["normal_mode", "redo"]
"C-c" = "yank_to_clipboard"
"C-x" = ["yank_to_clipboard", "delete_selection"]
"C-v" = "paste_clipboard_before"
"A-v" = "paste_clipboard_after"
"C-s" = [":w", "normal_mode"]
up = "move_line_up"
down = "move_line_down"
left = "move_char_left"
right = "move_char_right"
)TOML";

  KeyMaps maps;
  std::vector<std::string> errors;
  std::ignore = ParseKeyMapConfig(kDefaults, maps, errors);
  return maps;
}

ErrorCtx ParseKeyMapConfig(std::string_view text, KeyMaps& maps,
                           std::vector<std::string>& errors) {
  Settings ignored;
  return ParseKeyMapConfig(text, maps, ignored, errors);
}

ErrorCtx LoadKeyMapConfig(const std::filesystem::path& path, KeyMaps& maps,
                          std::vector<std::string>& errors) {
  Settings ignored;
  return LoadKeyMapConfig(path, maps, ignored, errors);
}

ErrorCtx LoadKeyMapConfig(const std::filesystem::path& path, KeyMaps& maps, Settings& settings,
                          std::vector<std::string>& errors) {
  std::ifstream in(path);
  if (!in) {
    errors.push_back("cannot read " + path.string());
    return MakeErrorCtx(PieceTableErrorCode::kEmptyInputString);
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return ParseKeyMapConfig(text, maps, settings, errors);
}

}
