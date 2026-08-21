#ifndef KOI_NAVIGATE_H_
#define KOI_NAVIGATE_H_

#include <string>
#include <string_view>
#include <vector>

#include "editor.h"
#include "symbols.h"

namespace koi {

void FilePicker(Editor& ed, std::string_view query = "");

void BufferPicker(Editor& ed, std::string_view query = "");

void ContentPicker(Editor& ed, std::string_view query = "");

void SymbolPicker(Editor& ed, std::string_view query = "");

void BufferSymbolPicker(Editor& ed, std::string_view query = "");

void GotoDefinition(Editor& ed);

void ShowReferences(Editor& ed);

void ShowReferenceExcerpts(Editor& ed);
void GotoExcerptSource(Editor& ed);

void OpenReferenceExcerpts(Editor& ed, const std::vector<Symbol>& found, std::string_view word);

void SearchExcerpts(Editor& ed, std::string_view query);

bool SaveExcerptView(Editor& ed);

void ShowDefinitionExcerpts(Editor& ed);

void CommandExcerpts(Editor& ed, std::string_view command, bool watched = false,
                     bool with_msg = false);

void PinExcerpts(Editor& ed);
void MessagesView(Editor& ed);
void WatchView(Editor& ed);
void UnwatchView(Editor& ed);

bool StartCommandJob(Editor& ed, std::string_view command, bool watched, bool with_msg,
                     PendingCommand::Then then);

bool PumpCommandJobs(Editor& ed, bool defer_present = false);

bool CancelCommandJob(Editor& ed);

void KillAllCommandJobs(Editor& ed);

// Grows the scan pool to `workers` live threads; never shrinks it (shrinking
// joins, and a join blocks on whatever a worker is scanning).
void StartScanWorker(Index workers);
void StopScanWorker();

void MaybeRefreshExcerptView(Editor& ed);

int RerunWatchedViews(Editor& ed);

void MarkLiveViewsStale(Editor& ed, ExcerptView::Kind kind);

void RefreshLiveExcerptViews(Editor& ed);

void DropExcerptHunk(Editor& ed);

bool RebuildExcerptView(Editor& ed);

void AlignExcerptModel(Document& doc);

void DropUnreachableEpochs(Document& doc);

void LastPicker(Editor& ed);

void ToggleSidebar(Editor& ed);

void SetPinHere(Editor& ed, int slot);
void ClearPinSlot(Editor& ed, int slot);
void JumpToPin(Editor& ed, int slot);


void JumpToHotSymbol(Editor& ed, int index);

void RecordVisitHere(Editor& ed);
void RecordEditHere(Editor& ed);

void RestoreLastPosition(Editor& ed);

std::string ExpandPickerCommand(const Editor& ed, std::string_view command,
                                std::string_view query);

std::string PickerCommand(const Editor& ed, std::string_view name);

// Every picker there is. LastPicker has to know each of these names, or a
// picker records itself as the last one and then cannot be reopened.
std::vector<std::string_view> PickerNames();

// Which picker ran last, and with what query. Exposed as a pair so a test can
// check the reader inverts the writer.
void WriteLastPicker(std::string_view name, std::string_view query);
bool ReadLastPicker(std::string& name, std::string& query);

std::string FileFilterCommand(const Editor& ed);

std::string RankedFileRows(Editor& ed);

std::string BufferPickerItems(const Editor& ed);
void ChooseBufferRow(Editor& ed, std::string_view payload);

}

#endif
