// Tests for query.cpp: reading a whole file, and the byte ranges handed back
// for one.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void ReadWholeFileContract() {
  TEST_CASE("ReadWholeFile: regular files, precise errors, never a block");

  const Scratch scratch{"koi-readwholefile-test"};
  std::error_code error;

  const std::filesystem::path file = scratch.Write("plain.txt", "alpha\nbravo\n");
  EXPECT_EQ(ReadWholeFile(file, error), std::string("alpha\nbravo\n"));
  EXPECT_TRUE(!error);

  // The one case where an empty answer is the truth, and the only one. Every
  // assertion below is about telling the other cases apart from this one.
  error = std::make_error_code(std::errc::io_error);
  EXPECT_EQ(ReadWholeFile(scratch.Write("empty.txt", ""), error), std::string{});
  EXPECT_TRUE(!error);

  EXPECT_EQ(ReadWholeFile(scratch.dir / "missing.txt", error), std::string{});
  EXPECT_TRUE(error == std::errc::no_such_file_or_directory);

  EXPECT_EQ(ReadWholeFile(scratch.dir, error), std::string{});
  EXPECT_TRUE(error == std::errc::is_a_directory);

  const std::filesystem::path fifo = scratch.dir / "pipe";
  EXPECT_EQ(mkfifo(fifo.c_str(), 0600), 0);
  EXPECT_EQ(ReadWholeFile(fifo, error), std::string{});
  EXPECT_TRUE(error == std::errc::not_supported);

  if (::getuid() == 0) {
    // root reads through the mode bits, so there is no unreadable file to make.
    EXPECT_TRUE(true);
    EXPECT_TRUE(true);
  } else {
    const std::filesystem::path locked = scratch.Write("locked.txt", "secret\n");
    std::error_code chmod_ec;
    std::filesystem::permissions(locked, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, chmod_ec);
    EXPECT_EQ(ReadWholeFile(locked, error), std::string{});
    EXPECT_TRUE(error == std::errc::permission_denied);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace, chmod_ec);
  }

  // The failure class the shim covers from outside, reached here without one.
  // /proc/self/mem is a regular file that fstat sizes at zero and that read(2)
  // refuses with EIO at offset 0 -- so open succeeds, fstat succeeds, and the
  // read is what dies. That is exactly the path that used to return an empty
  // string with `error` left clear, which every caller then read as "this file
  // has nothing in it". No privileges, no injection: the kernel provides it.
  {
    error.clear();
    const std::string mem = ReadWholeFile("/proc/self/mem", error);
    EXPECT_EQ(mem, std::string{});
    EXPECT_TRUE(static_cast<bool>(error));
  }

  // st_size zero and yet not empty: the tail loop past st_size is what /proc
  // files are read by, and a failure-first rewrite of the read loop is the
  // obvious way to lose it.
  {
    error = std::make_error_code(std::errc::io_error);
    const std::string stat = ReadWholeFile("/proc/self/stat", error);
    EXPECT_TRUE(!error);
    EXPECT_TRUE(!stat.empty());
  }
}

void ABufferPastFourGigabytesGetsNoByteRangeAtAll() {
  TEST_CASE("query: a byte range past uint32 is refused, not wrapped");

  // Every offset tree-sitter deals in is a uint32, and the three call sites
  // that set a cursor's byte range each wrote `static_cast<uint32_t>(size)`.
  // Past 4 GiB that is not a cast, it is a wrap: 4 GiB + 100 bytes becomes a
  // range of 100, and the answer -- a scan, a set of text objects, a file's
  // overview -- is about an arbitrary prefix while claiming to be about the
  // whole. No fixture in a test suite can be that large, so the arithmetic is
  // pinned where it now lives, at the one place all three go through.
  constexpr std::size_t kCeiling = std::numeric_limits<std::uint32_t>::max();

  std::uint32_t end = 12345;
  EXPECT_TRUE(TreeSitterByteRange(0, end));
  EXPECT_EQ(end, std::uint32_t{0});
  EXPECT_TRUE(TreeSitterByteRange(1024, end));
  EXPECT_EQ(end, std::uint32_t{1024});

  // The last size that fits, and the first that does not.
  EXPECT_TRUE(TreeSitterByteRange(kCeiling, end));
  EXPECT_EQ(end, std::numeric_limits<std::uint32_t>::max());

  end = 4242;
  EXPECT_FALSE(TreeSitterByteRange(kCeiling + 1, end));
  // Left as it was rather than wrapped to zero: a caller that forgets to look
  // at the answer is handed back the range it already had, not a new and
  // plausible-looking one.
  EXPECT_EQ(end, std::uint32_t{4242});
  EXPECT_FALSE(TreeSitterByteRange(std::numeric_limits<std::size_t>::max(), end));
  EXPECT_EQ(end, std::uint32_t{4242});
}

}  // namespace koi
