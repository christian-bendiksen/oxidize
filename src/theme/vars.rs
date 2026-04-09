use anyhow::{Context, Result};
use std::{collections::HashMap, fs, path::Path};

pub fn build_vars_from_colors(colors_file: &Path) -> Result<HashMap<String, String>> {
    let src = fs::read_to_string(colors_file)
        .with_context(|| format!("read {}", colors_file.display()))?;

    let table: toml::Value = toml::from_str(&src).context("parse colors.toml")?;

    let mut vars = HashMap::new();
    flatten("", &table, &mut vars);

    // Collect derived keys separately to avoid a borrow conflict on `vars`.
    let derived: Vec<(String, String)> = vars
        .iter()
        .filter(|(_, v)| v.starts_with('#'))
        .flat_map(|(k, v)| derive_color_keys(k, v))
        .collect();

    vars.extend(derived);
    Ok(vars)
}

/// Flatten a TOML value into `prefix_key = string` pairs.
fn flatten(prefix: &str, value: &toml::Value, out: &mut HashMap<String, String>) {
    match value {
        toml::Value::Table(map) => {
            for (k, v) in map {
                let key = if prefix.is_empty() {
                    k.to_owned()
                } else {
                    format!("{prefix}_{k}")
                };
                flatten(&key, v, out);
            }
        }
        toml::Value::String(s) => {
            out.insert(prefix.to_owned(), s.clone());
        }
        toml::Value::Integer(i) => {
            out.insert(prefix.to_owned(), i.to_string());
        }
        toml::Value::Float(f) => {
            out.insert(prefix.to_owned(), f.to_string());
        }
        toml::Value::Boolean(b) => {
            out.insert(prefix.to_owned(), b.to_string());
        }
        // Arrays and datetimes are not used in color files, silently ignore.
        _ => {}
    }
}

/// Produce `<key>_strip` and `<key>_rgb` entries from a `#rrggbb` value.
fn derive_color_keys(key: &str, hex: &str) -> impl Iterator<Item = (String, String)> {
    let bare = hex.trim_start_matches('#');
    let rgb = hex_to_rgb(bare).map(|r| (format!("{key}_rgb"), r));
    let strip = (format!("{key}_strip"), bare.to_owned());
    std::iter::once(strip).chain(rgb)
}

/// Convert a bare 6-character hex string to `"r,g,b"`.
fn hex_to_rgb(hex: &str) -> Option<String> {
    if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let r = u8::from_str_radix(&hex[0..2], 16).ok()?;
    let g = u8::from_str_radix(&hex[2..4], 16).ok()?;
    let b = u8::from_str_radix(&hex[4..6], 16).ok()?;
    Some(format!("{r},{g},{b}"))
}

pub fn insert_border_active(vars: &mut HashMap<String, String>, color: &str) {
    vars.insert("border_active".to_owned(), color.to_owned());
    vars.extend(derive_color_keys("border_active", color));
}
