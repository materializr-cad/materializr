// The pre-filled GitHub issue the "Report a bug" dialog opens in the browser.

#include "core/BugReport.h"

#include <gtest/gtest.h>

using namespace materializr::bugreport;

TEST(BugReport, UrlEncodeLeavesUnreservedAlone) {
    EXPECT_EQ(urlEncode("AZaz09-_.~"), "AZaz09-_.~");
}

TEST(BugReport, UrlEncodeEscapesEverythingElse) {
    EXPECT_EQ(urlEncode("a b&c=d#e\n"), "a%20b%26c%3Dd%23e%0A");
    // UTF-8 is encoded byte by byte.
    EXPECT_EQ(urlEncode("\xC3\xA9"), "%C3%A9");
}

TEST(BugReport, BodyCarriesTheEnvironmentAndTheDescription) {
    Env e;
    e.version = "1.8.0-beta.6";
    e.os = "Android";
    e.arch = "arm64";
    e.graphics = "Adreno (TM) 650";
    const std::string b = buildBody(e, "It froze.", "");
    EXPECT_NE(b.find("It froze."), std::string::npos);
    EXPECT_NE(b.find("- Materializr: 1.8.0-beta.6"), std::string::npos);
    EXPECT_NE(b.find("- OS: Android (arm64)"), std::string::npos);
    EXPECT_NE(b.find("- Graphics: Adreno (TM) 650"), std::string::npos);
    // Unknown fields are left out, not printed empty.
    EXPECT_EQ(b.find("OpenCASCADE"), std::string::npos);
    EXPECT_EQ(b.find("Attachments"), std::string::npos);
}

TEST(BugReport, EmptyDescriptionGetsAPrompt) {
    EXPECT_NE(buildBody(Env{}, "", "").find("please describe"), std::string::npos);
}

TEST(BugReport, AttachmentsAreListedWhenPresent) {
    const std::string b = buildBody(Env{}, "x", "- materializr-bug-1.png\n");
    EXPECT_NE(b.find("### Attachments"), std::string::npos);
    EXPECT_NE(b.find("materializr-bug-1.png"), std::string::npos);
}

TEST(BugReport, ShortReportIsNotCut) {
    const std::string url = buildIssueUrl("https://github.com/o/r/issues/new", "T", "body");
    EXPECT_EQ(url, "https://github.com/o/r/issues/new?title=T&body=body");
}

TEST(BugReport, EmptyTitleFallsBack) {
    const std::string url = buildIssueUrl("https://x/new", "", "b");
    EXPECT_NE(url.find("title=Bug%20report"), std::string::npos);
}

TEST(BugReport, LongReportIsShortenedToFitTheLink) {
    std::string big;
    for (int i = 0; i < 4000; ++i) big += "word \xC3\xA9 ";   // multi-byte too
    const std::string url = buildIssueUrl("https://github.com/o/r/issues/new", "T", big, 6000);
    EXPECT_LE(url.size(), 6000u);
    EXPECT_NE(url.find(urlEncode("shortened")), std::string::npos);
    // The cut never leaves half a UTF-8 sequence (which would encode as a lone
    // %C3 / %A9 at the join): every %C3 is followed by %A9.
    for (size_t p = url.find("%C3"); p != std::string::npos; p = url.find("%C3", p + 1))
        EXPECT_EQ(url.compare(p + 3, 3, "%A9"), 0);
}

TEST(BugReport, TruncateUtf8NeverSplitsASequence) {
    const std::string s = "a\xC3\xA9";      // a, e-acute (2 bytes)
    EXPECT_EQ(truncateUtf8(s, 2), "a");     // would land mid-sequence
    EXPECT_EQ(truncateUtf8(s, 3), s);
}

TEST(BugReport, FileStemIsSortableAndPlain) {
    EXPECT_EQ(fileStem(2026, 11, 7, 4, 5, 6), "materializr-bug-20261107-040506");
}

TEST(BugReport, KindIsStatedAtTheTopOfTheBody) {
    const std::string b = buildBody(Env{}, "x", "", "Feature");
    EXPECT_EQ(b.rfind("**Type:** Feature", 0), 0u);
    EXPECT_EQ(buildBody(Env{}, "x", "").find("**Type:**"), std::string::npos);
}

TEST(BugReport, LabelIsAddedToTheLinkWhenGiven) {
    const std::string url = buildIssueUrl("https://x/new", "T", "b", 6000, "enhancement");
    EXPECT_EQ(url, "https://x/new?title=T&labels=enhancement&body=b");
    EXPECT_EQ(buildIssueUrl("https://x/new", "T", "b").find("labels"), std::string::npos);
}
