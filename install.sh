#!/usr/bin/env bash
# install.sh  ->  build Genie and install it for your user (no sudo, nothing outside $HOME).
#   ./install.sh              build + install
#   ./install.sh --uninstall  remove what it installed
# Re-run it after every Hyprland update: plugins must be rebuilt for each version.
set -u
cd "$(dirname "$(readlink -f "$0")")"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/hypr-genie"
BIN="$HOME/.local/bin"
SCRIPTS="genie-dock-icon genie-minimize genie-restore genie-events"
ok()  { printf '  \033[32m✓\033[0m %s\n' "$*"; }
die() { printf '  \033[31m✗\033[0m %s\n' "$*"; exit 1; }

if [ "${1:-}" = --uninstall ]; then
  hyprctl plugin unload "$DEST/genie.so" >/dev/null 2>&1
  for s in $SCRIPTS; do rm -f "$BIN/$s"; done
  rm -rf "$DEST"; ok "Genie removed (remove its lines from your Hyprland config too)"; exit 0
fi

echo "Checking your system"
for c in hyprctl jq socat make pkg-config "${CXX:-g++}"; do
  command -v "$c" >/dev/null 2>&1 || die "missing '$c' - install it with your package manager"
done
HDR=$(pkg-config --modversion hyprland 2>/dev/null) \
  || die "Hyprland's headers are missing (Fedora: sudo dnf install hyprland-devel glslang-devel)"
RUN=$(hyprctl version -j 2>/dev/null | jq -r '.tag // empty')
if [ -n "$RUN" ] && [[ "${RUN#v}" != "$HDR"* ]]; then
  die "you are running Hyprland $RUN but the headers are $HDR - update both, log in again, re-run"
fi
ok "Hyprland ${RUN:-?}, headers $HDR"

echo "Building (about a minute)"
if ! make -B CXX="${CXX:-g++}" > build.log 2>&1; then
  grep -m 12 'error' build.log; die "build failed - full log in build.log"
fi
ok "built genie.so"

echo "Installing"
mkdir -p "$DEST" "$BIN"
cp genie.so "$DEST/genie.so.tmp" && mv -f "$DEST/genie.so.tmp" "$DEST/genie.so"
for s in $SCRIPTS; do install -m 755 "scripts/$s" "$BIN/$s"; done
ok "$DEST/genie.so"
ok "$BIN/{${SCRIPTS// /,}}"

cat <<MSG

Next: add the lines from the README's "Set up" section to your Hyprland config,
then reload. To try it right now:
  hyprctl plugin load $DEST/genie.so
  genie-minimize          (minimises the focused window)
  genie-restore           (brings it back)
MSG
