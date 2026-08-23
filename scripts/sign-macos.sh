#!/usr/bin/env bash
# Signs, and optionally notarises, what it is given. Must run on a Mac -
# codesign, productsign and notarytool are Apple tools.
#
#   scripts/sign-macos.sh code   "dist/Fluid Solver 0.2 macos-arm64 omp" ...
#   scripts/sign-macos.sh app    "/path/to/Fluid Solver UI.app"
#   scripts/sign-macos.sh pkg    "dist/Fluid Solver 0.2 macos.pkg"
#   scripts/sign-macos.sh notary "dist/Fluid Solver 0.2 macos.pkg"
#
# Two identities, because Apple issues two certificates and they are not
# interchangeable:
#
#   CFD_SIGN_MACOS_APP_IDENTITY        "Developer ID Application: NAME (TEAM)"
#                                      signs executables and .app bundles.
#   CFD_SIGN_MACOS_INSTALLER_IDENTITY  "Developer ID Installer: NAME (TEAM)"
#                                      signs the .pkg. A .pkg signed with the
#                                      Application certificate is rejected.
#
# Both come from an Apple Developer Program membership ($99/year) and are
# created in Certificates, Identifiers & Profiles. `security find-identity -v`
# prints the exact strings once they are in the login keychain.
#
# Notarisation is separate from signing and is what actually stops the "cannot
# be opened because Apple cannot check it for malicious software" dialog on
# macOS 10.15 and later. Apple runs an automated scan and hands back a ticket;
# stapler writes that ticket into the file so it verifies with no network.
# Either:
#
#   CFD_NOTARY_PROFILE   the name of a profile stored earlier with
#                        `xcrun notarytool store-credentials`. One value, no
#                        secrets on the command line - the right thing locally.
#   CFD_NOTARY_APPLE_ID + CFD_NOTARY_TEAM_ID + CFD_NOTARY_PASSWORD
#                        an Apple ID with an app-specific password (appleid.
#                        apple.com > Sign-In and Security > App-Specific
#                        Passwords). The right thing in CI, as three secrets.
#
# With nothing configured every step reports what it skipped and exits 0, so an
# unsigned build is still a build. --require turns that into an error.

set -uo pipefail

REQUIRE=0
if [ "${1:-}" = "--require" ]; then REQUIRE=1; shift; fi

MODE="${1:-}"
shift || true

# Two ways to stop, and the difference between them matters to the caller.
#
# skip: nothing is configured, or this is not a Mac. That is a choice, not a
#       fault, so the build carries on - unless --require says this build is the
#       one that must not ship unsigned.
# die:  something was configured and then went wrong. Always an error, because
#       the alternative is a release that is quietly half-signed.
skip() {
    if [ "$REQUIRE" = 1 ]; then
        echo "sign-macos: $*" >&2
        exit 1
    fi
    echo "sign-macos: $*"
    exit 0
}

die() {
    echo "sign-macos: $*" >&2
    exit 1
}

command -v codesign >/dev/null 2>&1 || skip "this is not a Mac (no codesign), nothing was signed"

APP_ID="${CFD_SIGN_MACOS_APP_IDENTITY:-}"
INSTALLER_ID="${CFD_SIGN_MACOS_INSTALLER_IDENTITY:-}"

case "$MODE" in
# ---- Mach-O executables and .app bundles ------------------------------------
# --options runtime is the hardened runtime, which notarisation requires; a
# binary signed without it is accepted by codesign and then rejected by Apple.
# --timestamp is equally non-optional: an untimestamped signature stops
# verifying the day the certificate expires.
code|app)
    [ "$#" -gt 0 ] || { echo "sign-macos: nothing to sign"; exit 0; }
    [ -n "$APP_ID" ] || skip "CFD_SIGN_MACOS_APP_IDENTITY is not set, so no binary was signed.
  Users will see \"cannot be opened because the developer cannot be verified\"
  and have to right-click > Open the first time. Set it to the
  \"Developer ID Application: NAME (TEAM)\" string from \`security find-identity -v\`."

    failed=0
    for target in "$@"; do
        [ -e "$target" ] || continue
        printf '  signing %s ... ' "$(basename "$target")"
        # --force replaces a signature that is already there, which is what
        # re-running a build does. --deep is deliberately not used: it is
        # documented as unreliable for anything with nested code, and these
        # bundles hold exactly one executable each.
        if codesign --force --options runtime --timestamp \
                    --sign "$APP_ID" "$target" >/dev/null 2>&1; then
            echo "ok"
        else
            echo "failed"
            failed=$((failed + 1))
        fi
    done
    if [ "$failed" -gt 0 ]; then
        die "$failed file(s) could not be signed. Run codesign by hand for its own message."
    fi
    ;;

# ---- the installer package --------------------------------------------------
# productsign cannot sign in place: it reads one file and writes another, so
# the output is moved back over the input.
pkg)
    PKG="${1:-}"
    [ -n "$PKG" ] && [ -f "$PKG" ] || { echo "sign-macos: no package given"; exit 0; }
    [ -n "$INSTALLER_ID" ] || skip "CFD_SIGN_MACOS_INSTALLER_IDENTITY is not set, so the package is unsigned.
  Set it to the \"Developer ID Installer: NAME (TEAM)\" string - note Installer,
  not Application; the Installer's package check rejects the other one."

    tmp="$PKG.signed"
    printf '  signing %s ... ' "$(basename "$PKG")"
    if productsign --sign "$INSTALLER_ID" --timestamp "$PKG" "$tmp" >/dev/null 2>&1; then
        mv -f "$tmp" "$PKG"
        echo "ok"
    else
        rm -f "$tmp"
        echo "failed"
        die "productsign failed. Check that the identity exists: security find-identity -v"
    fi
    ;;

# ---- notarisation -----------------------------------------------------------
notary)
    PKG="${1:-}"
    [ -n "$PKG" ] && [ -f "$PKG" ] || { echo "sign-macos: no package given"; exit 0; }
    command -v xcrun >/dev/null 2>&1 || skip "xcrun is missing, so nothing was notarised"

    if [ -n "${CFD_NOTARY_PROFILE:-}" ]; then
        set -- --keychain-profile "$CFD_NOTARY_PROFILE"
    elif [ -n "${CFD_NOTARY_APPLE_ID:-}" ] && [ -n "${CFD_NOTARY_TEAM_ID:-}" ] &&
         [ -n "${CFD_NOTARY_PASSWORD:-}" ]; then
        set -- --apple-id "$CFD_NOTARY_APPLE_ID" \
               --team-id "$CFD_NOTARY_TEAM_ID" \
               --password "$CFD_NOTARY_PASSWORD"
    else
        skip "no notary credentials, so the package is signed but not notarised.
  Signing alone is no longer enough on macOS 10.15 and later - Gatekeeper still
  shows a warning until Apple has seen the file. Set CFD_NOTARY_PROFILE, or
  CFD_NOTARY_APPLE_ID + CFD_NOTARY_TEAM_ID + CFD_NOTARY_PASSWORD."
    fi

    echo "  submitting $(basename "$PKG") to Apple (this takes a few minutes) ..."
    if ! xcrun notarytool submit "$PKG" "$@" --wait; then
        die "notarytool rejected the package. \`xcrun notarytool log <id>\` says why -
  almost always a binary that was signed without --options runtime."
    fi

    printf '  stapling the ticket ... '
    if xcrun stapler staple "$PKG" >/dev/null 2>&1; then
        echo "ok"
        echo "sign-macos: $(basename "$PKG") is signed, notarised and stapled."
    else
        echo "failed"
        die "the ticket could not be stapled. The package is still notarised, so it
  opens on a machine that can reach Apple - but not on one that cannot."
    fi
    ;;

*)
    echo "usage: sign-macos.sh [--require] code|app|pkg|notary <file> ..." >&2
    exit 2
    ;;
esac

exit 0
