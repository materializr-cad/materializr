// AI Assistant settings round-trip through the plain-text .cfg file and are
// deliberately excluded from the portable JSON export/import path (same
// treatment as lastProjectPath) so a shared settings backup can't leak an
// API key.
#include "io/Settings.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using materializr::AppSettings;
using materializr::AiProvider;
namespace SettingsIO = materializr::SettingsIO;

namespace {
std::string tmpCfg(const char* tag) {
    static int n = 0;
    return (fs::temp_directory_path() /
            ("mzr_ai_settings_" + std::string(tag) + "_" +
             std::to_string(++n) + ".cfg")).string();
}
} // namespace

TEST(AiSettings, RoundTripsThroughTheCfgFile) {
    const std::string p = tmpCfg("roundtrip");
    AppSettings s;
    s.ai.provider = AiProvider::OpenAiCompatible;
    s.ai.anthropicApiKey = "sk-ant-test123";
    s.ai.anthropicModel = "claude-sonnet-4-5";
    s.ai.openAiApiKey = "sk-openai-test456";
    s.ai.openAiBaseUrl = "http://localhost:11434/v1";
    s.ai.openAiModel = "llama3.1";
    ASSERT_TRUE(SettingsIO::save(p, s));

    AppSettings loaded = SettingsIO::load(p);
    EXPECT_EQ(loaded.ai.provider, AiProvider::OpenAiCompatible);
    EXPECT_EQ(loaded.ai.anthropicApiKey, "sk-ant-test123");
    EXPECT_EQ(loaded.ai.anthropicModel, "claude-sonnet-4-5");
    EXPECT_EQ(loaded.ai.openAiApiKey, "sk-openai-test456");
    EXPECT_EQ(loaded.ai.openAiBaseUrl, "http://localhost:11434/v1");
    EXPECT_EQ(loaded.ai.openAiModel, "llama3.1");
    fs::remove(p);
}

TEST(AiSettings, DefaultsToAnthropicWithNoKeys) {
    AppSettings s;
    EXPECT_EQ(s.ai.provider, AiProvider::Anthropic);
    EXPECT_TRUE(s.ai.anthropicApiKey.empty());
    EXPECT_TRUE(s.ai.openAiApiKey.empty());
    EXPECT_EQ(s.ai.openAiBaseUrl, "https://api.openai.com/v1");
}

TEST(AiSettings, ExcludedFromJsonExportAndImport) {
    const std::string exportPath = tmpCfg("export") + ".json";
    AppSettings s;
    s.ai.anthropicApiKey = "sk-ant-should-not-leak";
    ASSERT_TRUE(SettingsIO::exportJson(exportPath, s));

    std::ifstream f(exportPath);
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    EXPECT_EQ(body.find("sk-ant-should-not-leak"), std::string::npos)
        << "an API key must never appear in an exported settings file";

    // A malicious/foreign import file setting an AI key must not be applied.
    const std::string importPath = tmpCfg("import") + ".json";
    {
        std::ofstream f2(importPath);
        f2 << "{\n  \"aiAnthropicApiKey\": \"sk-ant-injected\"\n}\n";
    }
    bool ok = false;
    AppSettings imported = SettingsIO::importJson(importPath, &ok);
    EXPECT_TRUE(imported.ai.anthropicApiKey.empty())
        << "an imported settings file must not be able to inject an API key";
    fs::remove(exportPath);
    fs::remove(importPath);
}
