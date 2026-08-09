#ifndef KOI_KEYMAP_H_
#define KOI_KEYMAP_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor.h"
#include "error.h"

namespace koi {

enum class NamedKey : std::uint16_t {
  kNone = 0,
  kEsc, kRet, kTab, kBackspace, kDelete, kInsert,
  kLeft, kRight, kUp, kDown, kHome, kEnd, kPageUp, kPageDown,
  kF1, kF2, kF3, kF4, kF5, kF6, kF7, kF8, kF9, kF10, kF11, kF12,
};

enum KeyMod : std::uint8_t {
  kModNone = 0,
  kModCtrl = 1 << 0,
  kModAlt = 1 << 1,
  kModShift = 1 << 2,
};

struct Key {
  std::uint32_t code{0};
  NamedKey named{NamedKey::kNone};
  std::uint8_t mods{kModNone};

  friend bool operator==(const Key& a, const Key& b) {
    return (a.code == b.code) && (a.named == b.named) && (a.mods == b.mods);
  }
};

bool ParseKey(std::string_view text, Key& out);

std::string KeyToString(const Key& key);

bool DecodeCsiKey(std::string_view body, Key& out);

bool ParseKeySequence(std::string_view text, std::vector<Key>& out);

struct KeyNode {
  std::vector<std::string> commands;
  std::vector<std::pair<Key, KeyNode>> children;

  bool IsLeaf() const { return !commands.empty(); }
};

class KeyMap {
 public:
  enum class Lookup {
    kNoMatch,
    kPending,
    kMatched,
  };

  ErrorCtx Bind(const std::vector<Key>& sequence, std::vector<std::string> commands,
                std::string* error_detail = nullptr);

  Lookup Find(const std::vector<Key>& sequence, const std::vector<std::string>** commands) const;

  bool Empty() const { return root_.children.empty(); }
  const KeyNode& Root() const { return root_; }
  void Clear() { root_ = KeyNode{}; }

 private:
  KeyNode root_;
};

struct KeyMaps {
  KeyMap normal;
  KeyMap insert;
};

KeyMaps DefaultKeyMaps();

ErrorCtx LoadKeyMapConfig(const std::filesystem::path& path, KeyMaps& maps, Settings& settings,
                          std::vector<std::string>& errors);

ErrorCtx ParseKeyMapConfig(std::string_view text, KeyMaps& maps, Settings& settings,
                           std::vector<std::string>& errors);

ErrorCtx ParseKeyMapConfig(std::string_view text, KeyMaps& maps, std::vector<std::string>& errors);
ErrorCtx LoadKeyMapConfig(const std::filesystem::path& path, KeyMaps& maps,
                          std::vector<std::string>& errors);

}

#endif
