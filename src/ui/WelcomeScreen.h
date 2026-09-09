#pragma once

namespace materializr {

// Launch welcome screen: a centred modal shown once per start (unless the
// user is a Supporter) with the app version and a donation ask. Dismissible
// immediately - it recurs every launch, never blocks.
class WelcomeScreen {
public:
    // What the user did this frame; the caller owns the consequences (persisting
    // the Supporter flag lives with the rest of the settings plumbing).
    enum class Action { None, MarkSupporter };

    void setVisible(bool vis);
    bool isVisible() const;
    Action render();

private:
    bool m_visible = false;
};

} // namespace materializr
