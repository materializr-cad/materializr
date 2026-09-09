// StoreKit tip jar - see ios_storekit.h for the flow. Uses the classic
// StoreKit 1 API (SKPaymentQueue / SKProductsRequest): the modern StoreKit 2
// surface is Swift-only and this target is pure C++/Objective-C++. SK1 is
// deprecated but fully functional, and the whole integration is one product.
#include "ios_storekit.h"

#if defined(MZ_IOS)

#import <Foundation/Foundation.h>
#import <StoreKit/StoreKit.h>

#include <atomic>
#include <mutex>
#include <string>

static NSString* const kSupporterProductId = @"com.materializr.cad.supporter";

namespace {

std::atomic<int>  g_phase{static_cast<int>(materializr::TipPhase::Idle)};
// One-shot entitlement flag, consumed by the main loop (iosStoreConsumeEntitled).
std::atomic<bool> g_entitled{false};
// Sticky twin of g_entitled for the restore-finished callback: the main loop
// may consume g_entitled before that callback runs, and "restored nothing"
// must not be reported when a Supporter transaction did arrive.
std::atomic<bool> g_everEntitled{false};
std::mutex        g_msgMutex;
std::string       g_message;

void setPhase(materializr::TipPhase p) { g_phase = static_cast<int>(p); }

void setMessage(NSString* s) {
    std::lock_guard<std::mutex> lock(g_msgMutex);
    g_message = s ? [s UTF8String] : "";
}

bool userCancelled(NSError* e) {
    return e && [e.domain isEqualToString:SKErrorDomain] &&
           e.code == SKErrorPaymentCancelled;
}

} // namespace

@interface MZStoreObserver
    : NSObject <SKPaymentTransactionObserver, SKProductsRequestDelegate>
// Keeps the in-flight product request alive; under ARC a local would be
// deallocated before its delegate callbacks fire.
@property(nonatomic, strong) SKProductsRequest* inflightRequest;
@end

@implementation MZStoreObserver

// ── Product fetch (step 1 of a purchase) ─────────────────────────────────────

- (void)productsRequest:(SKProductsRequest*)request
     didReceiveResponse:(SKProductsResponse*)response {
    self.inflightRequest = nil;
    SKProduct* product = nil;
    for (SKProduct* p in response.products) {
        if ([p.productIdentifier isEqualToString:kSupporterProductId]) {
            product = p;
            break;
        }
    }
    if (!product) {
        // Product not configured / not yet approved in App Store Connect.
        setMessage(@"Support option is not available right now.");
        setPhase(materializr::TipPhase::Failed);
        return;
    }
    [[SKPaymentQueue defaultQueue] addPayment:[SKPayment paymentWithProduct:product]];
    // Still Working - the payment sheet takes over; a transaction callback ends it.
}

- (void)request:(SKRequest*)request didFailWithError:(NSError*)error {
    self.inflightRequest = nil;
    setMessage(error.localizedDescription.length
                   ? error.localizedDescription
                   : @"Could not reach the App Store.");
    setPhase(materializr::TipPhase::Failed);
}

// ── Transaction updates (purchase, restore, and launch-time redelivery) ─────

- (void)paymentQueue:(SKPaymentQueue*)queue
 updatedTransactions:(NSArray<SKPaymentTransaction*>*)transactions {
    for (SKPaymentTransaction* t in transactions) {
        switch (t.transactionState) {
        case SKPaymentTransactionStatePurchasing:
            break; // sheet up; stay Working
        case SKPaymentTransactionStateDeferred:
            // Ask to Buy: approval may take days - unblock the UI now.
            setMessage(@"Purchase is awaiting approval.");
            setPhase(materializr::TipPhase::Idle);
            break;
        case SKPaymentTransactionStatePurchased:
        case SKPaymentTransactionStateRestored:
            if ([t.payment.productIdentifier isEqualToString:kSupporterProductId]) {
                g_entitled = true;
                g_everEntitled = true;
            }
            [queue finishTransaction:t];
            setMessage(nil);
            setPhase(materializr::TipPhase::Idle);
            break;
        case SKPaymentTransactionStateFailed:
            if (userCancelled(t.error)) {
                setMessage(nil); // user backed out - not an error
                setPhase(materializr::TipPhase::Idle);
            } else {
                setMessage(t.error.localizedDescription.length
                               ? t.error.localizedDescription
                               : @"Purchase failed.");
                setPhase(materializr::TipPhase::Failed);
            }
            [queue finishTransaction:t];
            break;
        }
    }
}

- (void)paymentQueueRestoreCompletedTransactionsFinished:(SKPaymentQueue*)queue {
    if (g_everEntitled) {
        setPhase(materializr::TipPhase::Idle);
    } else {
        setMessage(@"No previous purchase found for this Apple ID.");
        setPhase(materializr::TipPhase::Failed);
    }
}

- (void)paymentQueue:(SKPaymentQueue*)queue
    restoreCompletedTransactionsFailedWithError:(NSError*)error {
    if (userCancelled(error)) {
        setMessage(nil);
        setPhase(materializr::TipPhase::Idle);
    } else {
        setMessage(error.localizedDescription.length
                       ? error.localizedDescription
                       : @"Restore failed.");
        setPhase(materializr::TipPhase::Failed);
    }
}

@end

static MZStoreObserver* g_observer = nil;

namespace materializr {

void iosStoreInit() {
    if (g_observer) return;
    g_observer = [MZStoreObserver new];
    [[SKPaymentQueue defaultQueue] addTransactionObserver:g_observer];
}

void iosStoreBuySupporter() {
    iosStoreInit();
    if (iosStorePhase() == TipPhase::Working) return;
    if (![SKPaymentQueue canMakePayments]) {
        setMessage(@"In-app purchases are disabled on this device.");
        setPhase(TipPhase::Failed);
        return;
    }
    setMessage(nil);
    setPhase(TipPhase::Working);
    SKProductsRequest* req = [[SKProductsRequest alloc]
        initWithProductIdentifiers:[NSSet setWithObject:kSupporterProductId]];
    req.delegate = g_observer;
    g_observer.inflightRequest = req;
    [req start];
}

void iosStoreRestore() {
    iosStoreInit();
    if (iosStorePhase() == TipPhase::Working) return;
    setMessage(nil);
    setPhase(TipPhase::Working);
    [[SKPaymentQueue defaultQueue] restoreCompletedTransactions];
}

TipPhase iosStorePhase() { return static_cast<TipPhase>(g_phase.load()); }

std::string iosStoreMessage() {
    std::lock_guard<std::mutex> lock(g_msgMutex);
    return g_message;
}

bool iosStoreConsumeEntitled() { return g_entitled.exchange(false); }

} // namespace materializr

#endif // MZ_IOS
