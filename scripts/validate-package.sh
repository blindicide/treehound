#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Only run in the disposable native package-build container, never on the host.
set -euo pipefail
format=$1
package=$2
version=$3
previous=${4:-}
if [[ -n "$previous" ]]; then
  case "$format" in
    DEB) dpkg -i "$previous" ;;
    RPM) rpm -Uvh "$previous" ;;
  esac
  test "$(treehound --version)" = "treehound 0.4.0"
  python3 tests/integration/package_upgrade.py prepare /usr/bin/treehoundd build-package-upgrade
fi
case "$format" in
  DEB)
    test "$(dpkg-deb -f "$package" Package)" = treehound
    test "$(dpkg-deb -f "$package" Version)" = "$version"
    test "$(dpkg-deb -f "$package" Architecture)" = amd64
    dpkg-deb -f "$package" Depends | grep 'libgtk-4'
    dpkg -i "$package"
    dpkg -i "$package" # reinstall/upgrade path, without auto-start
    ;;
  RPM)
    test "$(rpm -qp --queryformat '%{NAME}' "$package")" = treehound
    test "$(rpm -qp --queryformat '%{VERSION}' "$package")" = "$version"
    test "$(rpm -qp --queryformat '%{ARCH}' "$package")" = x86_64
    test "$(rpm -qp --queryformat '%{LICENSE}' "$package")" = MIT
    rpm -qpR "$package" | grep gtk4
    rpm -Uvh "$package"
    rpm -Uvh --replacepkgs "$package"
    ;;
  *) exit 2 ;;
esac
if [[ -n "$previous" ]]; then
  python3 tests/integration/package_upgrade.py check /usr/bin/treehoundd build-package-upgrade
fi
test "$(treehound --version)" = "treehound $version"
test "$(treehoundd --version)" = "treehoundd $version"
test -f /usr/share/applications/treehound.desktop
test -f /usr/share/icons/hicolor/scalable/apps/treehound.svg
test -f /usr/lib/systemd/user/treehound.service
test -f /usr/share/doc/treehound/LICENSE
grep -Fx 'Exec=treehound' /usr/share/applications/treehound.desktop
! ldd /usr/bin/treehoundd | grep -E 'libgtk|libgdk'
! pgrep -x treehoundd
! test -e /etc/systemd/system/treehound.service
python3 tests/integration/daemon.py /usr/bin/treehoundd /usr/bin/treehound
python3 tests/integration/gui.py /usr/bin/treehoundd /usr/bin/treehound
case "$format" in
  DEB) dpkg -r treehound ;;
  RPM) rpm -e treehound ;;
esac
! test -e /usr/bin/treehound
! test -e /usr/bin/treehoundd
! test -e /usr/lib/systemd/user/treehound.service
