// The driver. Every suite the binary runs is named in tests.h and called from
// here.
//
// The order below is neither alphabetical nor arbitrary: some suites stop and
// restart state the others share, and those run where the comments say.

#include "tests.h"

namespace koi {
namespace {

std::string SyntheticDocument(size_t bytes) {
  static constexpr std::string_view kWords[] = {"koi",  "piece", "vector", "undo",   "redo",
                                                "edit", "table", "cursor", "buffer", "index"};
  std::mt19937_64 rng(0xC0FFEEu);
  std::string out;
  out.reserve(bytes + 16);
  int col = 0;
  while (out.size() < bytes) {
    out.append(kWords[rng() % std::size(kWords)]);
    if (++col == 9) {
      out.push_back('\n');
      col = 0;
    } else {
      out.push_back(' ');
    }
  }
  return out;
}

void RunUnicodeTests(Rng& rng) {
  LineIndexMemo(rng);
  MappedOriginal();
  StatusSeverity();
  KeyParsing();
  CliModes();
  NewBuffer();
  CrashRecovery();
  BracketedPaste();
  DiskChangeOnFocus();
  DiskStampIsMtimeAndSize();
  InsertMotionCollapse();
  TrimOnSave();
  KeyMapTrie();
  ConfigParsing();
  CommandRegistry();
  EditingModel();
  GotoLineCommand();
  TypableCommandList();
  SetIndentAndLanguage();
  CommandExecution();
  FindCharAndScroll();
  MultiCursorCommentMappingIsUnchanged();
  ShellIntegration();
  CommandPrompt();
  TargetParsing();
  TargetPositioning();
  ViewportScrolling();
  SoftWrapLayout();
  RegexSearch();
  SearchLandsOnGraphemeBoundaries();
  JumpList();
  AnchorLineRules();
  AnchorPatienceDiff();
  AnchorEditDistance();
  AnchorResolveLadder();
  AnchorHealJobShapes();
  AnchorHealWriteBack();
  AnchorLiveShifting();
  AnchorJumpBackwardFollowsEdits();
  AnchorHealsThroughGit();
  AnchorHealNamesTheSymbol();
  AnchorHealCost();
  AnchorHealsFromTheEditor();
  AnchorHealAtFileEdges();
  AnchorHealStaleness();
  SearchHighlightDismissal();
  SelectRegexIsInteractive();
  SyntaxLanguages();
  SyntaxQueriesCompile();
  SyntaxHighlighting();
  SyntaxPredicates();
  ReadWholeFileContract();
  ScanBudgetAndCancel();
  CancelStopsTheWalk();
  ScanErrorOutlivesTheNextScan();
  SymbolLookupSaysWhyThePickerIsEmpty();
  SymbolQueriesCompile();
  TextObjectSuite();
  TreeIndentEngine();
  TreeIndentFallbacks();
  TreeIndentNewlinePath();
  TreeIndentReindentOnType();
  TreeIndentLanguageMatrix();
  TreeIndentVendoredQueryAdditions();
  TreeIndentTabsUtf8AndCrlf();
  TreeIndentHeaderParentFold();
  TreeIndentInjectionGuardOutlivesEdits();
  TreeIndentSaysNothingAboutAParseItNeverGot();
  TreeIndentWarnsOncePerBrokenQuery();
  TreeIndentBudgetsBoundTheKeystroke();
  TreeIndentFuzz(rng);
  SymbolExtraction();
  SymbolRows();
  WordMatching();
  HotFirstOrdering();
  PickerCommands();
  PickerPipelines();
  FilePickerRanking();
  FileFilter();
  SelfIsRunnableByName();
  ConfigMerge();
  ScanWorkersFollowTheSetting();
  ShippedConfigsLoad();
  ProjectState();
  ProjectPaths();
  LocationWrites();
  Themes();
  EditDescriptors(rng);
  DisplayWidths();
  ColumnMapping();
  CrlfLines();
  GraphemeQueriesMatchAFullWalk();
  SelectionMapping();
  SelectionMappingComposes(rng);
  SelectionNormalize();
  SelectionMovement();
  BlockCursorInvariant();
  MultiCursorEditing();
  MultiCursorFuzz(rng);
  LineIndexStatic();
  LineIndexUnderEditing(rng);
  ReplaceChangesLength(rng);
  UnicodeBoundaries();
  UnicodeEditsAreRejectedInsideClusters();
  UnicodeClusterSpanningPieces();
  UnicodeEditingRoundTrip(rng);
}

}  // namespace
}  // namespace koi

namespace {

// Every state path the editor derives -- the project database, the jump list,
// the picker's last selection, the crash journal -- hangs off $HOME. Run on the
// real one, the suite both reads state a live editor wrote (so a test can pass
// or fail depending on what the developer did in koi last) and writes into it:
// ~/.local/share/koi/ on this machine still holds directories named after the
// scratch fixtures of past runs. Give the run a HOME of its own, keyed on the
// pid so two runs cannot land on one, before anything can read it.
struct PrivateHome {
  std::filesystem::path dir;

  PrivateHome() {
    std::error_code ec;
    dir = std::filesystem::temp_directory_path(ec) /
          ("koi-tests-home-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      std::cerr << "could not make a private HOME: " << ec.message() << "\n";
      std::exit(1);
    }
    ::setenv("HOME", dir.c_str(), 1);
  }
  ~PrivateHome() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  PrivateHome(const PrivateHome&) = delete;
  PrivateHome& operator=(const PrivateHome&) = delete;
};

}  // namespace

int main() {
  const PrivateHome private_home;

  unsigned long long seed = 0x5EED1234ULL;
  if (const char* env = std::getenv("KOI_TEST_SEED")) seed = std::strtoull(env, nullptr, 0);
  koi::Rng rng{seed};
  std::cout << "seed: " << seed << " (set KOI_TEST_SEED to change)\n";

  koi::PieceTable medium = koi::MakeTable(koi::SyntheticDocument(50 * 1024));
  koi::RunTests(medium, rng);

  koi::PieceTable large = koi::MakeTable(koi::SyntheticDocument(200 * 1024));
  koi::RunTests(large, rng);

  koi::RunUnicodeTests(rng);
  koi::PieceLookupBoundaries();
  koi::PieceTreeArithmetic();
  koi::IndentDetection();
  koi::AutoIndentAndPairs();
  koi::SearchCacheIsPerDocument();
  koi::BufferSwitching();
  koi::Windows();
  koi::ResizablePanes(rng);
  koi::Rendering();
  koi::RenderingGaps();
  koi::BufferPickerRows();
  koi::PickerCommandShape();
  koi::FilePinsLandWhereYouLeft();
  koi::LastEditIsFoundAcrossFiles();
  koi::BoundaryRecording();
  koi::KeymapErrors();
  koi::ProjectStoreRobustness();
  koi::ReloadEveryBufferSuite();
  koi::KeyRecording(rng);
  koi::PromptCompletion();
  koi::AdversarialWindows(rng);
  koi::CrashRecoveryFollowsTheLiveBuffer();
  koi::CrashRecoveryCoversEveryBuffer();
  koi::ScrolloffIsDerivedNotCopied();
  koi::ScrollingPastTheEndOfTheFile();
  koi::OpeningAFileKeepsThePanesFittedSize();
  koi::ReferenceExcerptView();
  koi::EditableExcerptSave();
  koi::ExcerptUndoRevert();
  koi::ExcerptReadFailuresAreNamed();
  koi::ExcerptCommandModes();
  koi::ExcerptViewSources();
  koi::ExcerptViewFuzz(rng);
  koi::SearchExcerptView();
  koi::ChunkedScanSearch();
  koi::SavingOntoAnOpenBufferIsRefused();
  koi::JournalIsIndexableByRevision();
  koi::JournalIsBounded();
  koi::SwappingPanesKeepsTheirCursors();
  koi::AtomicWriteKeepsPermissions();
  koi::UndoRestoresTheCursorOfTheStepItUndoes();
  koi::RedoRestoresTheCursorsItRecorded();
  koi::ACursorNoteIsSpentByTheEditItWasTakenFor();
  koi::UndoDoesNotRestoreASelectionFromAnEarlierCommand();
  koi::TypableCommandsLeaveNormalModeUsable();
  koi::HistoryBudgetIsPerFile();
  koi::SplitsAndHighlighting();
  koi::ClickingChoosesTheWindowUnderIt();
  koi::AdversarialWindowContents(rng);
  koi::RenderLeavesWindowsAlone(rng);
  koi::BufferAndConfigCommands();
  koi::LiveViewsAndPanesFuzz(rng);

  // Round 5. Last, because ScanWorkerSurvivesShutdownAndFailure stops and
  // restarts the shared scan pool.
  koi::InsertModePasteLandsAtTheEndOfThePaste();
  koi::ControlBytesNeverReachTheTerminal();
  koi::AtomicWriteFollowsSymlinksAndKeepsHardLinks();
  koi::ExcerptHeaderLineStaysTrue();
  koi::ScanResultsAreCapped();
  koi::MultiCursorCopyAndPasteCarryEverySelection();
  koi::MarkdownHighlightsItsInlineAndFencedLanguages();
  koi::CancellingAJobDoesNotBlockTheEditor();
  koi::StmtSurvivesAFailedPrepare();
  koi::ExcerptSaveRoundTripFuzz(rng);

  // Round 7.
  koi::OpenLineMovesTheCaretsItAlreadyPlaced();
  koi::ReanchorRefusesToSwallowLinesItNeverShowed();
  koi::UndoingPastATrimmedHistoryBaseKeepsTheBufferDirty();
  koi::CursorNotesLandOnATrimmedHistoryBase();
  koi::AbandonedUndoBranchesAreReclaimed();
  koi::ExcerptViewsNeverAdmitIllFormedFiles();
  koi::ExcerptViewsFollowATrailingNewlineToggle();
  koi::NoOpEditsLeaveTheBufferClean();
  koi::InjectedRegionsKeepTheirParsedTree();
  koi::RunawayQueriesAreCutOffLikeRunawayParses();
  koi::QueriesThatRunOutOfMatchSlotsAdmitIt();
  koi::EveryInjectedRegionOnScreenIsPaintedNotJustTheFirstFewHundred();
  koi::InjectedTextPastTheFrameBudgetIsRefusedAndSaidSo();
  koi::FencesWithNoGrammarBehindThemDoNotSpendTheInjectionBudget();
  koi::AutoIndentSeesStringsInsideInjectedRegions();
  koi::ADocumentCannotNameTheSharedObjectKoiOpens();
  koi::AutoPairedKeysGoThroughTheEditFunnel();
  koi::ClustersLongerThanTheReadWindowStillSegment();
  koi::SearchSaysWhenItFailedInsteadOfFindingNothing();
  koi::RunawaySearchesGiveUpInsteadOfFreezingTheEditor();
  koi::ScansShareTheirWorkersAndSayWhenTheyAreWaiting();
  koi::AnUnusableProjectDatabaseIsRefusedInsteadOfSwallowingWrites();
  koi::LegacyProjectStateIsSeededNotStolen();
  koi::DeepProjectPathsStillGetAStateDirectory();
  koi::ProjectRootFindsARepositoryRootedAtHome();
  koi::ProjectStatePathsAreDerivedOncePerRoot();
  koi::SymbolNamesAreCappedNotQuadratic();
  koi::VisitReadsAreBoundedAndTheTableIsPruned();
  koi::SymbolReadsAreBoundedAndTheTableIsPruned();
  koi::ProjectPathsAreKeyedTheSameFromEveryDirectory();
  koi::StorePathsResolveAgainstTheRootFromBelowIt();
  koi::AV3StoreIsFoldedIntoOneDatabase();
  koi::TheJumpCursorBecomesARowId();
  koi::TheStoreKeepsOnlyPathsWorthKeeping();
  koi::TheBranchIsReadOffTheHeadFile();
  koi::FrecencyIsWeightTimesARecencyMultiplier();
  koi::TheStoreAgesItselfWhenItGetsHeavy();
  koi::TheVisitDebounceCountsFromTheLastCountedVisit();
  koi::TheLostJumpImportIsPutBack();
  koi::TwoOpensCannotMigrateTwice();
  koi::TheBranchARowWasMadeOnIsABonus();
  koi::TheBranchDiffIsReadOncePerBranch();
  koi::TheFilePickerLiftsFilesChangedOnThisBranch();

  // Step 5: the scorer, the blend and the queries table under them.
  koi::CompactMatchesBeatScatteredOnes();
  koi::CamelCaseCostsAndEarnsNothing();
  koi::BandsSeparateNameMatchesFromPathMatches();
  koi::TheBlendIsHandCheckable();
  koi::SpansAlwaysSpellTheNeedle();
  koi::ScoringAThousandRowsFitsTheKeystrokeBudget();
  koi::AConfirmedQueryEarnsItsPlaceAndDecaysOut();

  // Step 6: the parser, the prompt, the landing and the bounce.
  koi::SmartJumpParsing();
  koi::SmartJumpPipeline();
  koi::SmartJumpBranchDiffPrior();
  koi::SmartJumpFeedbackText();
  koi::SmartJumpLanding();
  koi::SmartJumpAdaptiveLoop();
  koi::SmartJumpStepping();
  koi::SmartJumpAutoFire();
  koi::SmartJumpBounceRule();
  koi::SmartJumpArrivalRules();
  koi::SmartJumpSnapshotCost();

  // Round 8.
  koi::QueryMatchesAreBoundedLikeQueryTime();
  koi::AQueryThatWillNotCompileFailsOncePerRunNotOncePerFile();
  koi::AProjectScaleScanKeepsItsOrderAndItsFrames();
  koi::ABufferPastFourGigabytesGetsNoByteRangeAtAll();
  koi::QueryPropertiesAreReadOffThePatternThatSetThem();
  koi::KindAndLinePredicatesAreAnsweredFromTheTree();
  koi::CapturesRememberTheMatchTheyCameFrom();
  koi::EveryVendoredIndentQueryCompilesAndItsPredicatesBite();
  koi::TextObjectLookupsAreBoundedAndSayWhenTheyFallShort();
  koi::OverviewMarksTheSectionsItCouldNotFinish();

  koi::ConcurrentSavesNeitherBlendNorShareATemp();

  koi::ExtendingFromAnInsertCaretDoesNotAnnexTheGraphemeInFront();

  koi::ABatchThatEditsNothingLeavesNoCursorNote();
  koi::AGroupedEditIsNeverFoldedIntoTheHistoryBase();
  koi::AMotionBatchAnEditATrimAndAnEditAllWalkBack();

  // Leap.
  koi::LeapJumpsToATypedPair();
  koi::LeapLabelsEveryTargetItOffers();
  koi::LeapPaintsItsMatchesAndLabels();
  koi::LeapAlwaysLandsOnThePairItOffered(rng);
  koi::LeapKeepsItsHintUnderABackgroundWarning();
  koi::LeapEndsWhenTheWindowResizes();
  koi::LeapWrapsWhereTheRendererWraps();
  koi::LeapRefusesAClusterTheRightEdgeCutsInHalf();
  koi::LeapRefusesANewlineAndKeepsTheTab();
  koi::LeapIgnoresALabelKeyThatNamesNothing();
  koi::LeapKeepsTheRowsAWideIndicatorWouldPushOffThePane();
  koi::ALabelOnATabIsOneCellNotAWholeTabStop();
  koi::LeapReadsOnlyTheColumnsThePaneAsksFor();
  koi::LeapSaysWhyItEndedInItsOwnWords();
  koi::LeapPicksGrowIntoCursors();
  koi::LeapCapitalsSelectToTheMatch();
  koi::LeapIdentifiesTheDocumentNotTheSlot();
  koi::ThemesFallBackToTheBuiltinForScopesTheyNeverNamed();
  koi::ThemeAlphaIsCompositedAtLoad();

  koi::ScanWorkerSurvivesShutdownAndFailure();

  return common::TestSummary();
}
