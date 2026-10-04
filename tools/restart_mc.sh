#!/bin/bash
# Restart the "DS Passthrough" Minecraft instance without losing the world: save through the mod's link, close the
# window the polite way (KWin script = the window's close button: Minecraft saves and quits), install the freshly
# built mod jar, launch again through PrismLauncher.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
INSTANCE="${MC_INSTANCE:-DS Passthrough}"
MODS="$HOME/.local/share/PrismLauncher/instances/$INSTANCE/minecraft/mods"
PID=$(pgrep -f "java.*instances/$INSTANCE" | head -1 || true)
if [ -n "$PID" ]; then
	timeout 30 uv run --quiet --with websockets "$HERE/tools/save.py" || echo "save through the link failed (old mod?): the close still saves"
	JS=$(mktemp --suffix=.js)
	cat > "$JS" <<JSEOF
const wins = workspace.windowList();
for (let i = 0; i < wins.length; ++i)
    if (wins[i].pid === $PID)
        wins[i].closeWindow();
JSEOF
	id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$JS" dsmc_close_mc)
	qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run
	sleep 1
	qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript dsmc_close_mc >/dev/null || true
	rm -f "$JS"
	for _ in $(seq 1 40); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
	kill -0 "$PID" 2>/dev/null && { echo "Minecraft didn't close; not touching it"; exit 1; }
fi
cp "$HERE/mc/build/libs/passthrough-0.1.0.jar" "$MODS/passthrough-0.1.0.jar.new" && mv -f "$MODS/passthrough-0.1.0.jar.new" "$MODS/passthrough-0.1.0.jar"
setsid -f prismlauncher -l "$INSTANCE" >/dev/null 2>&1
LOG="$HOME/.local/share/PrismLauncher/instances/$INSTANCE/minecraft/logs/latest.log"
sleep 6
for _ in $(seq 1 40); do grep -qE 'Loaded [0-9]+ advancements' "$LOG" 2>/dev/null && break; sleep 3; done
grep -E 'Loading Minecraft|host link|Mixin.*(rror|ail)|ERROR|Exception' "$LOG" | grep -v narrator | cut -c1-160 | head -8
