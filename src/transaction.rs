//! Atomic publish via a temp-dir -> rename protocol.
use crate::{ctx::Ctx, util};
use anyhow::{Context, Result};
use rustix::fs::{CWD, RenameFlags, renameat_with};
use std::{
    fs,
    path::{Path, PathBuf},
};
use tempfile::{Builder, TempDir};

pub struct Transaction {
    stage: TempDir,
    live: PathBuf,
    link: PathBuf,
}

impl Transaction {
    /// Create a fresh staging directory inside `generated/`.
    pub fn begin(ctx: &Ctx) -> Result<Self> {
        fs::create_dir_all(&ctx.generated_dir).context("create generated dir")?;

        let stage = Builder::new()
            .prefix(".stage.")
            .tempdir_in(&ctx.generated_dir)
            .context("create staging dir")?;

        Ok(Self {
            stage,
            live: ctx.live_dir.clone(),
            link: ctx.current_link.clone(),
        })
    }

    /// Path callers write rendered files into.
    #[inline]
    pub fn stage(&self) -> &Path {
        self.stage.path()
    }

    /// Atomically replace `live/` with the staged tree, then update the
    /// `current` symlink.
    pub fn commit(self) -> Result<()> {
        // Surrender TempDir ownership before any fallible operations.
        // This ensures the staging dir is never deleted by Drop on any
        // error path that follows.
        let stage_path = self.stage.keep();

        if fs::symlink_metadata(&self.live).is_ok() {
            renameat_with(CWD, &stage_path, CWD, &self.live, RenameFlags::EXCHANGE)
                .context("atomic exchange stage <-> live")?;
            // remove the displaced old tree
            fs::remove_dir_all(&stage_path).context("remove old live dir")?;
        } else {
            // live/ does not exist yet, plain rename is sufficient.
            fs::rename(&stage_path, &self.live).context("rename stage -> live")?;
        }

        util::symlink_force(&self.live, &self.link).context("update current symlink")
    }
}
