// Every suite the test binary runs, declared in one place: main.cpp calls them
// from here, and each file below defines the ones named under its own heading.
#ifndef KOI_TESTS_TESTS_H_
#define KOI_TESTS_TESTS_H_

#include "test_support.h"

namespace koi {

// anchor.cpp -- the resolve ladder, the heal job and the live-shift shadow
void AnchorLineRules();
void AnchorPatienceDiff();
void AnchorEditDistance();
void AnchorResolveLadder();
void AnchorHealJobShapes();
void AnchorHealWriteBack();
void AnchorLiveShifting();
void AnchorJumpBackwardFollowsEdits();
void AnchorHealsThroughGit();
void AnchorHealNamesTheSymbol();
void AnchorHealCost();
void AnchorHealsFromTheEditor();
void AnchorHealAtFileEdges();
void AnchorHealStaleness();

// cli.cpp -- the non-interactive command line modes
void CliModes();

// commands.cpp -- the command registry and what each typable command does to a buffer
void CommandRegistry();
void EditingModel();
void GotoLineCommand();
void SetIndentAndLanguage();
void TypableCommandList();
void PromptCompletion();
void BufferAndConfigCommands();
void CommandExecution();
void FindCharAndScroll();
void CommandPrompt();
void TypableCommandsLeaveNormalModeUsable();
void BracketedPaste();
void InsertMotionCollapse();
void NewBuffer();
void IndentDetection();
void AutoIndentAndPairs();
void InsertModePasteLandsAtTheEndOfThePaste();
void MultiCursorCopyAndPasteCarryEverySelection();
void OpenLineMovesTheCaretsItAlreadyPlaced();
void NoOpEditsLeaveTheBufferClean();

// crash.cpp -- crash recovery and its dumps
void CrashRecovery();
void CrashRecoveryFollowsTheLiveBuffer();
void CrashRecoveryCoversEveryBuffer();

// editor.cpp -- editor state: targets, viewports, soft wrap, buffers, the status line
void TargetParsing();
void TargetPositioning();
void ViewportScrolling();
void SoftWrapLayout();
void BufferSwitching();
void ScrolloffIsDerivedNotCopied();
void ScrollingPastTheEndOfTheFile();
void OpeningAFileKeepsThePanesFittedSize();
void StatusSeverity();

// excerpt.cpp -- the excerpt views in navigate.cpp and their editor/command surface
void ReferenceExcerptView();
void EditableExcerptSave();
void ExcerptUndoRevert();
void ExcerptReadFailuresAreNamed();
void ExcerptCommandModes();
void ExcerptViewSources();
void ExcerptViewFuzz(Rng& rng);
void SearchExcerptView();
void ExcerptHeaderLineStaysTrue();
void ExcerptSaveRoundTripFuzz(Rng& rng);
void ReanchorRefusesToSwallowLinesItNeverShowed();
void ExcerptViewsNeverAdmitIllFormedFiles();
void ExcerptViewsFollowATrailingNewlineToggle();
void AutoPairedKeysGoThroughTheEditFunnel();

// files.cpp -- files on disk: atomic writes, reload-on-change, disk stamps
void ReloadEveryBufferSuite();
void SavingOntoAnOpenBufferIsRefused();
void AtomicWriteKeepsPermissions();
void DiskChangeOnFocus();
void DiskStampIsMtimeAndSize();
void TrimOnSave();
void AtomicWriteFollowsSymlinksAndKeepsHardLinks();
void ConcurrentSavesNeitherBlendNorShareATemp();

// fuzzy.cpp -- the smart-jump scorer: the DP, the bands, the blend
void CompactMatchesBeatScatteredOnes();
void CamelCaseCostsAndEarnsNothing();
void BandsSeparateNameMatchesFromPathMatches();
void TheBlendIsHandCheckable();
void SpansAlwaysSpellTheNeedle();
void ScoringAThousandRowsFitsTheKeystrokeBudget();

// indent.cpp -- the tree-sitter indent engine and the newline path it feeds
void TreeIndentEngine();
void TreeIndentFallbacks();
void TreeIndentNewlinePath();
void TreeIndentReindentOnType();
void TreeIndentLanguageMatrix();
void TreeIndentVendoredQueryAdditions();
void TreeIndentTabsUtf8AndCrlf();
void TreeIndentHeaderParentFold();
void TreeIndentInjectionGuardOutlivesEdits();
void TreeIndentSaysNothingAboutAParseItNeverGot();
void TreeIndentWarnsOncePerBrokenQuery();
void TreeIndentBudgetsBoundTheKeystroke();
void TreeIndentFuzz(Rng& rng);

// smartjump.cpp -- the parser, the corpus snapshot, the pipeline and the landing
void SmartJumpParsing();
void SmartJumpPipeline();
void SmartJumpBranchDiffPrior();
void SmartJumpFeedbackText();
void SmartJumpLanding();
void SmartJumpAdaptiveLoop();
void SmartJumpStepping();
void SmartJumpAutoFire();
void SmartJumpBounceRule();
void SmartJumpArrivalRules();
void SmartJumpSnapshotCost();

// jumplist.cpp -- the jump list
void JumpList();

// keylog.cpp -- key recording and replay
void KeyRecording(Rng& rng);

// keymap.cpp -- key parsing, the keymap trie, and the TOML config that fills them
void ConfigMerge();
void ShippedConfigsLoad();
void KeyParsing();
void KeyMapTrie();
void ConfigParsing();
void KeymapErrors();

// leap.cpp -- leap: matching, labelling, painting and the picks it grows into
void LeapJumpsToATypedPair();
void LeapLabelsEveryTargetItOffers();
void LeapPaintsItsMatchesAndLabels();
void LeapAlwaysLandsOnThePairItOffered(Rng& rng);
void LeapKeepsItsHintUnderABackgroundWarning();
void LeapEndsWhenTheWindowResizes();
void LeapWrapsWhereTheRendererWraps();
void LeapRefusesAClusterTheRightEdgeCutsInHalf();
void LeapRefusesANewlineAndKeepsTheTab();
void LeapIgnoresALabelKeyThatNamesNothing();
void LeapKeepsTheRowsAWideIndicatorWouldPushOffThePane();
void ALabelOnATabIsOneCellNotAWholeTabStop();
void LeapReadsOnlyTheColumnsThePaneAsksFor();
void LeapSaysWhyItEndedInItsOwnWords();
void LeapPicksGrowIntoCursors();
void LeapCapitalsSelectToTheMatch();
void LeapIdentifiesTheDocumentNotTheSlot();

// navigate.cpp -- the pickers -- files, buffers, symbols -- and how they are driven
void PickerCommands();
void SelfIsRunnableByName();
void PickerPipelines();
void FilePickerRanking();
void TheFilePickerLiftsFilesChangedOnThisBranch();
void FileFilter();
void BufferPickerRows();
void PickerCommandShape();
void FilePinsLandWhereYouLeft();
void LastEditIsFoundAcrossFiles();
void BoundaryRecording();

// overview.cpp -- the overview mode
void OverviewMarksTheSectionsItCouldNotFinish();

// piece_doc.cpp -- the piece table itself -- edits, undo/redo, the line index, mapped originals
void LineIndexStatic();
void LineIndexUnderEditing(Rng& rng);
void ReplaceChangesLength(Rng& rng);
void EditDescriptors(Rng& rng);
void RunTests(PieceTable& table, Rng& rng);
void LineIndexMemo(Rng& rng);
void MappedOriginal();
void PieceLookupBoundaries();

// piece_tree.cpp -- the B+tree under the piece table: offsets, arithmetic, rebalancing
void PieceTreeArithmetic();

// project.cpp -- the project store: state paths, the database, visits and symbols
void ProjectState();
void ProjectPaths();
void LocationWrites();
void ProjectStoreRobustness();
void AnUnusableProjectDatabaseIsRefusedInsteadOfSwallowingWrites();
void LegacyProjectStateIsSeededNotStolen();
void DeepProjectPathsStillGetAStateDirectory();
void ProjectRootFindsARepositoryRootedAtHome();
void ProjectStatePathsAreDerivedOncePerRoot();
void ProjectPathsAreKeyedTheSameFromEveryDirectory();
void StorePathsResolveAgainstTheRootFromBelowIt();
void VisitReadsAreBoundedAndTheTableIsPruned();
void SymbolReadsAreBoundedAndTheTableIsPruned();
void AV3StoreIsFoldedIntoOneDatabase();
void TheJumpCursorBecomesARowId();
void TheStoreKeepsOnlyPathsWorthKeeping();
void TheBranchIsReadOffTheHeadFile();
void FrecencyIsWeightTimesARecencyMultiplier();
void TheStoreAgesItselfWhenItGetsHeavy();
void TheVisitDebounceCountsFromTheLastCountedVisit();
void TheLostJumpImportIsPutBack();
void TwoOpensCannotMigrateTwice();
void TheBranchARowWasMadeOnIsABonus();
void TheBranchDiffIsReadOncePerBranch();
void AConfirmedQueryEarnsItsPlaceAndDecaysOut();

// query.cpp -- the file/query layer: whole-file reads, byte ranges, predicates and properties
void ReadWholeFileContract();
void ABufferPastFourGigabytesGetsNoByteRangeAtAll();
void QueryPropertiesAreReadOffThePatternThatSetThem();
void KindAndLinePredicatesAreAnsweredFromTheTree();
void CapturesRememberTheMatchTheyCameFrom();
void EveryVendoredIndentQueryCompilesAndItsPredicatesBite();

// render.cpp -- what reaches the terminal: the frame, its gaps, and its escapes
void Rendering();
void RenderingGaps();
void ControlBytesNeverReachTheTerminal();

// search.cpp -- search: regex matching, highlights, caches, and the scans behind them
void RegexSearch();
void SearchHighlightDismissal();
void SelectRegexIsInteractive();
void SearchCacheIsPerDocument();
void SearchLandsOnGraphemeBoundaries();
void ChunkedScanSearch();
void ScanResultsAreCapped();
void SearchSaysWhenItFailedInsteadOfFindingNothing();
void RunawaySearchesGiveUpInsteadOfFreezingTheEditor();

// selection.cpp -- selections, cursor movement, multi-cursor mapping
void SelectionMapping();
void SelectionMappingComposes(Rng& rng);
void SelectionNormalize();
void SelectionMovement();
void BlockCursorInvariant();
void MultiCursorEditing();
void MultiCursorFuzz(Rng& rng);
void MultiCursorCommentMappingIsUnchanged();
void ExtendingFromAnInsertCaretDoesNotAnnexTheGraphemeInFront();

// shell.cpp -- shell quoting, expansion and the commands that run one
void ShellIntegration();

// sqlite.cpp -- the sqlite wrapper's statement handling
void StmtSurvivesAFailedPrepare();

// symbols.cpp -- symbol extraction and the scans that feed the pickers
void SymbolQueriesCompile();
void ScanBudgetAndCancel();
void CancelStopsTheWalk();
void SymbolExtraction();
void SymbolRows();
void ScanErrorOutlivesTheNextScan();
void SymbolLookupSaysWhyThePickerIsEmpty();
void HotFirstOrdering();
void WordMatching();
void SymbolNamesAreCappedNotQuadratic();
void QueryMatchesAreBoundedLikeQueryTime();
void AQueryThatWillNotCompileFailsOncePerRunNotOncePerFile();
void AProjectScaleScanKeepsItsOrderAndItsFrames();

// syntax.cpp -- tree-sitter parsing, highlighting, predicates and injections
void SyntaxLanguages();
void SyntaxQueriesCompile();
void SyntaxHighlighting();
void SyntaxPredicates();
void MarkdownHighlightsItsInlineAndFencedLanguages();
void InjectedRegionsKeepTheirParsedTree();
void RunawayQueriesAreCutOffLikeRunawayParses();
void QueriesThatRunOutOfMatchSlotsAdmitIt();
void EveryInjectedRegionOnScreenIsPaintedNotJustTheFirstFewHundred();
void InjectedTextPastTheFrameBudgetIsRefusedAndSaidSo();
void FencesWithNoGrammarBehindThemDoNotSpendTheInjectionBudget();
void AutoIndentSeesStringsInsideInjectedRegions();
void ADocumentCannotNameTheSharedObjectKoiOpens();

// textobject.cpp -- text objects
void TextObjectSuite();
void TextObjectLookupsAreBoundedAndSayWhenTheyFallShort();

// theme.cpp -- theme loading, scope lookup and the builtin fallback
void Themes();
void ThemesFallBackToTheBuiltinForScopesTheyNeverNamed();
void ThemeAlphaIsCompositedAtLoad();

// thread_pool.cpp -- the shared scan workers and the jobs that run on them
void ScanWorkersFollowTheSetting();
void ScanWorkerSurvivesShutdownAndFailure();
void CancellingAJobDoesNotBlockTheEditor();
void ScansShareTheirWorkersAndSayWhenTheyAreWaiting();

// undo_history.cpp -- undo, redo, cursor notes and the history budget, driven from the editor
void JournalIsIndexableByRevision();
void JournalIsBounded();
void UndoRestoresTheCursorOfTheStepItUndoes();
void RedoRestoresTheCursorsItRecorded();
void ACursorNoteIsSpentByTheEditItWasTakenFor();
void UndoDoesNotRestoreASelectionFromAnEarlierCommand();
void ABatchThatEditsNothingLeavesNoCursorNote();
void AGroupedEditIsNeverFoldedIntoTheHistoryBase();
void AMotionBatchAnEditATrimAndAnEditAllWalkBack();
void HistoryBudgetIsPerFile();
void UndoingPastATrimmedHistoryBaseKeepsTheBufferDirty();
void CursorNotesLandOnATrimmedHistoryBase();
void AbandonedUndoBranchesAreReclaimed();

// unicode.cpp -- grapheme clusters, display widths, column mapping
void UnicodeBoundaries();
void UnicodeEditsAreRejectedInsideClusters();
void UnicodeClusterSpanningPieces();
void UnicodeEditingRoundTrip(Rng& rng);
void DisplayWidths();
void ColumnMapping();
void CrlfLines();
void GraphemeQueriesMatchAFullWalk();
void ClustersLongerThanTheReadWindowStillSegment();

// windows.cpp -- the window/pane layer in editor.cpp, seen through render.cpp
void AdversarialWindows(Rng& rng);
void SplitsAndHighlighting();
void LiveViewsAndPanesFuzz(Rng& rng);
void AdversarialWindowContents(Rng& rng);
void RenderLeavesWindowsAlone(Rng& rng);
void ClickingChoosesTheWindowUnderIt();
void Windows();
void ResizablePanes(Rng& rng);
void SwappingPanesKeepsTheirCursors();

}  // namespace koi

#endif  // KOI_TESTS_TESTS_H_
