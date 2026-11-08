# Mac DMG packaging

Use the complete staged Shipping app, including Contents/UE, or the verified
local delivery app. The packaging script preserves the existing public bundle
identifier and app name, updates the requested version, applies the established
ad-hoc signature and includes the existing bilingual installation documents.

The output directory must be new or empty. Prior output is never deleted.
One app copy is used for both signing and the disk-image payload.

For the custom Finder layout, install the small build-only dependencies in a
project-local environment:

    python3 -m venv .local/toolchains/dmg-python
    .local/toolchains/dmg-python/bin/python3 -m pip install -r tools/release/mac/layout-requirements.txt
    export QINGLAN_DMG_LAYOUT_PYTHON="$PWD/.local/toolchains/dmg-python/bin/python3"
    export QINGLAN_APP="$PWD/.local/releases/visual-upgrade-2026-11-08/QinglanAscension-Mac-Shipping.app"
    export QINGLAN_VERSION=0.1.3
    export QINGLAN_RELEASE=0.1.3
    export QINGLAN_BUILD_NUMBER=20261108
    export QINGLAN_OUT_DIR="$PWD/.local/releases/visual-upgrade-2026-11-08/dmg"
    bash tools/release/package-mac-unsigned.sh

Without QINGLAN_DMG_LAYOUT_PYTHON, the script creates the ordinary disk image
with the same app and Applications shortcut. With it, an HFS+ image is mounted
briefly to write Finder window options, icon positions and a background alias
that points inside the disk image. It is then detached and compressed to UDZO.
The temporary writable image is removed after successful conversion. No Finder
automation is needed.

Before upload, verify the image with hdiutil verify, attach it read-only,
verify the app signature and cooked-file hashes, and run the contained game
with a fresh QA directory inside its sandbox. Detach the image after the run.
Upload the DMG and adjacent SHA256SUMS together; the build script itself makes
no GitHub changes. Release prose is prepared in docs/releases/v0.1.3.md.
