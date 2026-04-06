use anyhow::{Context, Result};
use std::{fs, path::Path};

/// Create (or replace) a Unix symlink atomically.
///
/// Ensures parent directories exist, removes any existing entry at `link`,
/// then creates `link -> target`.
#[cfg(unix)]
pub fn symlink_force(target: &Path, link: &Path) -> Result<()> {
    use std::os::unix::fs::symlink;

    if let Some(parent) = link.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create parent dir {}", parent.display()))?;
    }

    // Write the symlink to a sibling temp name first.
    let tmp = link.with_extension("symlink-tmp");
    symlink(target, &tmp).with_context(|| format!("create temp symlink {}", tmp.display()))?;

    // rename(2) atomically replaces 'link' (even if it already exists)
    fs::rename(&tmp, link)
        .with_context(|| format!("atomic rename symlink into place {}", link.display()))
}

#[cfg(not(unix))]
pub fn symlink_force(_target: &Path, _link: &Path) -> Result<()> {
    anyhow::bail!("symlinks are not supported on this platform")
}
