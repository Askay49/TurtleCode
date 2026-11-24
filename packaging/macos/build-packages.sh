#!/usr/bin/env bash
set -euo pipefail

build_dir=$(cd "$1" && pwd)
mkdir -p "$2"
output_dir=$(cd "$2" && pwd)
macdeployqt="$3"
arch=$(uname -m)

work_dir=$(mktemp -d)
trap 'rm -rf -- "$work_dir"' EXIT
image_root="$work_dir/image"
mkdir -p "$image_root"
app="$image_root/Turtle Code.app"
ditto "$build_dir/turtle-code.app" "$app"

cmake --install "$build_dir" --prefix "$work_dir/install"
ditto "$work_dir/install/share/licenses/turtle-code" "$app/Contents/Resources/licenses"
"$macdeployqt" "$app" -no-codesign "-libpath=$(brew --prefix)/lib"

# Drop Homebrew search paths so the app uses its bundled copies even on the runner.
while IFS= read -r -d '' binary; do
    [[ $(file -b "$binary") == *Mach-O* ]] || continue
    chmod u+w "$binary"
    while IFS= read -r rpath; do
        case "$rpath" in
            /opt/homebrew/*|/usr/local/*) install_name_tool -delete_rpath "$rpath" "$binary" ;;
        esac
    done < <(otool -l "$binary" | awk '/cmd LC_RPATH$/ { getline; getline; sub(/^ *path /, ""); sub(/ \(offset [0-9]+\)$/, ""); print }' | sort -u)
    if [[ $arch == x86_64 ]] && codesign -d "$binary" 2>/dev/null; then
        codesign --remove-signature "$binary"
    fi
done < <(find "$app/Contents" -type f -print0)

if [[ $arch == arm64 ]]; then
    # Apple Silicon requires an ad-hoc signature even for local execution.
    codesign --force --deep --sign - "$app"
    codesign --verify --deep --strict "$app"
fi

icon=$(/usr/libexec/PlistBuddy -c 'Print CFBundleIconFile' "$app/Contents/Info.plist")
test "$icon" = turtle-code.icns
test -s "$app/Contents/Resources/$icon"
test -f "$app/Contents/PlugIns/platforms/libqcocoa.dylib"
plist="$app/Contents/Info.plist"
plutil -lint "$plist"
test "$(/usr/libexec/PlistBuddy -c 'Print CFBundleDocumentTypes:0:CFBundleTypeRole' "$plist")" = Editor
/usr/libexec/PlistBuddy -c 'Print CFBundleDocumentTypes:0:LSItemContentTypes' "$plist" | grep -x '[[:space:]]*public[.]plain-text' >/dev/null

binary="$app/Contents/MacOS/turtle-code"
platform=cocoa
if [[ -f "$app/Contents/PlugIns/platforms/libqoffscreen.dylib" ]]; then
    platform=offscreen
fi
if ! version=$(DYLD_PRINT_LIBRARIES=1 QT_QPA_PLATFORM="$platform" "$binary" --version 2>"$work_dir/loaded-libraries"); then
    cat "$work_dir/loaded-libraries" >&2
    exit 1
fi
if grep -E '/opt/homebrew/|/usr/local/(Cellar|opt|lib|Frameworks)/' "$work_dir/loaded-libraries"; then
    echo 'The packaged app loads libraries from Homebrew.' >&2
    exit 1
fi
if [[ ! $version =~ ^Turtle\ Code\ v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
    echo "Unexpected version from the packaged app: $version" >&2
    exit 1
fi
echo "$version"
name="Turtle_Code-${BASH_REMATCH[1]}-macOS-$arch"

ditto -c -k --sequesterRsrc --keepParent "$app" "$output_dir/$name.app.zip"
ln -s /Applications "$image_root/Applications"
hdiutil create -srcfolder "$image_root" -volname 'Turtle Code' -format UDZO "$output_dir/$name.dmg"
hdiutil verify "$output_dir/$name.dmg"
