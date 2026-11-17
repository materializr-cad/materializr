# Materializr Privacy Policy

**Effective date:** July 5, 2026 (updated November 17, 2026 for the optional Report a Bug feature)

Materializr is a free, open-source CAD application. The short version: **it
does not collect, store, or share any personal data on its own.** There are no
accounts, no analytics, no advertising, no tracking, and no third-party data
SDKs. Your models and project files stay on your device. The one exception is
the optional **Report a Bug** feature (see below), which sends something only
when you press Send.

## What the app does on your device

- **Your work stays local.** Projects, settings, and exports are written only
  to storage you choose on your own device. Nothing is uploaded unless you
  choose to send a bug report.
- **Settings** (theme, layout, supporter status, and similar preferences) are
  stored in a local configuration file on your device.

## Network access

- **Desktop (Windows / macOS / Linux):** the only network feature is an
  optional update check that contacts the GitHub API
  (`api.github.com`) to compare your version against the latest release. This
  request carries no personal information beyond what any web request
  technically includes (such as your IP address, which we never see - the
  request goes to GitHub, governed by
  [GitHub's privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-privacy-statement)).
  It can be turned off in Settings, and is off in safe mode.
- **Android:** the app asks for the Internet permission for one purpose only,
  the optional **Report a Bug** feature below. It makes no other network
  requests. The update checker is not included in mobile builds.
- **iOS:** the app makes no network requests of its own. Report a Bug opens a
  pre-filled GitHub page in your browser instead.

## Report a Bug (optional)

**Help > Report a Bug** lets you send a bug report, feature request, or
question to the developers. Nothing is sent until you press **Send**, and
nothing is sent automatically or in the background.

What a report contains:

- The type, title, and description you write.
- Your app version, operating system and CPU type, OpenCASCADE version, graphics
  card name, interface layout, touch mode, units, and language.
- A screenshot of the app window, if "Screenshot of the window" is ticked.
  It shows whatever is visible in the app at that moment.
- The most recent diagnostic output of the app, if "Log" is ticked. It can
  include file names and folder paths; the report service replaces the account
  name in home-folder paths before storing it.
- A copy of your project file, only if you tick "Include your model".

Where it goes: the report is sent to a small service run by the project on
Cloudflare, which files it as a **public issue** on the project's GitHub
repository. The issue text, screenshot, and log can be seen by anyone, so do
not include anything private. A project file is **not** public: it is stored
privately and only the project maintainers can download it. The screenshot,
log, and project file are deleted from the report service after 14 days; the
GitHub issue itself stays until the maintainers remove it.

The service sees your IP address like any web server does. It is used only to
limit how many reports one address can send, as a salted hash that is not kept
beyond the rate-limit window (about a day). Cloudflare and GitHub handle their
own request data under their own privacy policies
([Cloudflare](https://www.cloudflare.com/privacypolicy/),
[GitHub](https://docs.github.com/en/site-policy/privacy-policies/github-privacy-statement)).

If you would rather not use the service, close the dialog; the app works
exactly the same without it.

## Purchases and donations

- **App Store (iOS):** the optional Supporter purchase is processed entirely
  by Apple through StoreKit. We never see your name, payment details, or
  Apple ID; Apple's handling of that data is governed by
  [Apple's privacy policy](https://www.apple.com/legal/privacy/). The app
  stores only a local yes/no flag that a purchase was made or restored.
- **Google Play (Android):** the optional Supporter purchase is processed
  entirely by Google through Play Billing. We never see your name, payment
  details, or Google account; Google's handling of that data is governed by
  [Google's privacy policy](https://policies.google.com/privacy). The app
  stores only a local yes/no flag that a purchase was made or restored.
- **Donation links** (such as Buy Me a Coffee, opened from the app in your
  browser) take you to an external site with its own privacy policy; the app
  sends no data to it.

## Platform-provided statistics

Apple and Google may provide the publisher with aggregated, anonymized
statistics (such as install counts or opt-in crash reports) as described in
their own policies. Materializr itself contains no crash-reporting or
analytics code.

## Children

Materializr does not collect data from anyone, including children, except what
a person chooses to put in a bug report as described above.

## Changes

If a future version ever changes any of the above (for example, adding an
opt-in feature that uses the network), this policy will be updated in the
repository before that version is released.

## Contact

Materializr is developed in the open at
<https://github.com/materializr-cad/materializr>. Store releases are
published by Ravenhold LLC. Questions or concerns: open an issue on the
repository, or ask on the project
[Discord](https://discord.gg/BRjzbMGZvE).
