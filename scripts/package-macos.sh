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
# Signs the Go.dot.app that `cmake --install` laid out, puts it in a DMG, and
# notarizes and staples the DMG. The macOS half of .github/workflows/release.yml,
# and runnable on the author's Mac as it is. The same steps as WFS-DIY's
# scripts/ci/build-macos-release.sh, for a bundle CMake built rather than Xcode.
#
#     scripts/package-macos.sh <staging-folder> <out.dmg>
#
# The staging folder is the install prefix: Go.dot.app plus the files that sit
# beside it in the DMG's window. The DMG also gets an Applications link.
#
# SIGNING IDENTITY: $DEVELOPER_ID, or the one "Developer ID Application" in the
# keychain (CI imports it from MACOS_CERT_P12_BASE64). With neither, the app is
# signed AD HOC and nothing is notarized - a build a tester can still open with
# the quarantine cleared, which is what a manual run on a fork gets. REQUIRE_SIGNED=1
# turns that fallback into an error; the workflow sets it for a tag.
#
# NOTARIZATION: an App Store Connect API key - NOTARY_API_KEY (the .p8's path),
# NOTARY_API_KEY_ID and NOTARY_API_ISSUER - or a notarytool keychain profile
# named by NOTARY_PROFILE on a Mac that has one. SKIP_NOTARIZE=1 skips it.
#
# INSIDE OUT, because a signature seals what is inside it: wfg first, with the
# entitlements (packaging/macos/entitlements.plist says what each is for), then
# the bundle, which signs the launcher as its main executable and seals wfg,
# Info.plist and Resources/ into one signature.

set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <staging-folder> <out.dmg>" >&2
    exit 2
fi

here="$(cd "$(dirname "$0")/.." && pwd)"
stage="$(cd "$1" && pwd)"
dmg="$2"
app="$stage/Go.dot.app"
entitlements="$here/packaging/macos/entitlements.plist"

[ -d "$app" ] || { echo "error: no Go.dot.app in $stage" >&2; exit 1; }

# --- Identity ---------------------------------------------------------------
if [ -z "${DEVELOPER_ID:-}" ]; then
    DEVELOPER_ID="$(security find-identity -v -p codesigning \
        | sed -nE 's/.*"(Developer ID Application: .*)"/\1/p' | head -n1)"
fi

if [ -z "$DEVELOPER_ID" ]; then
    if [ "${REQUIRE_SIGNED:-0}" = "1" ]; then
        echo "error: no 'Developer ID Application' identity, and this build must be signed." >&2
        echo "       In CI: are MACOS_CERT_P12_BASE64 and MACOS_CERT_PASSWORD in the job's environment?" >&2
        exit 1
    fi
    echo "==> No Developer ID: signing ad hoc, not notarizing"
    DEVELOPER_ID="-"
    SKIP_NOTARIZE=1
fi

# --timestamp and the hardened runtime only mean something with a real identity;
# an ad-hoc signature takes neither.
sign_flags=(--force --sign "$DEVELOPER_ID")
if [ "$DEVELOPER_ID" != "-" ]; then
    sign_flags+=(--options runtime --timestamp)
fi

echo "==> Signing as: $DEVELOPER_ID"

# --- Sign, inside out -------------------------------------------------------
codesign "${sign_flags[@]}" --entitlements "$entitlements" "$app/Contents/MacOS/wfg"
codesign "${sign_flags[@]}" --entitlements "$entitlements" "$app"
codesign --verify --strict --deep --verbose=2 "$app"

if codesign -d --entitlements - "$app/Contents/MacOS/wfg" 2>/dev/null \
        | grep -A1 get-task-allow | grep -qi true; then
    echo "error: get-task-allow is true on wfg - notarization would refuse it" >&2
    exit 1
fi

# --- The DMG ----------------------------------------------------------------
# A copy of the staging folder plus an Applications link, so the window is
# "drag Go.dot to Applications". hdiutil is retried: on a CI runner a volume of
# the same name left attached, or Spotlight in the fresh tree, fails it now
# and then with "Resource busy" (WFS-DIY's finding).
echo "==> Building $dmg"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
ditto "$stage" "$work/Go.dot"
ln -s /Applications "$work/Go.dot/Applications"

rm -f "$dmg"
attempt=0
until hdiutil create -volname "Go.dot" -srcfolder "$work/Go.dot" -ov -format UDZO "$dmg" >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 5 ]; then
        hdiutil create -volname "Go.dot" -srcfolder "$work/Go.dot" -ov -format UDZO "$dmg" >&2
        exit 1
    fi
    echo "    hdiutil busy - detach and retry $attempt/5"
    hdiutil detach "/Volumes/Go.dot" >/dev/null 2>&1 || true
    sleep 5
done

if [ "$DEVELOPER_ID" != "-" ]; then
    codesign --force --timestamp --sign "$DEVELOPER_ID" "$dmg"
fi

# --- Notarize and staple ----------------------------------------------------
if [ "${SKIP_NOTARIZE:-0}" = "1" ]; then
    echo "==> Not notarized: $dmg"
    exit 0
fi

if [ -n "${NOTARY_API_KEY:-}" ] && [ -n "${NOTARY_API_KEY_ID:-}" ] && [ -n "${NOTARY_API_ISSUER:-}" ]; then
    auth=(--key "$NOTARY_API_KEY" --key-id "$NOTARY_API_KEY_ID" --issuer "$NOTARY_API_ISSUER")
elif [ -n "${NOTARY_PROFILE:-}" ]; then
    auth=(--keychain-profile "$NOTARY_PROFILE")
else
    echo "error: signed, but no notarization credentials (NOTARY_API_KEY + _ID + _ISSUER, or NOTARY_PROFILE)." >&2
    echo "       Set SKIP_NOTARIZE=1 to stop at a signed DMG." >&2
    exit 1
fi

# The status is READ, not inferred from the exit code: notarytool's --wait
# returns once Apple has an answer, and "Invalid" is an answer. On anything but
# Accepted, Apple's log - which names the file and the reason - goes in the job
# output before the job fails.
echo "==> Notarizing (this waits for Apple)"
result="$(xcrun notarytool submit "$dmg" "${auth[@]}" --wait --output-format json)"
echo "$result"
status="$(printf '%s' "$result" | /usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))')"
if [ "$status" != "Accepted" ]; then
    id="$(printf '%s' "$result" | /usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')"
    [ -n "$id" ] && xcrun notarytool log "$id" "${auth[@]}" || true
    echo "error: notarization says '$status'" >&2
    exit 1
fi

xcrun stapler staple "$dmg"
xcrun stapler validate "$dmg"
spctl --assess --type open --context context:primary-signature --verbose=2 "$dmg"
echo "==> Signed, notarized and stapled: $dmg"
