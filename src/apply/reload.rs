use crate::ctx::Ctx;
use std::process::{Command, Stdio};

/// Reload waybar, mako, kitty, btop
pub fn run(ctx: &Ctx) {
    pkill_signal("waybar", "SIGUSR2");

    detach(Command::new("makoctl").arg("reload"));

    pkill_signal("btop", "SIGUSR2");
    pkill_signal("kitty", "SIGUSR1");
    pkill_signal("ghostty", "SIGUSR1");
    pkill_signal("hx", "SIGUSR1");
    reload_alacritty(ctx);
    reload_mango();
    restart_swayosd();
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

fn reload_mango() {
    detach(Command::new("mmsg").args(["-d", "reload_config"]));
}

fn restart_swayosd() {
    Command::new("pkill")
        .args(["-x", "swayosd-server"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .ok();
    detach(&mut Command::new("swayosd-server"));
}
