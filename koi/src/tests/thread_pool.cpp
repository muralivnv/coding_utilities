// Tests for thread_pool.h: the shared scan workers, the jobs that run on them,
// and what a cancel or a shutdown does to a job in flight.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

// What the cases below run their pool at. Four is the number the setting used
// to default to, and the cases that fill the pool and queue behind it need more
// than one worker to have anything to say -- so they name it here rather than
// reading `Settings::scan_workers`, which is a knob and has already moved once.
inline constexpr Index kScanWorkers = 4;

void ScanWorkersFollowTheSetting() {
  TEST_CASE("config: scan-workers is clamped on parse and only ever grows the pool");
  {
    // The parse clamps rather than complains, like tab-width: a number is
    // what was asked for, just not that one.
    KeyMaps maps = DefaultKeyMaps();
    Settings settings;
    std::vector<std::string> errors;
    std::ignore = ParseKeyMapConfig("[editor]\nscan-workers = 100\n", maps, settings, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(settings.scan_workers, 32);
    std::ignore = ParseKeyMapConfig("[editor]\nscan-workers = 0\n", maps, settings, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(settings.scan_workers, 1);
  }
  {
    // The pool is a process-wide singleton, so this starts from zero and puts
    // it back: the scan tests after this one size it themselves.
    StopScanWorker();
    EXPECT_EQ(ThreadPool::Instance().WorkerCount(), std::size_t{0});
    StartScanWorker(2);
    EXPECT_EQ(ThreadPool::Instance().WorkerCount(), std::size_t{2});
    // A raised setting applies live...
    StartScanWorker(6);
    EXPECT_EQ(ThreadPool::Instance().WorkerCount(), std::size_t{6});
    // ...a lowered one does not: shrinking joins workers, and a join blocks on
    // whatever they are scanning.
    StartScanWorker(3);
    EXPECT_EQ(ThreadPool::Instance().WorkerCount(), std::size_t{6});
    StopScanWorker();
    EXPECT_EQ(ThreadPool::Instance().WorkerCount(), std::size_t{0});
    // A count below the floor still yields a pool that can run something.
    StartScanWorker(0);
    EXPECT_EQ(ThreadPool::Instance().WorkerCount(), std::size_t{1});
    StopScanWorker();
  }
}

void ScanWorkerSurvivesShutdownAndFailure() {
  TEST_CASE("scan jobs: the pool can be restarted and a job always publishes");

  // ScanJob::done is the only thing PumpCommandJobs polls. The pool wraps every
  // task in a packaged_task, which swallows a throw into a future nobody reads,
  // so a task that failed to publish left the editor polling forever.
  {
    std::atomic<bool> reached{false};
    std::ignore = ThreadPool::Instance().AddTask([&reached] {
      reached.store(true);
      throw std::runtime_error("boom");
    });
    for (int i = 0; (i < 2000) && !reached.load(); ++i) usleep(500);
    EXPECT_TRUE(reached.load());
    // The pool has to survive it, or one bad task takes every later one with it.
    std::atomic<int> after{0};
    std::ignore = ThreadPool::Instance().AddTask([&after] { after.fetch_add(1); });
    for (int i = 0; (i < 2000) && (after.load() == 0); ++i) usleep(500);
    EXPECT_EQ(after.load(), 1);
  }

  // Shutdown used to latch its flag and drop whatever was queued, so the pool
  // was dead for good and a task already accepted simply never ran.
  {
    std::atomic<int> ran{0};
    std::ignore = ThreadPool::Instance().AddTask([&ran] {
      usleep(200 * 1000);
      ran.fetch_add(1);
    });
    std::ignore = ThreadPool::Instance().AddTask([&ran] { ran.fetch_add(100); });
    usleep(20 * 1000);
    ThreadPool::Instance().Shutdown();
    EXPECT_EQ(ran.load(), 101);
    EXPECT_EQ(static_cast<Index>(ThreadPool::Instance().WorkerCount()), Index{0});

    std::atomic<int> restarted{0};
    ThreadPool::Instance().Init(1);
    std::ignore = ThreadPool::Instance().AddTask([&restarted] { restarted.fetch_add(1); });
    for (int i = 0; (i < 2000) && (restarted.load() == 0); ++i) usleep(500);
    EXPECT_EQ(restarted.load(), 1);
    ThreadPool::Instance().Shutdown();
  }

  // End to end: a search started after the worker was stopped still finishes,
  // because EnsureScanWorker asks the pool rather than remembering that it once
  // called Init. Before, this hung until the caller's own budget ran out.
  {
    const Scratch scratch{"koi-scan-restart"};
    scratch.Write("one.txt", "widget here\n");
    StopScanWorker();
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    ed.settings.file_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";
    SearchExcerpts(ed, "widget");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_EQ(ed.doc.excerpts.refs.size(), std::size_t{1});
    EXPECT_TRUE(ed.pending_commands.empty());
  }
}

void CancellingAJobDoesNotBlockTheEditor() {
  TEST_CASE(":from-cancel returns promptly and leaves no zombie behind");
  const Scratch scratch{"koi-from-cancel"};
  scratch.Write("one.txt", "widget\n");

  const auto elapsed_ms = [](auto from) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - from)
        .count();
  };

  Editor ed;
  ed.theme = BuiltinTheme();
  ed.settings.excerpt_context = 0;
  ed.settings.file_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";

  // A child that traps SIGTERM and then sleeps. EndCommandJob used to follow
  // its SIGKILL with a blocking waitpid on the editor thread, which is fine
  // here but is not for a child wedged in uninterruptible sleep -- and that
  // path has no way to tell the two apart.
  RunTypableCommand(ed, "from sh -c 'trap \"\" TERM; sleep 30'");
  EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
  const int pid = ed.pending_commands.front().pid;
  EXPECT_TRUE(pid > 0);

  const auto started = std::chrono::steady_clock::now();
  EXPECT_TRUE(CancelCommandJob(ed));
  const auto took = elapsed_ms(started);
  EXPECT_TRUE(ed.pending_commands.empty());
  // Generous: the point is that it does not wait on the child, not that it is
  // fast. A blocking wait on a child that ignores SIGTERM would sit here.
  EXPECT_TRUE(took < 2000);

  // The child is gone, and pumping reaps whatever EndCommandJob could not.
  for (int i = 0; (i < 200) && (::kill(pid, 0) == 0); ++i) {
    std::ignore = PumpCommandJobs(ed);
    usleep(10 * 1000);
  }
  EXPECT_FALSE(::kill(pid, 0) == 0);

  // Quitting with a job still running takes the same path and must not hang.
  {
    Editor quitting;
    quitting.theme = BuiltinTheme();
    quitting.settings.excerpt_context = 0;
    quitting.settings.file_filter = ed.settings.file_filter;
    RunTypableCommand(quitting, "from sh -c 'trap \"\" TERM; sleep 30'");
    EXPECT_EQ(quitting.pending_commands.size(), std::size_t{1});
    const auto at_quit = std::chrono::steady_clock::now();
    KillAllCommandJobs(quitting);
    EXPECT_TRUE(elapsed_ms(at_quit) < 2000);
    EXPECT_TRUE(quitting.pending_commands.empty());
  }
}

void ScansShareTheirWorkersAndSayWhenTheyAreWaiting() {
  const Scratch scratch{"koi-scan-queue"};
  scratch.Write("one.txt", "widget here\ngadget here\nsprocket here\n");
  const std::string find = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";

  const auto settle = [](const std::shared_ptr<ScanJob>& slot) {
    for (int i = 0; (i < 40000) && !slot->done.load(std::memory_order_acquire); ++i) usleep(500);
    return slot->done.load(std::memory_order_acquire);
  };
  const auto right_of = [](const Editor& ed) {
    std::string out;
    for (const StatusSpan& span : StatusBar(ed).right) out += span.text;
    return out;
  };

  TEST_CASE("scan jobs: a slow scan does not hold back the ones queued after it");
  {
    // The pool used to have exactly one worker, so refining a query put the new
    // scan behind the old one's entire life -- and every scan begins with a
    // file-filter subprocess that no stop flag can interrupt, so "the old one's
    // life" is however long that `find` takes. Measured, a two-file search took
    // four seconds: the predecessor's, not its own.
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    // Asked for, not inherited. StartScanJob sizes the pool from this setting,
    // so a default of 1 leaves one worker and the second scan waits for the
    // first by design rather than by the bug this case is about -- which is a
    // failure that reads exactly like the regression it is meant to catch.
    ed.settings.scan_workers = kScanWorkers;

    ed.settings.file_filter = "sleep 2; " + find;
    SearchExcerpts(ed, "widget");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{1});
    const std::shared_ptr<ScanJob> slow = ed.pending_commands.back().scan;

    ed.settings.file_filter = find;
    SearchExcerpts(ed, "gadget");
    EXPECT_EQ(ed.pending_commands.size(), std::size_t{2});
    const std::shared_ptr<ScanJob> quick = ed.pending_commands.back().scan;
    EXPECT_TRUE((slow != nullptr) && (quick != nullptr));

    // Half a second to find one match in one file, against a predecessor that
    // is two seconds of `sleep`. Deliberately far from both numbers: the claim
    // is that the second scan does not wait for the first, not that either is
    // fast.
    for (int i = 0; (i < 1000) && !quick->done.load(std::memory_order_acquire); ++i) usleep(500);
    EXPECT_TRUE(quick->done.load(std::memory_order_acquire));
    EXPECT_FALSE(slow->done.load(std::memory_order_acquire));
    EXPECT_EQ(quick->hits.size(), std::size_t{1});

    EXPECT_TRUE(settle(slow));
    KillAllCommandJobs(ed);
  }

  TEST_CASE("scan jobs: one cancelled while it waits never runs, and says so");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;

    // Every worker busy, so the next scan has to wait for one. The size is
    // asked for rather than inherited from the default -- a pool of one has no
    // "queued behind a busy worker" to test -- and then counted back off the
    // pool rather than written down here, since it only ever grows and an
    // earlier case may have left it larger.
    ed.settings.scan_workers = kScanWorkers;
    StartScanWorker(ed.settings.scan_workers);
    const std::size_t workers = ThreadPool::Instance().WorkerCount();
    EXPECT_TRUE(workers > 1);
    ed.settings.file_filter = "sleep 2; " + find;
    std::vector<std::shared_ptr<ScanJob>> blocking;
    for (std::size_t n = 0; n < workers; ++n) {
      SearchExcerpts(ed, "blocker" + std::to_string(n));
      blocking.push_back(ed.pending_commands.back().scan);
    }
    // Waited for, not assumed: a worker takes a moment to pick a task up, and
    // the point of the case below is a job that is queued because the workers
    // are busy, not one queued because they had not woken yet.
    for (const std::shared_ptr<ScanJob>& one : blocking) {
      for (int i = 0; (i < 4000) && !one->begun.load(std::memory_order_acquire); ++i) usleep(500);
      EXPECT_TRUE(one->begun.load(std::memory_order_acquire));
    }

    // The filter of the queued scan leaves a trace, which is the observable for
    // "never ran": it is the first thing the job body does, it is a subprocess
    // nothing can cancel once started, and skipping it is the whole point of
    // testing the stop flag before it rather than only inside the scan loop.
    const std::filesystem::path marker = scratch.dir / "the-filter-ran";
    ed.settings.file_filter = "touch " + marker.string() + "; " + find;
    SearchExcerpts(ed, "queuedword");
    const std::shared_ptr<ScanJob> waiting = ed.pending_commands.back().scan;
    EXPECT_TRUE(waiting != nullptr);
    EXPECT_FALSE(waiting->begun.load(std::memory_order_acquire));

    // The status line agrees, while it waits, that it is waiting: the spinner
    // used to read "queuedword 3s" for a job that had not run for any of those
    // three seconds.
    EXPECT_TRUE(right_of(ed).find("queued") != std::string::npos);

    EXPECT_TRUE(CancelCommandJob(ed));
    // Not "after 0s", which reads as a scan that found nothing in no time.
    EXPECT_TRUE(ed.status.find("while it was queued") != std::string::npos);
    EXPECT_TRUE(ed.status.find(" after ") == std::string::npos);

    // Whatever is on the bar now is a scan that is genuinely running.
    EXPECT_TRUE(right_of(ed).find("queued") == std::string::npos);

    EXPECT_TRUE(settle(waiting));
    for (const std::shared_ptr<ScanJob>& one : blocking) EXPECT_TRUE(settle(one));
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(marker, ec));
    EXPECT_TRUE(waiting->hits.empty());
    KillAllCommandJobs(ed);
  }

  TEST_CASE("scan jobs: age is measured from when the work started, not when it was asked for");
  {
    // The 600 s watchdog and :from-cancel both read this, so a job stamped at
    // enqueue could "time out" having never run once the pool had a queue.
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    ed.settings.file_filter = find;
    SearchExcerpts(ed, "widget");
    const std::shared_ptr<ScanJob> one = ed.pending_commands.back().scan;
    EXPECT_TRUE(settle(one));
    EXPECT_TRUE(one->begun.load(std::memory_order_acquire));
    // Stamped when the worker picked it up, which is no earlier than the ask.
    const auto asked = ed.pending_commands.back().started.time_since_epoch().count();
    EXPECT_TRUE(one->begun_at.load(std::memory_order_relaxed) >= asked);
    EXPECT_TRUE(one->begun_at.load(std::memory_order_relaxed) <=
                std::chrono::steady_clock::now().time_since_epoch().count());
    KillAllCommandJobs(ed);
  }
}

}  // namespace koi
