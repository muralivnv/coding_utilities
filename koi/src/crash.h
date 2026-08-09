#pragma once

#include <span>
#include <string>

#include "piece_doc.h"

namespace koi {

std::string RecoveryPathFor(const std::string& path);

struct CrashDoc {
  const PieceTable* table{nullptr};
  const bool* modified{nullptr};
  std::string path;
};

void SetCrashDocuments(std::span<const CrashDoc> docs);

void SetCrashDocument(const PieceTable* table, const bool* modified, const std::string& path);

void InstallCrashHandlers(void (*restore_terminal)());

bool HangupRequested();

bool WriteRecoveryFile();

}
