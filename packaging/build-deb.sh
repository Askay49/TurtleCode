#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 APPIMAGE [OUTPUT_DIR]" >&2
    exit 1
fi

appimage=$(realpath "$1")
output_dir=$(realpath -m "${2:-dist}")
architecture=$(dpkg --print-architecture)
work_dir=$(mktemp -d)
trap 'rm -rf -- "$work_dir"' EXIT
package="$work_dir/package"
private_dir="$package/usr/lib/turtle-code"

(cd "$work_dir" && "$appimage" --appimage-extract >/dev/null)
mkdir -p "$package/DEBIAN" "$package/usr/bin" "$package/usr/lib" "$package/usr/share"
mv "$work_dir/squashfs-root/usr" "$private_dir"

for directory in applications icons; do
    mv "$private_dir/share/$directory" "$package/usr/share/"
done
mkdir -p "$package/usr/share/doc"
mv "$private_dir/share/licenses/turtle-code" "$package/usr/share/doc/"
doc_dir="$package/usr/share/doc/turtle-code"
mv "$doc_dir/LICENSE" "$doc_dir/copyright"
if [[ -d "$private_dir/share/doc" ]]; then
    mv "$private_dir/share/doc" "$doc_dir/bundled-libraries"
fi

# Keep these libraries in Depends so the package uses the host's versions.
rm -f -- "$private_dir/lib/"{libstdc++,libgcc_s,libglib-2.0,libzstd,libssl,libcrypto}.so* \
    "$private_dir/lib/"{libpcre2-8,libpcre2-16,libsqlite3}.so*

for file in lib/{libQt6Core.so.6,libgit2.so.1.1,libtree-sitter.so.0,libeditorconfig.so.0} \
    plugins/platforms/{libqxcb.so,libqwayland-generic.so}; do
    if [[ ! -f "$private_dir/$file" ]]; then
        echo "AppImage is missing $file." >&2
        exit 1
    fi
done

# linuxdeploy sets the ELF search paths and qt.conf so the executable can start
# from its installed location without exporting library paths to child processes.
ln -s ../lib/turtle-code/bin/turtle-code "$package/usr/bin/turtle-code"
find "$package" -type d -exec chmod 0755 {} +

mkdir -m 0700 "$work_dir/config" "$work_dir/data" "$work_dir/runtime"
version_output=$(env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH \
    QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$work_dir/config" \
    XDG_DATA_HOME="$work_dir/data" XDG_RUNTIME_DIR="$work_dir/runtime" \
    timeout 30 "$package/usr/bin/turtle-code" --version)
if [[ ! $version_output =~ ^Turtle\ Code\ v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
    echo "Unexpected Turtle Code version: $version_output" >&2
    exit 1
fi
version="${BASH_REMATCH[1]}-1"

installed_size=$(du -sk "$package/usr" | cut -f1)
cat > "$package/DEBIAN/control" <<EOF
Package: turtle-code
Version: $version
Architecture: $architecture
Maintainer: Turtle Code contributors <Askay49@users.noreply.github.com>
Section: editors
Priority: optional
Installed-Size: $installed_size
Depends: ca-certificates, libc6 (>= 2.35), libcom-err2, libegl1,
 libfontconfig1, libfreetype6, libgcc-s1, libgl1, libglib2.0-0t64 | libglib2.0-0,
 libglx0, libgpg-error0, libharfbuzz0b, libice6, libopengl0, libpcre2-8-0,
 libpcre2-16-0, libsm6, libsqlite3-0, libssl3t64 | libssl3, libstdc++6 (>= 12),
 libwayland-client0, libx11-6, libx11-xcb1, libxcb1, libzstd1, zlib1g
Recommends: git
Homepage: https://github.com/Askay49/TurtleCode
Description: Native Qt code editor with an integrated terminal and debugger
EOF

filename="turtle-code_${version}_${architecture}.deb"
dpkg-deb --build --root-owner-group -Zxz "$package" "$work_dir/$filename"
mkdir -p "$output_dir"
mv "$work_dir/$filename" "$output_dir/$filename"
