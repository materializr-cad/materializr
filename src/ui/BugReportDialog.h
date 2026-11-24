#pragma once
// Help > Report a Bug. Collects a description, a screenshot taken with this
// dialog hidden, an optional log and (optionally) a copy of the project, and
// Send posts them to the report relay (core/ReportSender), which files the
// GitHub issue - every platform, no GitHub account, no token in the binary.
// The fallback opens a pre-filled GitHub issue in the browser instead; GitHub
// has no API for attaching files there, so the screenshot / project are saved
// side by side and the user drags them into the issue (desktop: "Open folder";
// mobile: the system share sheet).

#include "../core/ReportSender.h"

#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace materializr {

class BugReportDialog {
public:
    struct Hooks {
        std::function<std::string()> layoutName;                    // "Classic" / "Modern" / "ImTouch"
        std::function<std::string()> projectName;                   // shown beside "Include your model"
        std::function<bool(const std::string&)> saveProjectCopy;   // writes the current project there
        std::function<void(const std::string&)> toast;
    };

    BugReportDialog() = default;
    ~BugReportDialog();
    BugReportDialog(const BugReportDialog&) = delete;
    BugReportDialog& operator=(const BugReportDialog&) = delete;

    void setHooks(Hooks h) { m_hooks = std::move(h); }
    // Opens the dialog and takes a fresh screenshot of whatever is on screen.
    void open();
    bool isVisible() const { return m_visible; }
    // Inside the ImGui frame.
    void render();
    // After the frame has been drawn, before the buffer swap: this is where a
    // pending screenshot is read back.
    void afterFrameRendered();

private:
    void captureFramebuffer();
    void submit();                 // fallback: open a pre-filled GitHub issue
    void startSend();              // post to the relay in the background
    void pollSend();
    void startAnother();
    bool saveAttachments(std::string& attachmentsMd, std::vector<ReportFile>& toSend);
    void refreshProjectSize();
    void freeThumb();

    Hooks m_hooks;
    bool m_visible = false;
    bool m_placeNext = false;
    int  m_hideFrames = 0;            // >0: dialog not drawn, screenshot pending

    int  m_kind = 0;                  // 0 Bug, 1 Feature, 2 Question
    char m_title[160] = {};
    char m_desc[4096] = {};
    bool m_includeShot = true;
    bool m_includeProject = false;
    bool m_includeLog = true;

    std::vector<uint8_t> m_png;       // last screenshot, PNG-encoded
    int m_shotW = 0, m_shotH = 0;
    unsigned int m_thumbTex = 0;      // GL texture of the screenshot, for the preview
    int m_thumbW = 0, m_thumbH = 0;

    std::string m_stem;               // file-name stem shared by this report's files
    std::string m_projectSize;        // "19.5 KB" once the project copy has been sized

    // Sending to the relay.
    std::future<ReportSendResult> m_send;
    bool m_sending = false;
    bool m_sent = false;
    std::string m_sentUrl;
    std::string m_sendError;

    // Result of the last fallback "open a pre-filled GitHub issue".
    bool m_submitted = false;
    std::string m_dir;
    std::vector<std::string> m_savedFiles;   // full paths
    std::string m_lastBody;                  // for "Copy report text"
    std::string m_error;
};

} // namespace materializr
