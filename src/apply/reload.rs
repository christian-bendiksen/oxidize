use crate::ctx::Ctx;
use std::{
    fs, io,
    process::{Command, Stdio},
    time::{SystemTime, UNIX_EPOCH},
};

/// Reload waybar, mako, kitty, btop
pub fn run(ctx: &Ctx) {
    reload_waybar(ctx);

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

fn reload_waybar(ctx: &Ctx) {
    if let Err(err) = write_waybar_header(ctx) {
        eprintln!("warn: failed to update waybar CSS: {err}");
    }
}

const OX_BEGIN: &str = "/* oxidize:begin */";
const OX_END: &str = "/* oxidize:end */";

fn write_waybar_header(ctx: &Ctx) -> io::Result<()> {
    let dest = ctx.config_home.join("waybar/style.css");
    let src = ctx.current_link.join("waybar.css");

    if !src.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("missing generated waybar CSS: {}", src.display()),
        ));
    }

    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)?;
    }

    let existing = fs::read_to_string(&dest).unwrap_or_default();
    let user_css = strip_oxidize_header(&existing);

    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);

    let header = format!(
        "{OX_BEGIN}\n\
         /* rev:{ts} */\n\
         @import url(\"../oxidize/themes/current/waybar.css\");\n\
         {OX_END}\n"
    );

    fs::write(&dest, format!("{header}\n{user_css}"))
}

fn strip_oxidize_header<'a>(s: &'a str) -> &'a str {
    let Some(rest) = s.strip_prefix(OX_BEGIN) else {
        return s;
    };
    let Some(pos) = rest.find(OX_END) else {
        return s;
    };
    rest[pos + OX_END.len()..].trim_start_matches('\n')
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
