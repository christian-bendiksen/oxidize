use std::{fs, path::Path};

#[derive(Clone, Copy)]
enum Wm {
    Hyprland,
    Niri,
    Mango,
}

const ALL: &[Wm] = &[Wm::Hyprland, Wm::Niri, Wm::Mango];

impl Wm {
    fn filename(self) -> &'static str {
        match self {
            Self::Hyprland => "hyprland.conf",
            Self::Niri => "niri-colors.kdl",
            Self::Mango => "mango.conf",
        }
    }

    fn parse_border(self, conf: &str) -> Option<String> {
        match self {
            Self::Hyprland => parse_hyprland(conf),
            Self::Niri => parse_niri(conf),
            Self::Mango => parse_mango(conf),
        }
    }
}

pub fn resolve(theme_root: &Path) -> Option<String> {
    ALL.iter().find_map(|&wm| {
        fs::read_to_string(theme_root.join(wm.filename()))
            .ok()
            .and_then(|s| wm.parse_border(&s))
    })
}

fn parse_hyprland(conf: &str) -> Option<String> {
    conf.lines()
        .map(str::trim)
        .filter(|l| !l.starts_with('#'))
        .find_map(|line| {
            let rest = line
                .strip_prefix("$activeBorderColor")
                .or_else(|| line.strip_prefix("col.active_border"))?;
            let val = rest
                .trim_start_matches(|c: char| c.is_ascii_whitespace() || c == '=')
                .trim();
            (!val.starts_with('$')).then(|| super::colors::normalize_color(val))
        })
}

fn parse_niri(conf: &str) -> Option<String> {
    conf.lines().map(str::trim).find_map(|line| {
        let color = line
            .strip_prefix("active-color")?
            .trim()
            .trim_matches('"')
            .trim_matches('\'');
        (!color.is_empty()).then(|| super::colors::normalize_color(color))
    })
}

fn parse_mango(conf: &str) -> Option<String> {
    conf.lines().map(str::trim).find_map(|line| {
        let rest = line
            .strip_prefix("focuscolor")?
            .trim_start_matches(|c: char| c.is_ascii_whitespace() || c == '=')
            .trim();
        let hex8 = rest.strip_prefix("0x")?;
        (hex8.len() == 8).then(|| format!("#{}", &hex8[..6]))
    })
}
