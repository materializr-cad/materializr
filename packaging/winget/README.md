# winget (Windows Package Manager) manifests

Reference copies of the [winget](https://learn.microsoft.com/windows/package-manager/)
manifests for Materializr. The canonical home is Microsoft's
[`winget-pkgs`](https://github.com/microsoft/winget-pkgs) repo - these are kept
here so the packaging steps live with the source (same idea as the F-Droid
recipe in `metadata/`).

**Why bother:** the NSIS installer is unsigned, so Windows SmartScreen warns on
download. A code-signing certificate (the fast fix) costs money. winget is the
free path: once the package is in the catalog, `winget install Materializr.Materializr`
installs without the SmartScreen prompt, and the installer slowly accrues
SmartScreen reputation from the download volume.

## Files

| File | Manifest type |
|------|---------------|
| `Materializr.Materializr.yaml`               | version |
| `Materializr.Materializr.installer.yaml`     | installer (URL + SHA256 + install behavior) |
| `Materializr.Materializr.locale.en-US.yaml`  | default locale (name, description, license) |

## Submitting a new version

1. **Get the installer SHA256** of the released `Materializr-Setup.exe`:
   ```sh
   curl -sL -o Setup.exe https://github.com/materializr-cad/materializr/releases/download/vX.Y.Z/Materializr-Setup.exe
   sha256sum Setup.exe        # uppercase it for the manifest
   ```
2. **Bump** `PackageVersion`, the `InstallerUrl` tag, `InstallerSha256`, and the
   `AppsAndFeaturesEntries.DisplayVersion` (matches the NSIS `/DAPPVERSION`, which
   is the git tag, e.g. `vX.Y.Z`) in all three files.
3. **Validate** against the schemas (CI-free, runs anywhere with Python):
   ```sh
   python3 -c "import yaml,json,urllib.request,jsonschema as J; \
     [J.validate(yaml.safe_load(open(f)), json.load(urllib.request.urlopen(u))) \
      for f,u in {...}.items()]"
   ```
   On Windows you can instead run `winget validate <dir>`.
4. **Submit** to `microsoft/winget-pkgs` under
   `manifests/m/Materializr/Materializr/X.Y.Z/`. Easiest is
   [`wingetcreate`](https://github.com/microsoft/winget-create) on Windows:
   ```pwsh
   wingetcreate update Materializr.Materializr --version X.Y.Z `
     --urls https://github.com/materializr-cad/materializr/releases/download/vX.Y.Z/Materializr-Setup.exe `
     --submit
   ```
   …or fork the repo, copy these files to that path, and open a PR by hand.

## Notes / gotchas

- **Identifier:** `Materializr.Materializr` (`Publisher.Package`, both the brand -
  the accepted form for a single-product publisher).
- **Silent install:** `nullsoft` type implies `/S`; the NSIS script installs
  machine-wide to `%ProgramFiles%\Materializr` and needs elevation.
- **ARP DisplayVersion has a leading `v`** because the workflow passes the git
  tag (`${github.ref_name}` = `vX.Y.Z`) as `APPVERSION`. The installer manifest
  declares that exact value so winget's post-install detection matches. (If you
  later strip the `v` in `windows.yml`, drop it here too.)
- Microsoft's validation pipeline installs the package in a sandbox VM, so the
  silent install + Add/Remove-Programs registration must succeed - both are
  handled by `packaging/windows/installer.nsi`.
