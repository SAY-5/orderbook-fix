// SPDX-License-Identifier: MIT
#include "fix/parser.h"
#include "fix/serializer.h"

#include <gtest/gtest.h>

using namespace obfix::fix;

namespace {
// Build a wire message using the serializer so the test corpus is
// internally consistent. Returns bytes with '|' as SOH (kSohText).
std::string wire(const std::vector<std::pair<int, std::string>>& body) {
    Serializer s(kSohText);
    return s.build(body);
}
}  // namespace

TEST(FixParser, ChecksumValueIs3DigitZeroPadded) {
    EXPECT_EQ(format_checksum(0), "000");
    EXPECT_EQ(format_checksum(7), "007");
    EXPECT_EQ(format_checksum(99), "099");
    EXPECT_EQ(format_checksum(255), "255");
    EXPECT_EQ(format_checksum(256), "000");  // mod 256
}

TEST(FixParser, ChecksumIsByteSumMod256) {
    std::string s = "abc";  // 97+98+99 = 294, mod 256 = 38
    EXPECT_EQ(checksum(s), 38u);
}

TEST(FixParser, ParseLogon) {
    std::string m = wire({{35, "A"},
                          {49, "CLIENT"},
                          {56, "OBFIX"},
                          {34, "1"},
                          {52, "20260513-00:00:00.000"},
                          {108, "30"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.consumed, m.size());
    EXPECT_EQ(r.msg.msg_type(), "A");
    EXPECT_EQ(r.msg.get(49).value_or(""), "CLIENT");
    EXPECT_EQ(r.msg.get(108).value_or(""), "30");
}

TEST(FixParser, ParseNewOrderSingleAllTags) {
    std::string m = wire({
        {35, "D"},
        {49, "C"},
        {56, "S"},
        {34, "2"},
        {52, "20260513-00:00:00.000"},
        {11, "abc-1"},
        {55, "SYM"},
        {54, "1"},
        {40, "2"},
        {38, "100"},
        {44, "10000"},
        {60, "20260513-00:00:00.001"},
    });
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(11).value_or(""), "abc-1");
    EXPECT_EQ(r.msg.get(38).value_or(""), "100");
    EXPECT_EQ(r.msg.get(44).value_or(""), "10000");
    EXPECT_EQ(r.msg.get(54).value_or(""), "1");
}

TEST(FixParser, ParseCancelRequest) {
    std::string m = wire({
        {35, "F"},
        {49, "C"},
        {56, "S"},
        {34, "5"},
        {41, "orig-1"},
        {11, "cxl-1"},
        {55, "SYM"},
        {54, "1"},
        {60, "20260513-00:00:00.002"},
    });
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(41).value_or(""), "orig-1");
    EXPECT_EQ(r.msg.get(11).value_or(""), "cxl-1");
}

TEST(FixParser, ParseHeartbeat) {
    std::string m = wire({{35, "0"}, {49, "C"}, {56, "S"}, {34, "3"}, {52, "x"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.msg_type(), "0");
}

TEST(FixParser, ParseTestRequest) {
    std::string m = wire({{35, "1"}, {49, "C"}, {56, "S"}, {34, "4"}, {112, "TID-1"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(112).value_or(""), "TID-1");
}

TEST(FixParser, ParseLogout) {
    std::string m = wire({{35, "5"}, {49, "C"}, {56, "S"}, {34, "9"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.msg_type(), "5");
}

TEST(FixParser, ParseExecutionReport) {
    std::string m = wire({
        {35, "8"},
        {49, "S"},
        {56, "C"},
        {34, "1"},
        {37, "1"},
        {11, "abc-1"},
        {17, "1-A"},
        {150, "0"},
        {39, "0"},
        {151, "100"},
        {14, "0"},
        {6, "0"},
    });
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.msg_type(), "8");
    EXPECT_EQ(r.msg.get(150).value_or(""), "0");
}

TEST(FixParser, RejectIncompleteHeader) {
    Parser p(kSohText);
    EXPECT_EQ(p.parse_one("8=FIX.4").err, ParseError::Incomplete);
    EXPECT_EQ(p.parse_one("").err, ParseError::Incomplete);
}

TEST(FixParser, RejectBadBegin) {
    Parser p(kSohText);
    std::string bad = "8=FIX.4.2|9=10|35=A|10=000|";
    auto r = p.parse_one(bad);
    EXPECT_EQ(r.err, ParseError::BadHeader);
}

TEST(FixParser, RejectMissingBodyLength) {
    Parser p(kSohText);
    std::string bad = "8=FIX.4.4|35=A|10=000|";
    auto r = p.parse_one(bad);
    EXPECT_EQ(r.err, ParseError::BadHeader);
}

TEST(FixParser, RejectNonNumericBodyLength) {
    Parser p(kSohText);
    std::string bad = "8=FIX.4.4|9=abc|35=A|10=000|";
    auto r = p.parse_one(bad);
    EXPECT_EQ(r.err, ParseError::BadBodyLength);
}

TEST(FixParser, RejectChecksumMismatch) {
    Parser p(kSohText);
    // Build a valid message, then corrupt the checksum.
    std::string m = wire({{35, "0"}, {49, "C"}, {56, "S"}, {34, "1"}});
    // Replace last 4 chars before SOH with bad checksum.
    auto soh = m.rfind('|', m.size() - 2);
    ASSERT_NE(soh, std::string::npos);
    auto eq = m.rfind('=', soh);
    m.replace(eq + 1, 3, "000");
    auto r = p.parse_one(m);
    EXPECT_EQ(r.err, ParseError::BadCheckSum);
}

TEST(FixParser, RejectTruncatedBody) {
    Parser p(kSohText);
    std::string m = wire({{35, "0"}, {49, "C"}, {56, "S"}, {34, "1"}});
    // Strip the last 5 bytes.
    auto r = p.parse_one(std::string_view(m).substr(0, m.size() - 5));
    EXPECT_EQ(r.err, ParseError::Incomplete);
}

TEST(FixParser, RejectMalformedTag) {
    Parser p(kSohText);
    // Tag "abc" is not numeric.
    std::string body = "abc=x|";
    std::string msg = "8=FIX.4.4|9=";
    msg += std::to_string(body.size());
    msg += "|";
    msg += body;
    // Append a fake checksum so we get past the framing and hit the field walk.
    std::string upto = msg;
    unsigned cs = checksum(upto);
    msg += "10=";
    msg += format_checksum(cs);
    msg += "|";
    auto r = p.parse_one(msg);
    EXPECT_EQ(r.err, ParseError::MalformedField);
}

TEST(FixParser, RejectDuplicateTag) {
    Parser p(kSohText);
    std::string m = wire({{35, "A"}, {49, "C"}, {49, "D"}, {56, "S"}, {34, "1"}});
    auto r = p.parse_one(m);
    EXPECT_EQ(r.err, ParseError::DuplicateField);
}

TEST(FixParser, AcceptZeroLengthValue) {
    Parser p(kSohText);
    std::string m = wire({{35, "A"}, {49, ""}, {56, "S"}, {34, "1"}});
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(49).value_or("X"), "");
}

TEST(FixParser, MultipleMessagesInBuffer) {
    Parser p(kSohText);
    std::string m1 = wire({{35, "0"}, {49, "C"}, {56, "S"}, {34, "1"}});
    std::string m2 = wire({{35, "0"}, {49, "C"}, {56, "S"}, {34, "2"}});
    std::string buf = m1 + m2;
    auto r1 = p.parse_one(buf);
    ASSERT_EQ(r1.err, ParseError::None);
    EXPECT_EQ(r1.consumed, m1.size());
    auto r2 = p.parse_one(std::string_view(buf).substr(r1.consumed));
    ASSERT_EQ(r2.err, ParseError::None);
    EXPECT_EQ(r2.consumed, m2.size());
}

TEST(FixParser, SerializerProducesValidChecksum) {
    Serializer s(kSohText);
    Parser p(kSohText);
    auto wire = s.build({{35, "A"}, {49, "C"}, {56, "S"}, {34, "1"}, {108, "30"}});
    auto r = p.parse_one(wire);
    EXPECT_EQ(r.err, ParseError::None);
}

TEST(FixParser, EmptyBodyMessage) {
    // 8=FIX.4.4|9=0|10=NNN| would have body_len 0; we never emit such a
    // message but the parser should reject it as MalformedField (it has no
    // MsgType).
    Parser p(kSohText);
    std::string msg = "8=FIX.4.4|9=0|";
    unsigned cs = checksum(msg);
    msg += "10=";
    msg += format_checksum(cs);
    msg += "|";
    auto r = p.parse_one(msg);
    // Body of length 0 is technically parseable but has no MsgType.
    // The parser does not enforce MsgType presence; session layer does.
    EXPECT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.msg_type(), "");
}

TEST(FixParser, LargeQuantityField) {
    std::string m =
        wire({{35, "D"}, {49, "C"}, {56, "S"}, {34, "1"}, {38, "1000000000"}, {44, "999999"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(38).value_or(""), "1000000000");
}

TEST(FixParser, SohInsideValueIsRejected) {
    // We do not support length-prefixed XML-like fields. A SOH inside a
    // value just terminates that field early; the next chunk becomes a
    // malformed tag.
    Parser p(kSohText);
    std::string raw = "8=FIX.4.4|9=10|35=A|x|10=000|";
    auto r = p.parse_one(raw);
    EXPECT_NE(r.err, ParseError::None);
}

TEST(FixParser, ParseSequenceReset) {
    std::string m = wire({{35, "4"}, {49, "C"}, {56, "S"}, {34, "1"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.msg_type(), "4");
}

TEST(FixParser, ParseResendRequest) {
    std::string m = wire({{35, "2"}, {49, "C"}, {56, "S"}, {34, "1"}, {7, "5"}, {16, "0"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(7).value_or(""), "5");
}

TEST(FixParser, ParseReject) {
    std::string m = wire({{35, "3"}, {49, "C"}, {56, "S"}, {34, "1"}, {45, "2"}, {58, "bad tag"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(58).value_or(""), "bad tag");
}

TEST(FixParser, ParseCancelReplace) {
    std::string m = wire({
        {35, "G"},
        {49, "C"},
        {56, "S"},
        {34, "5"},
        {41, "orig-1"},
        {11, "rpl-1"},
        {55, "SYM"},
        {54, "1"},
        {38, "200"},
        {44, "10001"},
        {40, "2"},
    });
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.msg_type(), "G");
    EXPECT_EQ(r.msg.get(38).value_or(""), "200");
}

TEST(FixParser, NumericTagBoundary) {
    // Tag 99999 is allowed.
    std::string m = wire({{35, "A"}, {49, "C"}, {56, "S"}, {34, "1"}, {99999, "v"}});
    Parser p(kSohText);
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(99999).value_or(""), "v");
}

TEST(FixParser, RejectMissingChecksum) {
    Parser p(kSohText);
    std::string msg = "8=FIX.4.4|9=5|35=A|";  // no trailing 10=
    auto r = p.parse_one(msg);
    EXPECT_EQ(r.err, ParseError::Incomplete);
}

TEST(FixParser, RejectShortChecksum) {
    Parser p(kSohText);
    std::string msg = "8=FIX.4.4|9=5|35=A|10=00|";  // 2-digit
    auto r = p.parse_one(msg);
    EXPECT_NE(r.err, ParseError::None);
}

TEST(FixParser, BodyLengthMismatchTreatedAsChecksumFail) {
    // If BodyLength understates, the byte at body_end will not be '1' of
    // "10=", and we report BadCheckSum.
    Parser p(kSohText);
    std::string body = "35=A|49=C|56=S|34=1|";
    std::string msg = "8=FIX.4.4|9=";
    msg += std::to_string(body.size() - 5);  // lie about length
    msg += "|";
    msg += body;
    std::string upto = msg;
    unsigned cs = checksum(upto);
    msg += "10=";
    msg += format_checksum(cs);
    msg += "|";
    auto r = p.parse_one(msg);
    EXPECT_EQ(r.err, ParseError::BadCheckSum);
}

TEST(FixParser, PreservesBeginStringAndBodyLengthInMap) {
    Parser p(kSohText);
    std::string m = wire({{35, "0"}, {49, "C"}, {56, "S"}, {34, "1"}});
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(8).value_or(""), "FIX.4.4");
    EXPECT_FALSE(r.msg.get(9).value_or("").empty());
    EXPECT_FALSE(r.msg.get(10).value_or("").empty());
}

TEST(FixParser, NegativePriceNotRejectedAtParseLayer) {
    // Parser is content-agnostic; price validation is the session/app job.
    Parser p(kSohText);
    std::string m = wire({{35, "D"}, {49, "C"}, {56, "S"}, {34, "1"}, {44, "-100"}});
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(44).value_or(""), "-100");
}

TEST(FixParser, RoundTripFidelity) {
    Serializer s(kSohText);
    Parser p(kSohText);
    auto out1 = s.build({{35, "D"},
                         {49, "C"},
                         {56, "S"},
                         {34, "1"},
                         {11, "abc"},
                         {38, "100"},
                         {44, "1000"},
                         {54, "1"}});
    auto r = p.parse_one(out1);
    ASSERT_EQ(r.err, ParseError::None);
    // Rebuild from the parsed map (omit 8/9/10, which the serializer prepends).
    std::vector<std::pair<int, std::string>> body;
    for (auto& kv : r.msg.fields) {
        if (kv.first == 8 || kv.first == 9 || kv.first == 10) continue;
        body.emplace_back(kv.first, kv.second);
    }
    auto out2 = s.build(body);
    auto r2 = p.parse_one(out2);
    EXPECT_EQ(r2.err, ParseError::None);
    EXPECT_EQ(r2.msg.get(11).value_or(""), "abc");
}

TEST(FixParser, IgnoresUnknownTagsAtParseLayer) {
    Parser p(kSohText);
    std::string m = wire({{35, "A"}, {49, "C"}, {56, "S"}, {34, "1"}, {12345, "extra"}});
    auto r = p.parse_one(m);
    ASSERT_EQ(r.err, ParseError::None);
    EXPECT_EQ(r.msg.get(12345).value_or(""), "extra");
}
