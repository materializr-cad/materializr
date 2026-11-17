// The app side of "Report a Bug > Send": reading the relay's reply and building
// the environment JSON it expects. (The network calls themselves are exercised
// against the staging Worker, not here.)

#include "core/BugReport.h"
#include "core/ReportSender.h"

#include <gtest/gtest.h>

using namespace materializr;

TEST(ReportSender, SuccessReplyYieldsTheIssueUrl) {
    auto r = parseRelayReply(200,
        R"({"ok":true,"issueUrl":"https://github.com/o/r/issues/7","number":7,"id":"abc","dryRun":false})");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.issueUrl, "https://github.com/o/r/issues/7");
    EXPECT_EQ(r.httpStatus, 200);
}

TEST(ReportSender, ARelayErrorShowsItsOwnMessage) {
    auto r = parseRelayReply(429, R"({"ok":false,"message":"You've sent several reports recently. Please try again later."})");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "You've sent several reports recently. Please try again later.");
}

TEST(ReportSender, EscapedCharactersInTheMessageAreDecoded) {
    auto r = parseRelayReply(400, R"({"ok":false,"message":"The screenshot isn't \"valid\"\nsorry"})");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.message.find("\"valid\""), std::string::npos);
}

TEST(ReportSender, ANonJsonErrorFallsBackToAFriendlyMessage) {
    auto r = parseRelayReply(502, "<html>Bad gateway</html>");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.message.find("trouble"), std::string::npos);
    EXPECT_NE(parseRelayReply(429, "").message.find("several reports"), std::string::npos);
    EXPECT_NE(parseRelayReply(418, "").message.find("418"), std::string::npos);
}

TEST(ReportSender, OkTrueWithoutAnHttpsUrlIsNotSuccess) {
    // A tampered or buggy reply must never make the app open a non-https link.
    EXPECT_FALSE(parseRelayReply(200, R"j({"ok":true,"issueUrl":"javascript:alert(1)"})j").ok);
    EXPECT_FALSE(parseRelayReply(200, R"({"ok":true})").ok);
    EXPECT_FALSE(parseRelayReply(500, R"({"ok":true,"issueUrl":"https://github.com/x"})").ok);
}

TEST(ReportSender, EnvJsonUsesTheRelaysKeysAndEscapes) {
    bugreport::Env e;
    e.version = "1.8.0";
    e.os = "Android";
    e.graphics = "Adreno \"650\"\n(TM)";
    const std::string j = bugreport::envJson(e);
    EXPECT_EQ(j, R"j({"version":"1.8.0","os":"Android","graphics":"Adreno \"650\"\n(TM)"})j");
    EXPECT_EQ(bugreport::envJson(bugreport::Env{}), "{}");
    e.touchMode = "on";
    EXPECT_NE(bugreport::envJson(e).find("\"touchMode\":\"on\""), std::string::npos);
}
