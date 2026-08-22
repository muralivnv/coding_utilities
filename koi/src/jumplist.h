#ifndef KOI_JUMPLIST_H_
#define KOI_JUMPLIST_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "piece_doc.h"
#include "project.h"

namespace koi {

struct Jump {
  // The `locations` row this came from. A buffer open on the file knows where
  // its rows have drifted to since the store last saw them, and the id is what
  // asks it -- see AnchorShadowLine.
  std::int64_t id{0};
  std::string path;
  Index line{1};
  Index col{1};
};

struct JumpStore {
  // A view over the project store's `locations`, not a database of its own:
  // since v4 a jump is a row beside the file and symbol history, and the store
  // hands over the one connection they share. Null plus `error` is a failure --
  // and the only one left is having no project store to read.
  static std::shared_ptr<JumpStore> Open(std::shared_ptr<ProjectStore> project, std::string pane,
                                         std::string& error);

  virtual ~JumpStore() = default;

  // A place with nothing but its position: a step back into an excerpt view,
  // or a caller with no buffer in hand. What such a record does not say, the
  // row it merges onto keeps.
  void Record(const std::filesystem::path& path, Index line, Index col);

  // The full record. The jump list writes `locations` through the project
  // store's one writer, so a jump and a boundary record merge onto each other
  // rather than racing to describe the same place two ways; what the jump list
  // owns is the transaction around it and the pane's cursor.
  virtual void Record(const LocationRecord& row) = 0;

  virtual bool Step(bool forward, Jump& out) = 0;

  // Whether a step back should record the place it is leaving: true unless the
  // pane is already part-way back through the list, in which case the place it
  // is leaving is one the list already holds.
  virtual bool AtNewest() = 0;
};

std::string PaneId();

}

#endif
