#pragma once
#include <functional>

// Tiny global bridge (same spirit as touch_mode.h) so PLUGINS can read and
// switch the interface layout without a PluginContext round-trip. Application
// binds the two callbacks at startup; until then the getter reports Classic
// and the setter is a no-op. Index values mirror UiLayout in io/Settings.h:
// 0 = Classic, 1 = Modern, 2 = im-touch.
//
// First consumer: the Getting Started tour's layout picker, which switches
// the layout LIVE behind its modal as the user taps each option (the whole
// app re-renders in the candidate layout - a real preview, not a screenshot).
namespace materializr {

int  currentUiLayoutIndex();
void requestUiLayout(int index);   // switches immediately and persists

void bindUiLayoutBridge(std::function<int()> get,
                        std::function<void(int)> set);

// Same bridge, for the UI LANGUAGE. Index mirrors materializr::Lang (0 =
// English). The getter reports -1 when the user has never chosen one, which is
// what tells the setup wizard to ask. Setting switches live AND persists.
//
// Second consumer of this pattern: the tour's language page, which has to come
// before everything else -- a user who cannot read the current language cannot
// read the page asking which layout they want either.
int  currentLanguageIndex();
void requestLanguage(int index);

void bindLanguageBridge(std::function<int()> get,
                        std::function<void(int)> set);

} // namespace materializr
