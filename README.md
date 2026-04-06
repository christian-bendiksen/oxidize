# oxidize-theme

> WIP

Atomic Wayland theme switcher. Renders config files from color definitions, reloads kitty/waybar/mako/btop, cycles wallpapers, and applies GNOME settings — all in one command.

## Theme structure

```
~/.config/oxidize/themes/
├── data/
│   └── <theme-name>/
│       ├── colors.toml        # required
│       ├── light.mode         # optional: marks theme as light
│       ├── icons.theme        # optional: icon theme name
│       ├── backgrounds/       # optional: wallpaper images
│       └── kitty.conf         # optional: any config file, used verbatim
├── templates/                 # *.tpl files, rendered from colors.toml
├── user-templates/            # *.tpl overrides (win over templates/)
└── generated/
    └── live/                  # active rendered output (symlinked as current/)
```

## Configuration files

Theme config files are resolved in this order — first match wins:

```
user-templates/<file>.tpl   rendered, always wins
data/<theme>/<file>         copied verbatim if present
templates/<file>.tpl        rendered as fallback
```

Place a `kitty.conf` (or any config file) directly in the theme directory to use it as-is. Templates only fill in what the theme doesn't provide.

## colors.toml

```toml
[palette]
bg = "#1e1e2e"
fg = "#cdd6f4"
```

Every color produces three template variables:

| Variable | Value |
|---|---|
| `palette_bg` | `#1e1e2e` |
| `palette_bg_strip` | `1e1e2e` |
| `palette_bg_rgb` | `30,30,46` |

Use them in templates as `{{ palette_bg }}`.
