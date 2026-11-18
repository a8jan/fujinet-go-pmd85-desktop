#!/bin/sh
# Build and run the GNOME frontend inside the GNOME 51 SDK (flatpak), for
# development without installing GTK/libadwaita on the host.
#
#   tools/dev-gnome.sh sdl     # once: SDL3 into ~/.local/sdl3-gnome51
#   tools/dev-gnome.sh build   # configure (first time) + build build-gnome/
#   tools/dev-gnome.sh run     # run the app
#   tools/dev-gnome.sh shell   # a shell in the SDK, same mounts
#   tools/dev-gnome.sh install-desktop   # launcher entry + icons into
#                                        # ~/.local/share, starting `run`
#
# Run from anywhere; paths resolve against the repository. The FujiNet
# library comes from the host build (tools/fujinet/work/out/), see README.
# GPMD85_SRC (env) points at a local GPMD85Emulator checkout; unset, CMake
# clones the pinned commit.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

SDK=org.gnome.Sdk//51
APP_ID=online.fujinet.go.pmd85.gnome
SDL_TAG=release-3.4.12
SDL_PREFIX='$HOME/.local/sdl3-gnome51'

REPO=$(cd "$(dirname "$0")/.." && pwd)

# The home mount is not optional: the app keeps its settings in
# ~/.config/fujinet-go-pmd85 and its ROMs and FujiNet tree in
# ~/.local/share/fujinet-go-pmd85. Without it they land in the sandbox's
# throwaway home and are gone when the app exits.
MOUNTS="--filesystem=home --filesystem=$REPO"
if [ -n "${GPMD85_SRC:-}" ]; then
    MOUNTS="$MOUNTS --filesystem=$GPMD85_SRC"
fi

sdk() {
    # shellcheck disable=SC2086
    flatpak run --user $MOUNTS "$@"
}

cmd=${1:-}
[ $# -gt 0 ] && shift
case "$cmd" in
sdl)
    sdk --share=network --command=sh "$SDK" -c "
        set -e
        src=\$HOME/.local/src/sdl3
        [ -d \"\$src\" ] || git clone --depth 1 --branch $SDL_TAG https://github.com/libsdl-org/SDL.git \"\$src\"
        cmake -S \"\$src\" -B \"\$src/build\" -G Ninja -DCMAKE_BUILD_TYPE=Release \
              -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DCMAKE_INSTALL_PREFIX=$SDL_PREFIX
        cmake --build \"\$src/build\"
        cmake --install \"\$src/build\""
    ;;
build)
    gp=${GPMD85_SRC:+-DGPMD85_SRC=$GPMD85_SRC}
    # network for the first configure, which may clone the pinned GPMD85
    sdk --share=network --command=sh "$SDK" -c "
        set -e
        cd '$REPO'
        [ -f build-gnome/build.ninja ] || cmake -S . -B build-gnome -G Ninja \
              -DFRONTEND=gnome -DWITH_FUJINET=OFF $gp \
              -DCMAKE_PREFIX_PATH=$SDL_PREFIX
        cmake --build build-gnome"
    ;;
run)
    sdk --socket=wayland --socket=fallback-x11 --share=ipc --share=network \
        --socket=pulseaudio --device=all --own-name="$APP_ID" \
        --cwd="$REPO" \
        --command="$REPO/build-gnome/frontends/gnome/fujinet-go-pmd85-gnome" \
        "$SDK" "$@"
    ;;
shell)
    sdk --share=network --cwd="$REPO" --command=bash "$SDK"
    ;;
install-desktop)
    # Desktops (GNOME Shell, KDE, COSMIC) find a window's name and icon by
    # its app id in the installed launcher entries; a build-tree run has
    # none, hence a gear icon and the id's last word as the name. This
    # installs the entry (starting this script's `run`) and the icons.
    data=${XDG_DATA_HOME:-$HOME/.local/share}
    mkdir -p "$data/applications"
    sed "s|^Exec=.*|Exec=$REPO/tools/dev-gnome.sh run|" \
        "$REPO/frontends/gnome/data/$APP_ID.desktop" \
        > "$data/applications/$APP_ID.desktop"
    for d in "$REPO"/data/icons/hicolor/*/apps; do
        size=$(basename "$(dirname "$d")")
        mkdir -p "$data/icons/hicolor/$size/apps"
        cp "$d/fujinet-go-pmd85.png" "$data/icons/hicolor/$size/apps/$APP_ID.png"
    done
    command -v update-desktop-database >/dev/null &&
        update-desktop-database "$data/applications" || true
    command -v gtk-update-icon-cache >/dev/null &&
        gtk-update-icon-cache -q -t "$data/icons/hicolor" || true
    echo "installed $data/applications/$APP_ID.desktop and icons"
    ;;
*)
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
