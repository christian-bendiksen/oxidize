use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod apply;
mod ctx;
mod install;
mod render;
mod theme;
mod transaction;
mod util;

use ctx::Ctx;
use theme::Theme;
use transaction::Transaction;

use crate::apply::ApplyFlags;

#[derive(Parser)]
#[command(name = "oxidize", about = "Atomic Wayland theme switcher")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Create the oxidize directory scaffold
    Init,

    /// Apply a theme by name
    Set {
        theme: String,
        #[arg(long)]
        skip_apply: bool,
        #[arg(long)]
        skip_gnome: bool,
        #[arg(long)]
        skip_icons: bool,
        #[arg(long)]
        skip_reload: bool,
        #[arg(long)]
        skip_wallpaper: bool,
    },

    /// Install a theme from a git repository and apply it
    Install { url: String },

    /// Reload apps without changing the theme
    Reload,

    /// Apply GNOME color-scheme and gtk-theme for the current theme
    Gnome {
        #[arg(long)]
        skip_icons: bool,
    },

    /// Cycle to the next wallpaper for the current theme
    Wallpaper,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let ctx = Ctx::new().context("initialise context")?;

    match cli.cmd {
        Cmd::Init => cmd_init(&ctx),
        Cmd::Set {
            theme,
            skip_apply,
            skip_gnome,
            skip_icons,
            skip_reload,
            skip_wallpaper,
        } => cmd_set(
            &ctx,
            &theme,
            apply::ApplyFlags {
                skip_apply,
                skip_gnome,
                skip_icons,
                skip_reload,
                skip_wallpaper,
            },
        ),
        Cmd::Install { url } => {
            let name = install::run(&ctx, &url).context("install theme")?;
            cmd_set(&ctx, &name, ApplyFlags::default())
        }
        Cmd::Reload => {
            apply::reload::run(&ctx);
            Ok(())
        }
        Cmd::Gnome { skip_icons } => {
            let theme = Theme::load_current(&ctx)?;
            apply::gnome::run(&theme, skip_icons);
            Ok(())
        }
        Cmd::Wallpaper => {
            let theme = Theme::load_current(&ctx)?;
            apply::wallpaper::run(&ctx, &theme)
        }
    }
}

fn cmd_init(ctx: &Ctx) -> Result<()> {
    let dirs = [
        ("data", &ctx.data_dir),
        ("templates", &ctx.templates_dir),
        ("user-templates", &ctx.user_templates_dir),
        ("generated/live", &ctx.live_dir),
    ];

    for (label, path) in &dirs {
        let existed = path.is_dir();
        std::fs::create_dir_all(path).with_context(|| format!("create {}", path.display()))?;
        let status = if existed { "exists " } else { "created" };
        println!("  {status}  {label:<16} {}", path.display());
    }

    Ok(())
}

fn cmd_set(ctx: &Ctx, theme_name: &str, flags: apply::ApplyFlags) -> Result<()> {
    let theme = Theme::load(&ctx.data_dir, theme_name).context("load theme")?;

    // Stage -> commit (atomic rename).
    let txn = Transaction::begin(ctx).context("begin transaction")?;
    render::engine::render_all(
        &ctx.templates_dir,
        &ctx.user_templates_dir,
        &theme.root,
        txn.stage(),
        &theme.vars,
    )
    .context("render templates")?;
    theme.stage(txn.stage()).context("stage assets")?;
    txn.commit().context("commit transaction")?;

    // Persist theme name outside the atomic tree (intentional).
    std::fs::write(&ctx.current_theme_file, format!("{}\n", theme.name))
        .context("write current.theme")?;

    if flags.skip_apply {
        return Ok(());
    }

    // Apply steps are best-effort: warn on failure, never abort.
    if !flags.skip_gnome {
        apply::gnome::run(&theme, flags.skip_icons);
    }
    if !flags.skip_reload {
        apply::reload::run(ctx);
    }
    if !flags.skip_wallpaper {
        if let Err(e) = apply::wallpaper::run(ctx, &theme) {
            eprintln!("warn: wallpaper apply failed: {e:#}");
        }
    }

    Ok(())
}
