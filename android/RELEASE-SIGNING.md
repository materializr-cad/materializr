# Release signing (Android)

The GitHub-download APK should be signed with a stable release key, not the
throwaway debug key. This is a **local, free** step - it needs no Google account,
no $25 fee, and no file-manager/scoped-storage changes (those are Play-Store-only
concerns). F-Droid, if/when we go there, signs with its own key, so this is for
the GitHub APK (and later a Play upload key).

## 1. Generate the keystore (run this yourself - you own the password)

```sh
~/Android/jdk/bin/keytool -genkeypair -v \
  -keystore ~/materializr-release.jks \
  -alias materializr \
  -keyalg RSA -keysize 4096 -validity 10000
```

It prompts for a keystore password, a key password, and your name/org details.

> **BACK UP `~/materializr-release.jks` AND THE PASSWORDS.** Store them somewhere
> safe and redundant (password manager + offline copy). If you ever push this app
> to Play without Play App Signing and then lose this key, you can never update
> the app again. Even for GitHub-only distribution, a new key = a different
> signature, so existing users must uninstall before they can update.

## 2 + 3. Build a signed APK

`build.gradle` takes the key + password from **environment variables first**,
then from `android/keystore.properties`. A PKCS12 keystore (keytool's modern
default) uses **one password** for both the store and the key, so you only set
one. Pick whichever route you like:

### Route A - env vars (recommended; password never written to disk)

```sh
read -rs -p "Keystore password: " KSPW; echo
cd android
MATERIALIZR_KEYSTORE="$HOME/materializr-release.jks" \
MATERIALIZR_STORE_PASSWORD="$KSPW" \
JAVA_HOME=~/Android/jdk ANDROID_HOME=~/Android/Sdk \
  ./gradlew assembleRelease
unset KSPW
```

`read -rs` doesn't echo and passing `$KSPW` (not the literal) keeps the password
out of your shell history. It lives only in the build process's environment for
that one run.

### Route B - keystore.properties file (so the password is stored once)

```sh
cp android/keystore.properties.example android/keystore.properties
# edit it: storeFile=~/materializr-release.jks + your password in BOTH
# password fields (same value). The file + *.jks are gitignored.
cd android && JAVA_HOME=~/Android/jdk ANDROID_HOME=~/Android/Sdk ./gradlew assembleRelease
```

Either way the result is `app/build/outputs/apk/release/app-release.apk`
(properly signed). With neither configured, the release build is left unsigned.

Verify the signature:

```sh
~/Android/Sdk/build-tools/34*/apksigner verify --print-certs \
  app/build/outputs/apk/release/app-release.apk
```

Attach `app-release.apk` (renamed to `Materializr-<version>.apk`) to the GitHub
release instead of the debug build.

## Later: Google Play (only if/when you go there)

- **Play App Signing** (recommended): Google holds the real signing key; you
  upload with an *upload key* (a keystore generated exactly as above). Losing the
  upload key is recoverable via Google.
- **$25** one-time Play Developer registration.
- **Drop `MANAGE_EXTERNAL_STORAGE`** (All-Files-Access) for SAF/scoped storage -
  Play restricts All-Files-Access to specific app categories and would likely
  reject a CAD app. This is **independent of signing** and not needed for GitHub
  or F-Droid.
