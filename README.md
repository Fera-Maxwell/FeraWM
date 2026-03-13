# FeraWM

A minimal X11 window manager written in C. Every app gets a **bone** — a container window that holds and manages it.

Built as part of the **FeraDE** project.

---

## Building

```sh
git clone https://github.com/Fera-Maxwell/FeraWM.git
cd FeraWM
make
sudo make install
```

The binary installs to `/usr/local/bin/ferawm`. A session entry is placed in `/usr/share/xsessions/ferawm.desktop` for display managers like LightDM.

---

## Config

Config is loaded from `~/.config/fera/ferawm.conf`, falling back to `/etc/fera/ferawm.conf`.

Changes are applied automatically — FeraWM watches the config file and hot-reloads on save. No restart needed for most changes.

---

## Config Reference

### Variables

Variables start with `$` and can be reused anywhere in the config after they are defined.

```ini
$m = super
$s = shift
$c = ctrl
```

---

### general { }

| Key | Type | Default | Description |
|---|---|---|---|
| `bone_color` | hex | `7c6af7` | Border color of the focused window |
| `bone_unfocus` | hex | `2a2a3a` | Border color of unfocused windows |
| `bone_gap` | int | `20` | Gap from screen edges in pixels |
| `inner_gap` | int | `10` | Gap between tiled windows in pixels |
| `border_size` | int int | `4 4` | Border padding X and Y in pixels |
| `tile_mode` | 0/1 | `0` | Start with tiling mode on |
| `mouse_focus` | true/false | `false` | Focus follows mouse. When enabled, keyboard focus changes also warp the cursor to the focused window |
| `animate_tab` | 0–3 | `1` | Window preview style when modifier is held: `0` = snap only, `1` = live resize, `2` = ghost (outline only), `3` = pixel |

---

### input { }

| Key | Type | Default | Description |
|---|---|---|---|
| `modifier` | name | `super` | Main modifier key. One of: `super` `alt` `ctrl` `shift` |
| `xkb_layout` | string | `us` | Keyboard layout passed to `setxkbmap` |
| `xkb_options` | string | _(empty)_ | Extra xkb options, e.g. `ctrl:nocaps` |

#### input > mouse { }

| Key | Type | Default | Description |
|---|---|---|---|
| `natural_scroll` | true/false | `false` | Reverse mouse scroll direction |
| `sensitivity` | float | `0.0` | Pointer speed from `-1.0` to `1.0` |
| `accel_profile` | string | `flat` | `flat` or `adaptive` |

#### input > touchpad { }

| Key | Type | Default | Description |
|---|---|---|---|
| `natural_scroll` | true/false | `true` | Reverse touchpad scroll direction |
| `sensitivity` | float | `0.5` | Pointer speed from `-1.0` to `1.0` |
| `accel_profile` | string | `flat` | `flat` or `adaptive` |

---

### autostart { }

Programs to launch on startup.

| Key | Behavior |
|---|---|
| `once` | Spawns the program only if it is not already running |
| `restart` | Kills and respawns the program on every FeraWM restart |

Multiple programs are comma-separated:

```ini
autostart {
    once    = steam -silent, vesktop
    restart = picom, feh --bg-scale ~/Pictures/wallpapers/background.jpg
}
```

---

### keybind

```ini
keybind = <combo> = <action>
keybind = <combo> = spawn = <command>
keybind = <combo> = ws_switch = <1-4>
keybind = <combo> = ws_move   = <1-4>
```

Combos are `+` separated. Use variable names or modifier names directly.

Available modifier tokens: `modifier`, `super`, `alt`, `ctrl`, `shift`

Available key names: `return`, `tab`, `space`, `escape`, `left`, `right`, `up`, `down`, any single letter, or any XKeysym name.

#### Actions

| Action | Description |
|---|---|
| `kill_window` | Close the focused window gracefully |
| `kill_wm` | Exit FeraWM |
| `fullscreen` | Toggle fullscreen on the focused window |
| `cycle_focus` | Cycle focus through windows on the current workspace |
| `reload` | Reload the config file |
| `restart` | Fully restart FeraWM and rerun `autostart_restart` programs |
| `spawn = <cmd>` | Run a shell command |
| `ws_switch = <n>` | Switch to workspace n (1–4) |
| `ws_move = <n>` | Move focused window to workspace n (1–4) |
| `tile_toggle` | Toggle tiling mode on/off |
| `tile_float` | Toggle the focused window between floating and tiled |
| `tile_left` | Swap focused window left in the tile grid |
| `tile_right` | Swap focused window right in the tile grid |
| `tile_up` | Swap focused window up in the tile grid |
| `tile_down` | Swap focused window down in the tile grid |
| `focus_left` | Move focus left without swapping |
| `focus_right` | Move focus right without swapping |
| `focus_up` | Move focus up without swapping |
| `focus_down` | Move focus down without swapping |

---

## Tiling

Tiling can be toggled at runtime with `tile_toggle`. When enabled, windows are arranged in equal columns up to 3 per row. New windows are appended to the end. Removing a window reflows the grid automatically.

Windows can be dragged between cells while the modifier is held — releasing near a cell snaps them into it.

A window can be floated individually with `tile_float` while keeping the rest tiled.

---

## Workspaces

FeraWM has 4 workspaces. Windows on inactive workspaces are parked offscreen and restored when switching back.

---

## Mouse Controls

With the modifier held:

- **Left click + drag** — move window
- **Right click + drag** — resize window

---

## Command Line

```
ferawm -v        print version and exit
ferawm -V        verbose logging
```

---

## Panels and Bars

FeraWM reads `_NET_WM_STRUT` and `_NET_WM_STRUT_PARTIAL` from all windows. Panels that set these properties correctly will automatically reserve screen space and tiled windows will not overlap them.

Dock-type windows (`_NET_WM_WINDOW_TYPE_DOCK`) are mapped directly without being managed, and are always raised above other windows.

---

## Version

`0.2.2`

Part of the **FeraDE** project — [github.com/Fera-Maxwell/FeraWM](https://github.com/Fera-Maxwell/FeraWM)
