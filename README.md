# oxidize

> Atomic theme switcher for Wayland desktops.

Define a theme once in `colors.toml`, and oxidize renders your application configs from it, atomically swaps them into place.

## Features

- **Atomic publishing.** Staged renders are promoted via `renameat2(RENAME_EXCHANGE)` — no half-written configs on failure.
- **One palette, many apps.** A single `colors.toml` feeds kitty, waybar, mako, fuzzel, Hyprland, Niri, and anything else you template.
- **Three-tier overrides.** `user-templates/` > `data/<theme>/` > `templates/`. Personal tweaks never get overwritten.
- **WM-native border detection.** Parses the theme's shipped Hyprland / Niri / Mango config to derive active and inactive border colors automatically.
- **GNOME integration.** Sets `color-scheme`, `gtk-theme`, and `icon-theme` via `gsettings`.
- **Wallpaper cycling** through the theme's `backgrounds/`.
- **Git-based install.** Clone and apply a theme in one command.

## Quick start

```sh
oxidize init
oxidize install https://github.com/user/my-theme
oxidize set tokyo-night
oxidize list
```

## How it works

`oxidize set <theme>`:

1. Loads `data/<theme>/colors.toml` into a flat variable map.
2. Derives `{{ <name> }}`, `{{ <name>_strip }}`, and `{{ <name>_rgb }}` for every color.
3. Resolves border colors from the theme's WM config, with palette fallbacks.
4. Renders everything into a staging directory (`user-templates/` > `data/<theme>/` > `templates/`).
5. Atomically swaps `stage ↔ live/` via a single rename syscall.
6. Applies GNOME settings, cycles the wallpaper, and signals running apps.

## Directory layout

```
~/.config/oxidize/themes/
├── data/<theme>/      # colors.toml + optional verbatim files + backgrounds/
├── templates/         # shipped *.tpl files
├── user-templates/    # local *.tpl overrides (highest precedence)
├── generated/live/    # the published render — apps read from here
├── current            # symlink → generated/live/
└── current.theme      # active theme name
```

## colors.toml

```toml
accent     = "#56949f"
foreground = "#575279"
background = "#faf4ed"

color0  = "#f2e9e1"
# ... through color15
```

Every color produces three template variables:

| Variable | Value |
|---|---|
| `{{ foreground }}` | `#575279` |
| `{{ foreground_strip }}` | `575279` |
| `{{ foreground_rgb }}` | `87,82,121` |

Oxidize additionally derives `{{ border_active }}` and `{{ border_inactive }}` (plus `_strip` / `_rgb` variants) from the theme's Hyprland / Niri / Mango config, with palette fallbacks to `accent` and `color8`.

## Template syntax

Minimal `{{ name }}` substitution. Unknown variables stay literal so partial renders are inspectable:

```
# kitty.conf.tpl
background   {{ background }}
color4       {{ color4 }}
```

## Commands

| Command | Description |
|---|---|
| `init` | Create the theme scaffold. |
| `set <theme>` | Render and apply a theme. `--skip-{apply,gnome,icons,reload,wallpaper}` available. |
| `install <url>` | Git-clone a theme and apply it. |
| `list` | List installed themes, marking the current one. |
| `current` | Print the current theme name. |
| `remove <theme>` | Delete a theme. `--yes` skips confirmation; `--force` allows removing the active theme. |
| `update [theme]` | `git pull --ff-only` one theme or all git-installed themes. |
| `reload` | Reload apps without changing the theme. |
| `gnome` | Apply GNOME settings for the current theme. |
| `wallpaper` | Cycle to the next wallpaper. |

## Installation

### Full stack (oxidize + dotfiles + themes)

The companion [`oxidize-dotfiles`](https://github.com/christian-bendiksen/oxidize-dotfiles) ships a one-shot installer that builds the oxidize binary, symlinks ready-made templates and themes into `~/.config/oxidize/`, and wires per-app configs (kitty, waybar, niri, mango, mako, helix, gtk, ...) into `~/.config/`.

```sh
git clone https://github.com/christian-bendiksen/oxidize-dotfiles ~/oxidize-dotfiles
bash ~/oxidize-dotfiles/install.sh
```

The installer targets **AerynOS** (uses `moss` for system packages). On other distros, skim `install.sh` and install the equivalents from your own package manager before running it — the rest (oxidize build, symlinks, theme setup) is distro-agnostic.

### Binary only

```sh
git clone https://github.com/christian-bendiksen/oxidize
cd oxidize
cargo install --path .
```

## License

MIT.
