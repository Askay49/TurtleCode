#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -m) != x86_64 ]]; then
    echo 'AppImage packaging requires an x86_64 Linux host.' >&2
    exit 1
fi

build_dir=$(realpath "${1:-build/appimage}")
output_dir=$(realpath -m "${2:-dist}")
mkdir -p "$output_dir"
work_dir=$(mktemp -d "$build_dir/appimage.XXXXXX")
trap 'rm -rf -- "$work_dir"' EXIT
app_dir="$work_dir/AppDir"
tools_dir="$work_dir/tools"
mkdir -p "$tools_dir"

# Release tags and SHA-256 checksums pin the packaging tools.
# Update both together when upgrading linuxdeploy or its Qt plugin.
download_tool() {
    local repository=$1 release=$2 name=$3 checksum=$4
    curl --fail --location --retry 3 --silent --show-error \
        "https://github.com/$repository/releases/download/$release/$name" \
        --output "$tools_dir/$name"
    printf '%s  %s\n' "$checksum" "$tools_dir/$name" | sha256sum --check --status
    chmod +x "$tools_dir/$name"
}
download_tool linuxdeploy/linuxdeploy 1-alpha-20251107-1 linuxdeploy-x86_64.AppImage \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
download_tool linuxdeploy/linuxdeploy-plugin-qt 1-alpha-20250213-1 linuxdeploy-plugin-qt-x86_64.AppImage \
    15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724

DESTDIR="$app_dir" cmake --install "$build_dir" --prefix /usr
desktop-file-validate "$app_dir/usr/share/applications/turtle-code.desktop"
cp "$app_dir/usr/share/icons/hicolor/256x256/apps/turtle-code.png" "$app_dir/turtle-code.png"
ln -s turtle-code.png "$app_dir/.DirIcon"

# Extraction avoids requiring FUSE on the build runner. Bundle TLS libraries
# explicitly because QtNetwork loads its SSL backend at runtime.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-/usr/bin/qmake6}"
export EXTRA_PLATFORM_PLUGINS='libqoffscreen.so;libqwayland-egl.so;libqwayland-generic.so'
export LDAI_OUTPUT="$output_dir/Turtle_Code-x86_64.AppImage"
export LDAI_NO_APPSTREAM=1
export ARCH=x86_64
multiarch=$(${CC:-cc} -print-multiarch)
"$tools_dir/linuxdeploy-x86_64.AppImage" \
    --appdir "$app_dir" \
    --desktop-file "$app_dir/usr/share/applications/turtle-code.desktop" \
    --icon-file "$app_dir/usr/share/icons/hicolor/256x256/apps/turtle-code.png" \
    --library "/usr/lib/$multiarch/libssl.so.3" \
    --library "/usr/lib/$multiarch/libcrypto.so.3" \
    --library "$(${CXX:-c++} -print-file-name=libstdc++.so.6)" \
    --plugin qt --output appimage

# Verify the packaged launcher and offscreen Qt plugin, not the build binary.
# Keep settings/migration and IPC isolated from the user's installed editor.
mkdir -p "$work_dir/smoke" "$work_dir/config" "$work_dir/data" "$work_dir/runtime"
chmod 700 "$work_dir/runtime"
(
    cd "$work_dir/smoke"
    "$LDAI_OUTPUT" --appimage-extract >/dev/null
    env -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH \
        QT_QPA_PLATFORM=offscreen \
        XDG_CONFIG_HOME="$work_dir/config" XDG_DATA_HOME="$work_dir/data" \
        XDG_RUNTIME_DIR="$work_dir/runtime" \
        timeout 30 ./squashfs-root/AppRun --version
)
(
    cd "$output_dir"
    sha256sum Turtle_Code-x86_64.AppImage > Turtle_Code-x86_64.AppImage.sha256
)
