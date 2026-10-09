# Packages and release validation

The authoritative version is `project(VERSION ...)` in CMakeLists.txt. Binaries, CPack metadata and release tag checks derive from it. Packages are x86-64 only. Native build environments are Debian 13 and Fedora 44. The release workflow also supports manual dispatch for pre-release verification without publishing.

Each native job builds all targets with warnings as errors, runs CTest including actual Xvfb GTK browse/search, builds its CPack format, checks package metadata/dependencies/files/version, installs and reinstalls it, runs installed daemon/CLI/GTK integration, and removes it. The workflow first installs the published v0.5.0 package, creates a real indexed fixture and history snapshot, then upgrades to the new package and checks preserved entry/root/snapshot identities. Reinstall also exercises replacement at the current version. Neither package has install scripts that start or enable a daemon. The user service is installed under `/usr/lib/systemd/user`.

Only after both native jobs succeed does a matching SemVer tag publish both packages and SHA256SUMS through GitHub Actions. Manual uploading is not the normal path. A workflow file alone does not establish successful package validation; inspect the actual run and assets.

The examples below install the last published v0.5.1 packages, not a v0.5.2
artifact. Build v0.5.2 packages locally with CPack if needed; no release assets
are claimed for this source patch.

```sh
sudo apt install ./treehound_0.5.1_amd64.deb
sudo dnf install ./treehound-0.5.1-1.x86_64.rpm
# Optional, explicitly chosen persistence:
systemctl --user enable --now treehound.service
# Stop/disable before uninstalling:
systemctl --user disable --now treehound.service
sudo apt remove treehound        # Debian
sudo dnf remove treehound        # Fedora
```

Package removal never deletes the user's index or configuration. Upgrades preserve SQLite data and migrations run in the daemon. GTK is a system dependency, never bundled. Source builds can generate packages with `cpack -G DEB` or `cpack -G RPM` from the build directory; validate install/removal only in disposable containers.

The published [v0.5.1 release](https://github.com/blindicide/treehound/releases/tag/v0.5.1)
was independently downloaded and checked against `SHA256SUMS`. DEB control
contents contain only metadata and file checksums; RPM scriptlets are empty.
Both formats contain the GUI, GTK-independent daemon, user service, desktop
entry, icon, MIT license and documentation. Native install/upgrade/removal
evidence is in [the successful release run](https://github.com/blindicide/treehound/actions/runs/37855797617);
the upgrade checks preserve root, entry and snapshot identities from v0.5.0.

To verify downloads before installation, keep both packages and `SHA256SUMS`
in one directory and run `sha256sum -c SHA256SUMS`. An OK result for both files
confirms they match that release's checksum manifest.
