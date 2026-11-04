# NoFocusLoss for Moon Sweepers

Keep driving while you alt-tab.

In vanilla Moon Sweepers your vehicle stops as soon as the game window loses focus: Alt+Tab, clicking a second
monitor, opening Discord or a browser. This mod hides the focus loss from the game, so your vehicle keeps going on
cruise control, autopilot or held keys while you do something else.

- **No mod loader needed.** No BepInEx, no MelonLoader. One DLL, drop it in and you're done.
- **Client-side only.** It doesn't change save files, servers or other players.
- **Easy to remove.** Delete the files and the game is vanilla again.

---

## Installation

1. **Download** the latest `NoFocusLoss-MoonSweepers-vX.Y.Z.zip` from the
   [Releases](../../releases/latest) page.
2. **Open the game folder.** In Steam, right-click **Moon Sweepers**, then choose **Manage** → **Browse local files**.
   The default location is:
   ```
   C:\Program Files (x86)\Steam\steamapps\common\Moon Sweepers
   ```
3. **Extract** `version.dll` and `NoFocusLoss.ini` from the zip into that folder, next to `MoonSweepers.exe`:
   ```
   Moon Sweepers\
   ├── MoonSweepers.exe
   ├── UnityPlayer.dll
   ├── version.dll        <-- from the mod
   ├── NoFocusLoss.ini    <-- from the mod
   └── ...
   ```
4. **Start the game** as usual through Steam.

### Check that it works

After starting the game, a file called `NoFocusLoss.log` appears in the game folder. It should contain lines like
these:

```
NoFocusLoss 1.0.0 loaded. Enabled=1 ...
USER32 hooks active in UnityPlayer.dll: 14
Game window attached (hwnd=...)
```

Alt-tab out of the game and you'll see:

```
Focus loss hidden from game (WM_ACTIVATE) - game keeps running.
```

## Uninstall

Delete these files from the game folder:

- `version.dll`
- `NoFocusLoss.ini`
- `NoFocusLoss.log`

## Configuration

Edit `NoFocusLoss.ini` in the game folder and restart the game after changing it.
Every setting is `1` for on or `0` for off.

| Setting | Default | Description |
|---|---|---|
| `Enabled` | `1` | Turns the whole mod on or off. |
| `KeepHeldKeys` | `1` | Keys you hold while tabbing out (e.g. **W**) stay pressed in game, so the vehicle keeps driving. Alt/Ctrl/Shift/Win/Tab are always released. With `0`, every key is released and only cruise control or autopilot keeps driving. |
| `BlockBackgroundInput` | `1` | Typing or clicking in other windows never controls the game. |
| `ReleaseCursor` | `1` | The mouse is never locked to the game window while it's in the background. |
| `Log` | `1` | Writes `NoFocusLoss.log` to the game folder. |

## Tips and known limitations

- Use **Windowed** or **Borderless Fullscreen**. Exclusive fullscreen can still minimize when you alt-tab.
- With `KeepHeldKeys=1`: if you held a key while tabbing out, tap it once when you come back so the game sees it as
  released.
- After a game update the mod should keep working. If it stops, check `NoFocusLoss.log` and open an
  [issue](../../issues) with the log attached.

## FAQ

**Is this safe? Will it get me banned?**
The mod only changes how the game window reacts to focus changes on your own PC. It doesn't touch game files, memory
values, saves or network traffic. Moon Sweepers has no anti-cheat. Use it at your own risk anyway, like any mod.

**Why `version.dll`?**
Unity's `UnityPlayer.dll` loads `version.dll`, and Windows checks the game folder first. The mod passes every real
`version.dll` function through to `C:\Windows\System32\version.dll`, so nothing breaks. This "DLL proxy" trick is used by
many mods and mod loaders.

**Why not BepInEx?**
Moon Sweepers uses Unity 6000.6 with IL2CPP metadata v108, which BepInEx doesn't officially support yet. A native DLL
works whatever the IL2CPP version and needs nothing else installed.

**My antivirus flags `version.dll`.**
DLL proxies sometimes trigger heuristic warnings. The full source is in this repository, so you can read it and
[build it yourself](#building-from-source).

## How it works

1. **Loading.** `version.dll` is loaded by `UnityPlayer.dll` at startup. It does nothing outside the game process, for
   example in the crash handler.
2. **Window procedure.** The mod wraps Unity's window procedure (`UnityWndClass`). It swallows the "lost focus"
   messages `WM_ACTIVATE` (inactive), `WM_ACTIVATEAPP` (false) and `WM_KILLFOCUS`. The engine, the Input System and the
   game scripts never see the focus loss, so nothing calls `OnApplicationFocus(false)`, input isn't reset and the game
   doesn't pause.
3. **Import hooks.** The mod replaces a few USER32 imports of `UnityPlayer.dll` in its import table. This keeps the
   fake focus consistent without side effects:
   - `GetForegroundWindow`, `GetActiveWindow` and `GetFocus` report the game window.
   - `ClipCursor` and `SetCursorPos` are ignored while another window is in front, so the mouse stays free.
   - `GetAsyncKeyState` and raw input (`RIDEV_INPUTSINK`) are ignored while another window is in front, so typing
     elsewhere doesn't steer your vehicle.
   - `SetForegroundWindow`, `SetFocus`, `SetWindowPos` and `ShowWindow` are blocked while the game is in the
     background, so it can't steal focus back or minimize itself.

## Building from source

Requirements: Visual Studio 2022 or newer, with the **Desktop development with C++** workload.

```bat
git clone https://github.com/Yukitus86/MoonSweepers-NoFocusLoss.git
cd MoonSweepers-NoFocusLoss
build.bat
```

The output goes to `dist\` (`version.dll`, `NoFocusLoss.ini`, `README.md`).

## Project layout

```
src/NoFocusLoss.c    mod source (focus faking, hooks, version.dll forwarding)
src/version.def      export list of the proxy DLL
src/NoFocusLoss.rc   version resource
NoFocusLoss.ini      default config
build.bat            build script (finds Visual Studio automatically)
```

## License

[MIT](LICENSE). Not affiliated with the developers of Moon Sweepers.
