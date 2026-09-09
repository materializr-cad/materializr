# Release signing (Android APK)

The release APK must be signed with a private keystore. **The keystore and its
password live OUTSIDE this repository and are never committed.** `.gitignore`
also blocks `*.jks`, `*.keystore`, and `keystore.properties` as a safety net.

> ⚠️ **Back up the keystore and remember its password.** If you lose either, you
> can never publish an update under the same signing identity.

## One-time: create the keystore

```bash
keytool -genkeypair -v \
  -keystore ~/materializr-release.jks \
  -storetype PKCS12 \
  -alias materializr \
  -keyalg RSA -keysize 4096 -validity 10000
```

It prompts for the keystore password and a few certificate identity fields.
PKCS12 uses a single password for both the store and the key. Verify with:

```bash
keytool -list -v -keystore ~/materializr-release.jks
```

## Build a signed release APK

`android/app/build.gradle` reads the signing config from environment variables
(preferred - nothing stored at rest) or, failing that, a gitignored
`android/keystore.properties`. The key alias defaults to `materializr`.

### Option 1 - environment variables (recommended)

```bash
cd android
read -s -p "Keystore password: " MZRPW; echo
MATERIALIZR_KEYSTORE=~/materializr-release.jks \
MATERIALIZR_STORE_PASSWORD="$MZRPW" \
MATERIALIZR_KEY_ALIAS=materializr \
./gradlew assembleRelease
unset MZRPW
# → android/app/build/outputs/apk/release/app-release.apk
```

Available env vars: `MATERIALIZR_KEYSTORE`, `MATERIALIZR_STORE_PASSWORD`,
`MATERIALIZR_KEY_ALIAS` (default `materializr`), `MATERIALIZR_KEY_PASSWORD`
(defaults to the store password).

### Option 2 - gitignored properties file

Create `android/keystore.properties` (already gitignored):

```properties
storeFile=/home/<you>/materializr-release.jks
storePassword=********
keyAlias=materializr
keyPassword=********
```

Then `cd android && ./gradlew assembleRelease`.

When neither env vars nor the properties file are present (CI, fresh clone,
debug work) release builds go **unsigned**; `assembleDebug` is never affected.
