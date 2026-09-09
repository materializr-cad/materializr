#pragma once
#include "platform_defs.h"

#include <string>

// iOS StoreKit tip jar (implemented in ios_storekit.mm). One non-consumable
// "Supporter" product (com.materializr.cad.supporter): buying or restoring it
// permanently silences the Welcome screen. Safe to include everywhere: on
// non-iOS platforms the calls are inline no-ops so callers need no guards.
//
// Flow: iosStoreInit() attaches the payment-queue observer at launch (Apple
// requires this so purchases interrupted in a previous run are delivered).
// The Welcome screen drives iosStoreBuySupporter()/iosStoreRestore() and polls
// iosStorePhase()/iosStoreMessage() for button state and error text; the main
// loop polls iosStoreConsumeEntitled() once per frame and persists the
// Supporter flag when it fires. StoreKit callbacks may arrive off the SDL
// thread, so all state crosses via atomics / a mutex-guarded string.
namespace materializr {

enum class TipPhase {
    Idle,     // nothing in flight; message (if any) is a final status
    Working,  // product fetch / payment sheet / restore in progress
    Failed,   // last attempt failed; message says why
};

#if defined(MZ_IOS)

void iosStoreInit();            // attach the transaction observer - once, at launch
void iosStoreBuySupporter();    // fetch the Supporter product and start a purchase
void iosStoreRestore();         // restore a previous Supporter purchase
TipPhase iosStorePhase();
std::string iosStoreMessage();  // status / error line for the Welcome screen ("" = none)
bool iosStoreConsumeEntitled(); // one-shot: a purchase or restore granted Supporter

#else

inline void iosStoreInit() {}
inline void iosStoreBuySupporter() {}
inline void iosStoreRestore() {}
inline TipPhase iosStorePhase() { return TipPhase::Idle; }
inline std::string iosStoreMessage() { return {}; }
inline bool iosStoreConsumeEntitled() { return false; }

#endif

} // namespace materializr
