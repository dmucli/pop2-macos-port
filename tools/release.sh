#!/bin/zsh
# Build every package and create a DRAFT GitHub release with them (publish it on GitHub).
#   tools/release.sh 0.3 [notes.md]
# - macOS: Developer ID signature + notarization (`make dist`; SIGN_ID defaults to the first
#   "Developer ID Application" identity in the keychain, NOTARY_PROFILE to "notary")
# - Linux: Recalbox (arm64, x86_64), desktop tarballs, PortMaster (`make packages`, Docker)
# - tools/crosscheck.sh must pass (same emulated memory on macOS and Linux) unless
#   SKIP_CROSSCHECK=1
# The version goes into packaging/Info.plist; the working tree must be clean otherwise.
set -eu
ROOT=${0:A:h:h}
cd $ROOT
VERSION=${1:?usage: tools/release.sh VERSION [notes.md]}
NOTES=${2:-}
TAG=v$VERSION

[ -z "$(git status --porcelain -- . ':!packaging/Info.plist')" ] || { echo "commit your changes first"; exit 1; }
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && { echo "$TAG already exists"; exit 1; }
SIGN_ID=${SIGN_ID:-$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)}
[ -n "$SIGN_ID" ] || { echo "no Developer ID Application identity in the keychain"; exit 1; }

# version
plutil -replace CFBundleShortVersionString -string "$VERSION" packaging/Info.plist
plutil -replace CFBundleVersion -string "$(( $(git rev-list --count HEAD) + 1 ))" packaging/Info.plist
if [ -n "$(git status --porcelain packaging/Info.plist)" ]; then
    git commit -q -m "Version $VERSION" packaging/Info.plist
fi

[ "${SKIP_CROSSCHECK:-0}" = 1 ] || tools/crosscheck.sh arm64 amd64

make dist BUILD=build/release CORE=mini SIGN_ID="$SIGN_ID" NOTARY_PROFILE=${NOTARY_PROFILE:-notary}
make packages

ASSETS=(build/release/PoP2-Port-macOS.zip build/PoP2-Port-Recalbox-arm64.zip build/PoP2-Port-Recalbox-x86_64.zip
        build/PoP2-Port-Linux-arm64.tar.gz build/PoP2-Port-Linux-x86_64.tar.gz build/PoP2-Port-PortMaster.zip)
for a in $ASSETS; do [ -f $a ] || { echo "missing $a"; exit 1; }; done

git tag -a $TAG -m "Version $VERSION"
git push -q origin HEAD $TAG
if [ -n "$NOTES" ]; then
    gh release create $TAG --draft --title "$TAG" --notes-file "$NOTES" $ASSETS
else
    gh release create $TAG --draft --title "$TAG" --generate-notes $ASSETS
fi
echo "draft release $TAG created: review and publish it on GitHub"
