// Tests for the shell-history wire protocol. This is the layer where a mistake
// silently costs history entries -- the shell hook has nobody to report to -- so
// the emphasis is on round-tripping and on every way a datagram can be malformed.

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "protocol.h"
#include "test_harness.h"

using namespace ghatothkacha;

namespace {

constexpr char kUS = kDelimiter;

std::string Datagram(std::string_view action, std::initializer_list<std::string_view> fields) {
  std::string out(action);
  for (std::string_view f : fields) {
    out.push_back(kUS);
    out.append(f);
  }
  return out;
}

HistoryItem InsertFixture() {
  HistoryItem item;
  item.id = "1234";
  item.cmd = "git commit -m 'fix'";
  item.dir = "/home/user/project";
  item.start_timestamp_ns = 1719000000000000000ull;
  return item;
}

HistoryItem UpdateFixture() {
  HistoryItem item;
  item.id = "1234";
  item.end_timestamp_ns = 1719000000123456789ull;
  item.retcode = 0;
  return item;
}

}  // namespace

static void TestStrip() {
  TEST_CASE("Strip trims both ends");
  EXPECT_EQ(Strip("  hello  "), "hello");
  EXPECT_EQ(Strip("hello"), "hello");
  EXPECT_EQ(Strip(""), "");
  EXPECT_EQ(Strip("   "), "");
  EXPECT_EQ(Strip("\t\n x \r\n"), "x");
  EXPECT_EQ(Strip("a b"), "a b");  // interior space survives
  // A high byte is not whitespace; an unsigned cast to isspace is what guarantees
  // this, and a UTF-8 command must not be trimmed into nonsense.
  EXPECT_EQ(Strip("\xC3\xA9"), "\xC3\xA9");
  EXPECT_EQ(Strip(" \xC3\xA9 "), "\xC3\xA9");
}

static void TestStringToInt() {
  TEST_CASE("StringToInt accepts whole numbers");
  EXPECT_EQ(StringToInt<uint64_t>("0").value_or(99u), 0u);
  EXPECT_EQ(StringToInt<uint64_t>("1719000000000000000").value_or(0u), 1719000000000000000ull);
  EXPECT_EQ(StringToInt<int>("-1").value_or(0), -1);
  EXPECT_EQ(StringToInt<int>("130").value_or(0), 130);
  EXPECT_EQ(StringToInt<uint64_t>(std::to_string(std::numeric_limits<uint64_t>::max())).value_or(0u),
            std::numeric_limits<uint64_t>::max());

  TEST_CASE("StringToInt rejects what is not a whole number");
  EXPECT_FALSE(StringToInt<uint64_t>("").has_value());
  EXPECT_FALSE(StringToInt<uint64_t>("abc").has_value());
  EXPECT_FALSE(StringToInt<uint64_t>(" 12").has_value());   // from_chars will not skip space
  EXPECT_FALSE(StringToInt<uint64_t>("-1").has_value());    // unsigned target
  EXPECT_FALSE(StringToInt<uint64_t>("99999999999999999999999").has_value());  // out of range
  // from_chars stops at the first non-digit and reports success on the prefix, so a
  // trailing tail is silently accepted. Documented here because a corrupt timestamp
  // would otherwise be stored rather than dropped.
  EXPECT_EQ(StringToInt<uint64_t>("12abc").value_or(0u), 12u);
}

static void TestFieldValidation() {
  TEST_CASE("ToInsertItem accepts a complete record");
  {
    const auto item = ToInsertItem("id1", "ls -la", "/tmp", "42");
    EXPECT_TRUE(item.has_value());
    if (item) {
      EXPECT_EQ(item->id, "id1");
      EXPECT_EQ(item->cmd, "ls -la");
      EXPECT_EQ(item->dir, "/tmp");
      EXPECT_EQ(item->start_timestamp_ns, 42u);
    }
  }

  TEST_CASE("ToInsertItem requires every field");
  EXPECT_FALSE(ToInsertItem("", "ls", "/tmp", "42").has_value());
  EXPECT_FALSE(ToInsertItem("id", "", "/tmp", "42").has_value());
  EXPECT_FALSE(ToInsertItem("id", "ls", "", "42").has_value());
  EXPECT_FALSE(ToInsertItem("id", "ls", "/tmp", "").has_value());
  // Blank is as empty as empty: a command of only spaces is not history.
  EXPECT_FALSE(ToInsertItem("  ", "ls", "/tmp", "42").has_value());
  EXPECT_FALSE(ToInsertItem("id", "   ", "/tmp", "42").has_value());
  EXPECT_FALSE(ToInsertItem("id", "ls", "\t\n", "42").has_value());
  // A timestamp that does not parse is rejected rather than stored as zero.
  EXPECT_FALSE(ToInsertItem("id", "ls", "/tmp", "not-a-number").has_value());

  TEST_CASE("validation checks the stripped value but stores the original");
  {
    // The command is stored verbatim -- leading whitespace in a command is real --
    // while validation only asks whether anything is there.
    const auto item = ToInsertItem("id", "  ls  ", "/tmp", "42");
    EXPECT_TRUE(item.has_value());
    if (item)
      EXPECT_EQ(item->cmd, "  ls  ");
  }

  TEST_CASE("ToUpdateItem accepts a complete record");
  {
    const auto item = ToUpdateItem("id1", "99", "130");
    EXPECT_TRUE(item.has_value());
    if (item) {
      EXPECT_EQ(item->id, "id1");
      EXPECT_EQ(item->end_timestamp_ns, 99u);
      EXPECT_EQ(item->retcode, 130);
    }
  }
  {
    // A negative return code is legitimate and must survive.
    const auto item = ToUpdateItem("id", "1", "-1");
    EXPECT_TRUE(item.has_value());
    if (item)
      EXPECT_EQ(item->retcode, -1);
  }
  {
    // retcode 0 is the common case and must not be confused with a missing field.
    const auto item = ToUpdateItem("id", "1", "0");
    EXPECT_TRUE(item.has_value());
    if (item)
      EXPECT_EQ(item->retcode, 0);
  }

  TEST_CASE("ToUpdateItem requires every field");
  EXPECT_FALSE(ToUpdateItem("", "1", "0").has_value());
  EXPECT_FALSE(ToUpdateItem("id", "", "0").has_value());
  EXPECT_FALSE(ToUpdateItem("id", "1", "").has_value());
  EXPECT_FALSE(ToUpdateItem("id", "x", "0").has_value());
  EXPECT_FALSE(ToUpdateItem("id", "1", "x").has_value());
  EXPECT_FALSE(ToUpdateItem(" ", "1", "0").has_value());
}

static void TestRoundTrip() {
  TEST_CASE("an insert survives encode then decode");
  {
    const HistoryItem sent = InsertFixture();
    const std::string wire{EncodeMessage(sent, false)};
    const Message got = DecodeMessage(wire);
    EXPECT_TRUE(got.action == Action::kInsert);
    EXPECT_EQ(got.item.id, sent.id);
    EXPECT_EQ(got.item.cmd, sent.cmd);
    EXPECT_EQ(got.item.dir, sent.dir);
    EXPECT_EQ(got.item.start_timestamp_ns, sent.start_timestamp_ns);
  }

  TEST_CASE("an update survives encode then decode");
  {
    const HistoryItem sent = UpdateFixture();
    const std::string wire{EncodeMessage(sent, true)};
    const Message got = DecodeMessage(wire);
    EXPECT_TRUE(got.action == Action::kUpdate);
    EXPECT_EQ(got.item.id, sent.id);
    EXPECT_EQ(got.item.end_timestamp_ns, sent.end_timestamp_ns);
    EXPECT_EQ(got.item.retcode, sent.retcode);
  }

  TEST_CASE("the wire format is what the daemon expects");
  {
    const HistoryItem sent = InsertFixture();
    const std::string wire{EncodeMessage(sent, false)};
    EXPECT_EQ(wire.front(), 'I');
    EXPECT_EQ(wire[1], kUS);
    // Four fields means four delimiters, one before each.
    EXPECT_EQ(static_cast<size_t>(std::count(wire.begin(), wire.end(), kUS)), 4u);
    // cmd is last, which is what lets a command hold spaces and quotes freely.
    EXPECT_TRUE(wire.ends_with(std::string{sent.cmd}));
  }
  {
    const std::string wire{EncodeMessage(UpdateFixture(), true)};
    EXPECT_EQ(wire.front(), 'U');
    EXPECT_EQ(static_cast<size_t>(std::count(wire.begin(), wire.end(), kUS)), 3u);
  }

  TEST_CASE("payloads that stress the format");
  for (std::string_view cmd : {"echo 'quoted'", "a\tb", "grep -r \"x\" .", "caf\xC3\xA9 --utf8",
                               "echo $HOME && ls | wc -l", "a\nb"}) {
    HistoryItem sent = InsertFixture();
    sent.cmd = cmd;
    const std::string wire{EncodeMessage(sent, false)};
    const Message got = DecodeMessage(wire);
    EXPECT_TRUE(got.action == Action::kInsert);
    EXPECT_EQ(got.item.cmd, cmd);
  }
  {
    // A long command, well past any single field the shell usually produces.
    HistoryItem sent = InsertFixture();
    const std::string long_cmd = "echo " + std::string(60000, 'x');
    sent.cmd = long_cmd;
    const std::string wire{EncodeMessage(sent, false)};
    const Message got = DecodeMessage(wire);
    EXPECT_TRUE(got.action == Action::kInsert);
    EXPECT_EQ(got.item.cmd.size(), long_cmd.size());
  }
  {
    // Extreme timestamps and return codes.
    HistoryItem sent = UpdateFixture();
    sent.end_timestamp_ns = std::numeric_limits<uint64_t>::max();
    sent.retcode = -1;
    const std::string wire{EncodeMessage(sent, true)};
    const Message got = DecodeMessage(wire);
    EXPECT_TRUE(got.action == Action::kUpdate);
    EXPECT_EQ(got.item.end_timestamp_ns, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(got.item.retcode, -1);
  }
}

static void TestDecodeRejections() {
  TEST_CASE("truncated and empty datagrams");
  EXPECT_TRUE(DecodeMessage("").action == Action::kNone);
  EXPECT_TRUE(DecodeMessage("I").action == Action::kNone);
  EXPECT_TRUE(DecodeMessage("U").action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(std::string_view(&kUS, 1)).action == Action::kNone);

  TEST_CASE("a missing leading delimiter is rejected");
  EXPECT_TRUE(DecodeMessage("Iid\x1F""1\x1F/tmp\x1Fls").action == Action::kNone);
  EXPECT_TRUE(DecodeMessage("I id\x1F""1\x1F/tmp\x1Fls").action == Action::kNone);

  TEST_CASE("an unknown action byte is ignored");
  EXPECT_TRUE(DecodeMessage(Datagram("X", {"id", "1", "/tmp", "ls"})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("i", {"id", "1", "/tmp", "ls"})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("u", {"id", "1", "0"})).action == Action::kNone);

  TEST_CASE("field counts are exact");
  // Insert needs exactly 4.
  EXPECT_TRUE(DecodeMessage(Datagram("I", {"id", "1", "/tmp"})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("I", {"id", "1", "/tmp", "ls", "extra"})).action == Action::kNone);
  // Update needs exactly 3.
  EXPECT_TRUE(DecodeMessage(Datagram("U", {"id", "1"})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("U", {"id", "1", "0", "extra"})).action == Action::kNone);
  // The right count but the wrong action.
  EXPECT_TRUE(DecodeMessage(Datagram("U", {"id", "1", "/tmp", "ls"})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("I", {"id", "1", "0"})).action == Action::kNone);

  TEST_CASE("a delimiter inside a command drops the entry rather than mangling it");
  {
    // 0x1F is a unit separator precisely so this does not happen in practice, but
    // the outcome must be a dropped entry, not a truncated command in the database.
    HistoryItem sent = InsertFixture();
    const std::string embedded = std::string("ls") + kUS + "rm -rf /";
    sent.cmd = embedded;
    const std::string wire{EncodeMessage(sent, false)};
    EXPECT_TRUE(DecodeMessage(wire).action == Action::kNone);
  }

  TEST_CASE("field-level validation still applies after a clean split");
  EXPECT_TRUE(DecodeMessage(Datagram("I", {"", "1", "/tmp", "ls"})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("I", {"id", "1", "/tmp", ""})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("I", {"id", "notanumber", "/tmp", "ls"})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("U", {"id", "1", "notanumber"})).action == Action::kNone);
  // All-empty fields: the right shape, no content.
  EXPECT_TRUE(DecodeMessage(Datagram("I", {"", "", "", ""})).action == Action::kNone);
  EXPECT_TRUE(DecodeMessage(Datagram("U", {"", "", ""})).action == Action::kNone);

  TEST_CASE("a valid datagram built by hand is accepted");
  {
    // Proves the rejections above are rejecting for the stated reason and not
    // because the hand-built shape is wrong.
    const Message got = DecodeMessage(Datagram("I", {"id", "1", "/tmp", "ls"}));
    EXPECT_TRUE(got.action == Action::kInsert);
    EXPECT_EQ(got.item.id, "id");
    EXPECT_EQ(got.item.start_timestamp_ns, 1u);
    EXPECT_EQ(got.item.dir, "/tmp");
    EXPECT_EQ(got.item.cmd, "ls");
  }
  {
    const Message got = DecodeMessage(Datagram("U", {"id", "7", "130"}));
    EXPECT_TRUE(got.action == Action::kUpdate);
    EXPECT_EQ(got.item.end_timestamp_ns, 7u);
    EXPECT_EQ(got.item.retcode, 130);
  }
}

static void TestEncodeBufferReuse() {
  TEST_CASE("EncodeMessage's view is only valid until the next call");
  {
    // Documented behaviour, and the reason SendToDaemon copies nothing: the second
    // encode is free to reuse the buffer, so callers must send before re-encoding.
    const HistoryItem a = InsertFixture();
    const std::string first{EncodeMessage(a, false)};
    const HistoryItem b = UpdateFixture();
    const std::string second{EncodeMessage(b, true)};
    EXPECT_TRUE(first.front() == 'I');
    EXPECT_TRUE(second.front() == 'U');
    // Re-encoding the first must reproduce it exactly.
    EXPECT_EQ(std::string{EncodeMessage(a, false)}, first);
  }
}

int main() {
  TestStrip();
  TestStringToInt();
  TestFieldValidation();
  TestRoundTrip();
  TestDecodeRejections();
  TestEncodeBufferReuse();
  return common::TestSummary();
}
