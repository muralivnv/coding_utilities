#ifndef KOI_JUMPLIST_H_
#define KOI_JUMPLIST_H_

#include <filesystem>
#include <memory>
#include <string>

#include "piece_doc.h"

namespace koi {

struct Jump {
  std::string path;
  Index line{1};
  Index col{1};
};

struct JumpStore {
  // Same contract as ProjectStore::Open: null plus `error` is a failure, a
  // store plus a non-empty `error` is the warning that the old database was
  // corrupt and this one started empty in its place.
  static std::shared_ptr<JumpStore> Open(const std::filesystem::path& db, std::string pane,
                                         std::string& error);

  virtual ~JumpStore() = default;

  virtual void Record(const std::filesystem::path& path, Index line, Index col) = 0;

  virtual bool Step(bool forward, Jump& out) = 0;

  virtual bool AtNewest() = 0;

  virtual int Count() = 0;
};

std::filesystem::path JumpDbPath();

std::string PaneId();

}

#endif
