#ifndef KOI_KEYLOG_H_
#define KOI_KEYLOG_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "editor.h"
#include "keymap.h"

namespace koi {

enum class KeyOutcome : std::uint8_t {
  kPending,
  kBinding,
  kInsertText,
  kUnbound,
  kCount,
  kPrompt,
  kPendingChar,
  kChordTimeout,
  kOther,
};

std::string_view NameOfOutcome(KeyOutcome outcome);

class KeyRecorder {
 public:

  // Same contract as ProjectStore::Open: null plus `error` is a failure, a
  // recorder plus a non-empty `error` is the warning that the old database was
  // corrupt and this one started empty in its place.
  static std::shared_ptr<KeyRecorder> Open(const std::filesystem::path& db, std::string pane,
                                           std::string& error);

  virtual ~KeyRecorder() = default;

  virtual void SetKeyMap(std::string fingerprint) = 0;

  virtual void Begin(const Editor& ed, const Key& key, const std::vector<Key>& prefix) = 0;

  virtual void Resolve(KeyOutcome outcome, const std::vector<std::string>* commands) = 0;

  virtual void Abandon() = 0;

  virtual void NoteChordTimeout(const Editor& ed, const std::vector<Key>& chord) = 0;

  virtual bool Buffered() const = 0;

  virtual void Flush() = 0;
};

std::string KeyMapFingerprint(const KeyMaps& maps);

bool ContentsAreSensitive(std::string_view path);

}

#endif
