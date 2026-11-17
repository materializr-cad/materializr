#include "BugReportDialog.h"

#include "../core/BugReport.h"
#include "../core/LogCapture.h"
#include "../core/ReportSender.h"
#include "../core/Units.h"
#include "../gl_common.h"
#include "../i18n.h"
#include "../io/ImageEncode.h"
#include "../mobile_files.h"
#include "../platform_defs.h"
#include "../platform_sdl.h"
#include "../touch_mode.h"
#include "../url_open.h"
#include "UiTheme.h"
#include "ui_scale.h"

#include <Standard_Version.hxx>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifndef MATERIALIZR_VERSION
#define MATERIALIZR_VERSION "0.0.0"
#endif

namespace materializr {

namespace {

const char* kIssuesNew = "https://github.com/materializr-cad/materializr/issues/new";

// The report service (a Cloudflare Worker that files the issue). MZR_RELAY_URL
// overrides it, for pointing a build at the staging copy.
#ifndef MZ_RELAY_URL
#define MZ_RELAY_URL "https://materializr-relay.materializr-relay.workers.dev/v1/report"
#endif
std::string relayUrl() {
    if (const char* e = std::getenv("MZR_RELAY_URL")) if (*e) return e;
    return MZ_RELAY_URL;
}

bugreport::Env gatherEnv(const BugReportDialog::Hooks& hooks) {
    bugreport::Env e;
    e.version = MATERIALIZR_VERSION;
#if defined(__ANDROID__)
    e.os = "Android";
#elif defined(MZ_IOS)
    e.os = "iOS";
#elif defined(_WIN32)
    e.os = "Windows";
#elif defined(__APPLE__)
    e.os = "macOS";
#elif defined(__linux__)
    e.os = "Linux";
#else
    e.os = "Unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    e.arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    e.arch = "arm64";
#elif defined(__arm__)
    e.arch = "arm";
#elif defined(__i386__) || defined(_M_IX86)
    e.arch = "x86";
#endif
    e.occt = OCC_VERSION_STRING_EXT;
    if (const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) e.graphics = r;
    if (hooks.layoutName) e.layout = hooks.layoutName();
    e.touchMode = touchMode() ? "on" : "off";
    e.units = unitInfo(currentUnit()).suffix;
    static const char* kLang[] = {"English", "Spanish", "Portuguese", "French",
                                  "German", "Italian", "Chinese"};
    const int li = static_cast<int>(language());
    if (li >= 0 && li < static_cast<int>(sizeof(kLang) / sizeof(kLang[0]))) e.language = kLang[li];
    return e;
}

std::string reportDir() {
    std::string base = platformPrefPath();
    if (base.empty()) base = "./";
    return base + "bug-reports/";
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

} // namespace

namespace {

std::string stampStem() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return bugreport::fileStem(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                               tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

std::string humanSize(std::uintmax_t n) {
    char b[32];
    if (n < 1024) std::snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(n));
    else if (n < 1024 * 1024) std::snprintf(b, sizeof(b), "%.1f KB", n / 1024.0);
    else std::snprintf(b, sizeof(b), "%.1f MB", n / (1024.0 * 1024.0));
    return b;
}

const char* kKindName[] = {"Bug", "Feature", "Question"};
const char* kKindLabel[] = {"bug", "enhancement", "question"};

} // namespace

BugReportDialog::~BugReportDialog() { freeThumb(); }

void BugReportDialog::freeThumb() {
    if (m_thumbTex) {
        const GLuint t = m_thumbTex;
        glDeleteTextures(1, &t);
        m_thumbTex = 0;
    }
    m_thumbW = m_thumbH = 0;
}

void BugReportDialog::open() {
    m_visible = true;
    m_placeNext = true;
    m_submitted = false;
    m_error.clear();
    m_savedFiles.clear();
    m_projectSize.clear();
    m_sent = false;
    m_sending = false;
    m_sentUrl.clear();
    m_sendError.clear();
    m_stem = stampStem();
    // Two hidden frames: the first lets the Help menu that was just clicked
    // close, the second is the one that gets read back.
    m_hideFrames = 2;
}

void BugReportDialog::refreshProjectSize() {
    m_projectSize.clear();
    if (!m_hooks.saveProjectCopy) return;
    const std::string dir = reportDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string p = dir + m_stem + ".mzr";
    if (!m_hooks.saveProjectCopy(p)) { m_projectSize = "?"; return; }
    const auto sz = std::filesystem::file_size(p, ec);
    m_projectSize = ec ? "?" : humanSize(sz);
}

void BugReportDialog::captureFramebuffer() {
    const ImGuiIO& io = ImGui::GetIO();
    const int w = static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x + 0.5f);
    const int h = static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y + 0.5f);
    m_png.clear();
    m_shotW = m_shotH = 0;
    freeThumb();
    if (w <= 0 || h <= 0) return;

    std::vector<uint8_t> raw(static_cast<size_t>(w) * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, g_windowFramebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    if (glGetError() != GL_NO_ERROR) return;

    // GL rows run bottom-up; PNG wants top-down. Alpha is forced opaque (the
    // default framebuffer's alpha is whatever the last blend left behind).
    std::vector<uint8_t> flipped(raw.size());
    const size_t row = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = raw.data() + static_cast<size_t>(h - 1 - y) * row;
        uint8_t* dst = flipped.data() + static_cast<size_t>(y) * row;
        std::copy(src, src + row, dst);
        for (int x = 0; x < w; ++x) dst[x * 4 + 3] = 255;
    }
    if (!encodePng(flipped.data(), w, h, m_png)) { m_png.clear(); return; }
    m_shotW = w;
    m_shotH = h;

    // Preview texture: box-downscale by a whole factor to roughly 480 px wide.
    const int f = std::max(1, (w + 479) / 480);
    const int tw = w / f, th = h / f;
    if (tw > 0 && th > 0) {
        std::vector<uint8_t> thumb(static_cast<size_t>(tw) * th * 4);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x)
                for (int c = 0; c < 4; ++c) {
                    unsigned sum = 0;
                    for (int dy = 0; dy < f; ++dy)
                        for (int dx = 0; dx < f; ++dx)
                            sum += flipped[(static_cast<size_t>(y * f + dy) * w + (x * f + dx)) * 4 + c];
                    thumb[(static_cast<size_t>(y) * tw + x) * 4 + c] = static_cast<uint8_t>(sum / (f * f));
                }
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, thumb.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_thumbTex = tex;
        m_thumbW = tw;
        m_thumbH = th;
    }
}

void BugReportDialog::afterFrameRendered() {
    if (m_hideFrames > 0 && --m_hideFrames == 0) captureFramebuffer();
}

// Writes the chosen attachments next to each other in the report folder.
// `attachmentsMd` is the bullet list for the fallback issue body; `toSend` is
// what the relay upload carries.
bool BugReportDialog::saveAttachments(std::string& attachmentsMd, std::vector<ReportFile>& toSend) {
    m_error.clear();
    m_savedFiles.clear();
    m_dir = reportDir();
    if (m_stem.empty()) m_stem = stampStem();
    const std::string& stem = m_stem;

    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);

    if (m_includeShot && !m_png.empty()) {
        const std::string p = m_dir + stem + ".png";
        if (writeFile(p, m_png)) {
            m_savedFiles.push_back(p);
            toSend.push_back({"screenshot", p, "image/png"});
            attachmentsMd += "- " + stem + ".png (screenshot)\n";
        } else {
            m_error = tr("Couldn't save the screenshot.");
        }
    }
    if (m_includeProject && m_hooks.saveProjectCopy) {
        const std::string p = m_dir + stem + ".mzr";
        if (m_hooks.saveProjectCopy(p)) {
            m_savedFiles.push_back(p);
            toSend.push_back({"project", p, "application/octet-stream"});
            attachmentsMd += "- " + stem + ".mzr (project file)\n";
        } else {
            m_error = tr("Couldn't save the project copy.");
        }
    }
    if (m_includeLog && logcapture::available()) {
        const std::string text = logcapture::snapshot();
        const std::string p = m_dir + stem + ".log";
        if (!text.empty() && writeFile(p, std::vector<uint8_t>(text.begin(), text.end()))) {
            m_savedFiles.push_back(p);
            toSend.push_back({"log", p, "text/plain"});
            attachmentsMd += "- " + stem + ".log (log)\n";
        }
    }
    return true;
}

// Fallback path: open a pre-filled GitHub issue; the user attaches files by hand.
void BugReportDialog::submit() {
    std::string attachments;
    std::vector<ReportFile> unused;
    saveAttachments(attachments, unused);

    const int k = std::clamp(m_kind, 0, 2);
    m_lastBody = bugreport::buildBody(gatherEnv(m_hooks), m_desc, attachments, kKindName[k]);
    const std::string url = bugreport::buildIssueUrl(kIssuesNew, m_title, m_lastBody, 6000, kKindLabel[k]);
    m_submitted = true;
    if (!openUrl(url, "https://github.com/")) {
        m_error = tr("Couldn't open the browser - use \"Copy report text\" and paste it into a new issue on GitHub.");
    }
}

// Main path: post everything to the relay on a worker thread.
void BugReportDialog::startSend() {
    if (m_sending) return;
    std::string attachments;
    std::vector<ReportFile> files;
    saveAttachments(attachments, files);

    const int k = std::clamp(m_kind, 0, 2);
    std::vector<ReportField> fields = {
        {"kind", kKindName[k]},
        {"title", m_title},
        {"description", m_desc},
        {"env", bugreport::envJson(gatherEnv(m_hooks))},
    };
    m_sendError.clear();
    m_submitted = false;
    m_sending = true;
    const std::string url = relayUrl();
    m_send = std::async(std::launch::async, [url, fields = std::move(fields), files = std::move(files)]() {
        return sendReport(url, fields, files);
    });
}

// "Report another": blank form, fresh screenshot, fresh file names.
void BugReportDialog::startAnother() {
    m_title[0] = '\0';
    m_desc[0] = '\0';
    m_includeProject = false;
    m_projectSize.clear();
    m_sent = false;
    m_sentUrl.clear();
    m_sendError.clear();
    m_submitted = false;
    m_savedFiles.clear();
    m_stem = stampStem();
    m_hideFrames = 2;       // hide the dialog and retake the screenshot
}

void BugReportDialog::pollSend() {
    if (!m_sending || !m_send.valid()) return;
    if (m_send.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    ReportSendResult r = m_send.get();
    m_sending = false;
    if (r.ok) {
        m_sent = true;
        m_sentUrl = r.issueUrl;
        // Sent: the next report starts from a blank form, not this one's text.
        m_title[0] = '\0';
        m_desc[0] = '\0';
        m_includeProject = false;
        // The files were only staged for the upload; don't leave copies of the
        // user's design and log lying around once they have been sent.
        std::error_code ec;
        for (const auto& f : m_savedFiles) std::filesystem::remove(f, ec);
        m_savedFiles.clear();
        if (m_hooks.toast) m_hooks.toast(tr("Report sent. Thank you!"));
    } else {
        m_sendError = r.message;
    }
}

void BugReportDialog::render() {
    if (!m_visible || m_hideFrames > 0) return;
    pollSend();

    const ImVec2 sz = uiSz(530, 0);
    if (m_placeNext) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        m_placeNext = false;
    }
    // Fixed width; height is the reference dialog's tall form, clipped to the
    // screen. The form scrolls inside, the Close / Open buttons stay pinned.
    const float maxH = ImGui::GetIO().DisplaySize.y * 0.92f;
    ImGui::SetNextWindowSize(ImVec2(sz.x, std::min(uiSz(0, 760).y, maxH)), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin(tr("Report a problem"), &m_visible, flags)) { ImGui::End(); return; }

    const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f + 2.0f;
    ImGui::BeginChild("##bugBody", ImVec2(0, -footerH), false);

    const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    auto hint = [&](const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, dim);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };

    if (m_sent) {
        ImGui::Spacing();
        ImGui::TextColored(accentText(), "%s", tr("Report sent. Thank you!"));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", tr("It was filed as a public issue on GitHub, where you can follow the replies:"));
        ImGui::TextWrapped("%s", m_sentUrl.c_str());
        ImGui::EndChild();
        ImGui::Separator();
        const ImGuiStyle& st = ImGui::GetStyle();
        const float closeW = uiSz(80, 0).x, viewW = uiSz(130, 0).x, anotherW = uiSz(150, 0).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                             (closeW + viewW + anotherW + 2 * st.ItemSpacing.x));
        if (ImGui::Button(tr("Close"), ImVec2(closeW, 0))) m_visible = false;
        ImGui::SameLine();
        if (ImGui::Button(tr("View issue"), ImVec2(viewW, 0))) openUrl(m_sentUrl, "https://github.com/");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.48f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.56f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.40f, 0.85f, 1.0f));
        if (ImGui::Button(tr("Report another"), ImVec2(anotherW, 0))) startAnother();
        ImGui::PopStyleColor(3);
        ImGui::End();
        return;
    }

    // Type: one segmented row, the chosen kind highlighted.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Type"));
    ImGui::SameLine();
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float bw = (avail - 2 * gap) / 3.0f;
        const char* labels[3] = {tr("Bug"), tr("Feature"), tr("Question")};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            const bool on = (m_kind == i);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(labels[i], ImVec2(bw, 0))) m_kind = i;
            if (on) ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Title"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##bugTitle", tr("A short title for the problem"), m_title, sizeof(m_title));

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Description"));
    ImGui::InputTextMultiline("##bugDesc", m_desc, sizeof(m_desc), ImVec2(-1, uiSz(0, 130).y));
    if (reportSenderAvailable())
        hint(tr("Your report is filed as a public issue on GitHub, so the screenshot and log "
                "can be seen by anyone. Don't include anything private."));
    else
        hint(tr("This opens a pre-filled issue on GitHub in your browser. Nothing is sent from "
                "the app - you review it there and submit it with your GitHub account."));

    ImGui::Separator();
    if (m_hooks.saveProjectCopy) {
        if (ImGui::Checkbox(tr("Include your model (.mzr file)"), &m_includeProject)) {
            if (m_includeProject) refreshProjectSize();
        }
        if (reportSenderAvailable())
            hint(tr("Includes your design's full history so the problem can be reproduced exactly. "
                    "Stored privately for 14 days: only the Materializr maintainers can download it."));
        else
            hint(tr("Includes your design's full history so the problem can be reproduced exactly. "
                    "Files attached to a GitHub issue are public."));
        if (m_includeProject) {
            const std::string name = m_hooks.projectName ? m_hooks.projectName() : std::string();
            ImGui::Spacing();
            ImGui::Text("%s%s%s", name.c_str(), m_projectSize.empty() ? "" : ", ", m_projectSize.c_str());
        }
        ImGui::Spacing();
        hint(tr("Attaching your model helps us reproduce problems."));
        ImGui::Separator();
    }

    ImGui::TextColored(accentText(), "%s", tr("Attach"));
    ImGui::Checkbox(tr("Screenshot of the window"), &m_includeShot);
    if (m_includeShot) {
        if (m_thumbTex) {
            const float maxW = std::min(ImGui::GetContentRegionAvail().x, uiSz(220, 0).x);
            const float aw = maxW, ah = maxW * static_cast<float>(m_thumbH) / std::max(1, m_thumbW);
            ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(m_thumbTex)), ImVec2(aw, ah));
        } else {
            hint(tr("(no screenshot captured)"));
        }
        if (ImGui::Button(tr("Retake screenshot"))) m_hideFrames = 2;   // hide, then read back
    }
    ImGui::Spacing();
    if (logcapture::available()) {
        ImGui::Checkbox(tr("Log"), &m_includeLog);
        if (m_includeLog)
            hint(tr("The most recent diagnostic output. It can include file names and folder paths."));
    }

    if (m_sending) {
        ImGui::Spacing();
        hint(tr("Sending..."));
    }
    if (!m_sendError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", m_sendError.c_str());
    }
    if (reportSenderAvailable() && !m_submitted) {
        ImGui::Spacing();
        if (ImGui::SmallButton(tr("Open a pre-filled GitHub issue instead"))) submit();
    }

    if (m_submitted) {
        ImGui::Separator();
        if (!m_error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", m_error.c_str());
        if (!m_savedFiles.empty()) {
            ImGui::TextWrapped("%s", tr("Drag these files into the issue's comment box:"));
            for (const auto& f : m_savedFiles) {
                ImGui::BulletText("%s", std::filesystem::path(f).filename().string().c_str());
#if defined(MZ_MOBILE)
                ImGui::SameLine();
                ImGui::PushID(f.c_str());
                if (ImGui::SmallButton(tr("Share..."))) {
                    const auto ext = std::filesystem::path(f).extension().string();
                    mobileShareFile(f, ext == ".png" ? "image/png"
                                       : ext == ".log" ? "text/plain" : "application/octet-stream");
                }
                ImGui::PopID();
#endif
            }
#if !defined(MZ_MOBILE)
            ImGui::TextWrapped("%s", m_dir.c_str());
            if (ImGui::SmallButton(tr("Open folder"))) {
                std::string err;
                platformOpenUrl("file://" + m_dir, &err);
            }
            ImGui::SameLine();
#endif
        }
        if (ImGui::SmallButton(tr("Copy report text"))) {
            ImGui::SetClipboardText(m_lastBody.c_str());
            if (m_hooks.toast) m_hooks.toast(tr("Report text copied"));
        }
    }

    ImGui::EndChild();

    // Footer: Close, then the primary action, right-aligned like the reference.
    ImGui::Separator();
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float closeW = uiSz(80, 0).x, sendW = uiSz(150, 0).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                             (closeW + sendW + st.ItemSpacing.x));
        if (ImGui::Button(tr("Close"), ImVec2(closeW, 0))) m_visible = false;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.48f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.56f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.40f, 0.85f, 1.0f));
        if (reportSenderAvailable()) {
            ImGui::BeginDisabled(m_sending);
            if (ImGui::Button(m_sending ? tr("Sending...") : tr("Send"), ImVec2(sendW, 0))) startSend();
            ImGui::EndDisabled();
        } else if (ImGui::Button(tr("Open on GitHub"), ImVec2(sendW, 0))) {
            submit();
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::End();
}

} // namespace materializr
