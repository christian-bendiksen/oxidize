use crate::ctx::Ctx;
use std::process::{Command, Stdio};

/// Reload waybar, mako, kitty, btop, and XDG portals.
pub fn run(ctx: &Ctx) {
    pkill_signal("waybar", "SIGUSR2");

    detach(Command::new("makoctl").arg("reload"));

    // Use spawn (not status), systemctl can block on D-Bus activation.
    for service in [
        "xdg-desktop-portal.service",
        "xdg-desktop-portal-gtk.service",
    ] {
        detach(Command::new("systemctl").args(["--user", "restart", service]));
    }
    pkill_signal("btop", "SIGUSR2");
    pkill_signal("kitty", "SIGUSR1");
    pkill_signal("ghostty", "SIGUSR1");
    reload_alacritty(ctx);
}

fn pkill_signal(name: &str, signal: &str) {
    let flag = format!("-{signal}");
    detach(Command::new("pkill").args([flag.as_str(), name]));
}

fn detach(cmd: &mut Command) {
    cmd.stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .ok();
}

fn reload_alacritty(ctx: &Ctx) {
    let conf = ctx
        .config_dir
        .parent()
        .unwrap_or(&ctx.config_dir)
        .join("alacritty/alacritty.toml");
    if conf.exists() {
        detach(Command::new("touch").arg(conf));
    }
}
