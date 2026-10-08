# Packages and release validation

The authoritative version is `project(VERSION ...)` in CMakeLists.txt. Binaries, CPack metadata and release tag checks derive from it. Packages are x86-64 only. Native build environments are Debian 13 and Fedora 44. The release workflow also supports manual dispatch for pre-release verification without publishing.

Each native job builds all targets with warnings as errors, runs CTest including actual Xvfb GTK browse/search, builds its CPack format, checks package metadata/dependencies/files/version, installs and reinstalls it, runs installed daemon/CLI/GTK integration, and removes it. Reinstall exercises the replacement path at the current version; cross-version upgrades need separate validation when a previous release exists. Neither package has install scripts that start or enable a daemon. The user service is installed under `/usr/lib/systemd/user`.

Only after both native jobs succeed does a matching SemVer tag publish both packages and SHA256SUMS through GitHub Actions. Manual uploading is not the normal path. A workflow file alone does not establish successful package validation; inspect the actual run and assets.

```sh
sudo apt install ./treehound_0.1.0_amd64.deb
sudo dnf install ./treehound-0.1.0-1.x86_64.rpm
# Optional, explicitly chosen persistence:
systemctl --user enable --now treehound.service
# Stop/disable before uninstalling:
systemctl --user disable --now treehound.service
sudo apt remove treehound        # Debian
sudo dnf remove treehound        # Fedora
```

Package removal never deletes the user's index or configuration. Upgrades preserve SQLite data and migrations run in the daemon. GTK is a system dependency, never bundled. Source builds can generate packages with `cpack -G DEB` or `cpack -G RPM` from the build directory; validate install/removal only in disposable containers.
