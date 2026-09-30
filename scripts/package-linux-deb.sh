#!/usr/bin/env bash
# This file is part of Go.dot — https://github.com/pob31/go.dot
#
# Copyright (C) 2026 Pierre-Olivier Boulant
#
# Go.dot is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. Go.dot is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
# (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes the Linux package (author, 2026-09-30: "a proper Linux package"): a .deb
# that installs Go.dot for everybody, with the .wfg file type, the application
# menu's entry and the icons - what the Inno installer is on Windows. The Linux
# half of .github/workflows/release.yml, runnable on any Debian or Ubuntu.
#
#     scripts/package-linux-deb.sh <staging-folder> <version> <out.deb>
#
# The staging folder is what `cmake --install --component wfg` laid down - the
# tarball's contents - and <version> the workflow's (0.1.0, 0.1.0-alpha.1,
# 0.1.0-dev.<sha>).
#
# WHERE IT GOES:
#   /opt/go.dot/                          the staging folder, as it is
#   /usr/bin/go.dot                       a link to /opt/go.dot/go.dot.sh
#   /usr/share/applications/go.dot.desktop
#   /usr/share/mime/packages/go.dot.xml   *.wfg is application/x-go.dot-show
#   /usr/share/icons/hicolor/NxN/{apps,mimetypes}/   the app and the .wfg page
#
# ONE FOLDER IN /opt, not bin/ lib/ share/: wfg finds console/ and the empty
# show beside itself, as it does in the tarball, and the package stays the
# tarball plus what tells the desktop about it. go.dot.sh copies the empty show
# out to the user's own folder, since /opt is not theirs to save in.
#
# NO MAINTAINER SCRIPTS. The desktop, MIME and icon databases are refreshed by
# the triggers their own packages (desktop-file-utils, shared-mime-info,
# hicolor-icon-theme) put on those folders, on install and on removal alike.
#
# THE SAME FILE TYPE AND ENTRY `wfg associate` writes for one user from a
# tarball (src/wfg/engine/app/Associate.cpp) - keep the two saying the same.
#
# THE VERSION's "-" becomes "~" (0.1.0~dev.<sha>), because a Debian version
# reads the part after a hyphen as the packaging revision, and a "~" sorts
# before the release it leads to, as a pre-release should.

set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <staging-folder> <version> <out.deb>" >&2
    exit 2
fi

stage="$(cd "$1" && pwd)"
version="$2"
out="$3"

for needed in wfg go.dot.sh console/index.html Untitled; do
    if [ ! -e "$stage/$needed" ]; then
        echo "package-linux-deb: $stage has no $needed; is it a cmake --install of the wfg component?" >&2
        exit 1
    fi
done

root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT
chmod 755 "$root"

#  --- the program, as the tarball has it --------------------------------------
install -d -m 755 "$root/opt/go.dot" "$root/usr/bin"
cp -a "$stage/." "$root/opt/go.dot/"
ln -s ../../opt/go.dot/go.dot.sh "$root/usr/bin/go.dot"

#  --- what tells the desktop -------------------------------------------------
install -d -m 755 "$root/usr/share/applications" "$root/usr/share/mime/packages"

cat > "$root/usr/share/mime/packages/go.dot.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!-- Installed by the go.dot package. -->
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-go.dot-show">
    <comment>Go.dot show</comment>
    <icon name="application-x-go.dot-show"/>
    <glob pattern="*.wfg"/>
  </mime-type>
</mime-info>
EOF

cat > "$root/usr/share/applications/go.dot.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Go.dot
GenericName=Show control
Comment=Cues, sound and control for live shows
Exec=/opt/go.dot/go.dot.sh %f
Icon=go.dot
Terminal=false
Categories=AudioVideo;Audio;
MimeType=application/x-go.dot-show;
StartupWMClass=Go.dot
EOF

for png in "$stage"/icons/go.dot-[0-9]*.png; do
    size="$(basename "$png" .png)"; size="${size#go.dot-}"
    install -D -m 644 "$png" "$root/usr/share/icons/hicolor/${size}x${size}/apps/go.dot.png"
done

for png in "$stage"/icons/go.dot-document-*.png; do
    size="$(basename "$png" .png)"; size="${size#go.dot-document-}"
    install -D -m 644 "$png" "$root/usr/share/icons/hicolor/${size}x${size}/mimetypes/application-x-go.dot-show.png"
done

#  --- the package's own description ------------------------------------------
# Depends: what wfg links, and the X11 libraries JUCE opens by name when the
# window starts. Recommends: PipeWire's JACK, the way to every channel of a
# multichannel interface (README.txt), which go.dot.sh uses when it is there.
install -d -m 755 "$root/DEBIAN"
cat > "$root/DEBIAN/control" <<EOF
Package: go.dot
Version: ${version//-/\~}
Architecture: amd64
Maintainer: Pierre-Olivier Boulant <po2528@gmail.com>
Installed-Size: $(du -sk --exclude=DEBIAN "$root" | cut -f1)
Depends: libc6 (>= 2.39), libstdc++6, libgcc-s1, libasound2t64 | libasound2, libfreetype6, libfontconfig1, libx11-6, libxext6, libxinerama1, libxrandr2, libxcursor1, libxcomposite1, libxrender1
Recommends: pipewire-jack
Section: sound
Priority: optional
Homepage: https://github.com/pob31/go.dot
Description: cross-platform show control
 Go.dot plays a show: cues of sound, fades, OSC and MIDI, run from a list
 with GO, in a window and from a web page beside it. This is a test build,
 not ready to run a show - see /opt/go.dot/README.txt.
EOF

dpkg-deb --root-owner-group --build "$root" "$out" >/dev/null
echo "package-linux-deb: wrote $out"
