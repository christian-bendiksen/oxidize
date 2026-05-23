mod config;
mod dbus;
mod style;
mod symbols;

use std::{ffi::c_void, time::Duration};

const ATTACH_RETRIES: u32 = 50;
const ATTACH_RETRY: Duration = Duration::from_millis(100);

#[macro_export]
macro_rules! ox_log {
    ($($arg:tt)*) => {
        if $crate::config::debug_enabled() {
            eprintln!("[oxidize-gio] {}", format_args!($($arg)*));
        }
    };
}

/// Called by GIO when the module is loaded into a process.
///
/// # Safety
/// `module` is provided by GIO and must be either null or a valid `GTypeModule *`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn g_io_module_load(module: *mut c_void) {
    if config::disabled() {
        return;
    }

    if !module.is_null() {
        unsafe extern "C" {
            fn g_type_module_use(module: *mut c_void) -> i32;
        }
        unsafe { g_type_module_use(module) };
    }

    ox_log!(
        "module loaded pid={} version={}",
        std::process::id(),
        config::VERSION
    );

    glib::idle_add_local_once(|| attach_or_retry(0));
}

/// Called by GIO during module teardown.
///
/// # Safety
/// GIO calls this during process/module shutdown. No assumptions are made about
/// the module pointer here because the module state is process-local.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn g_io_module_unload(_module: *mut c_void) {
    dbus::unsubscribe();
    style::cleanup();
    ox_log!("module unloaded version={}", config::VERSION);
}

/// Called by GIO to query module capabilities.
///
/// # Safety
/// The returned null-terminated array is static and process-lifetime valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn g_io_module_query() -> *mut *mut i8 {
    // usize instead of *const i8 so the array is Sync and can live in a static.
    static FEATURES: [usize; 1] = [0];
    FEATURES.as_ptr().cast::<*mut i8>().cast_mut()
}

fn attach_or_retry(attempt: u32) {
    let Some((major, gtk)) = symbols::detected_gtk() else {
        retry_or_skip(attempt, "GTK symbols not ready");
        return;
    };

    let Some(gtk) = gtk else {
        ox_log!("unsupported GTK major version {major}; skipping");
        return;
    };

    ox_log!(
        "attach {gtk} display={} version={}",
        std::env::var("WAYLAND_DISPLAY").unwrap_or_default(),
        config::VERSION,
    );

    style::attach(gtk);
    dbus::subscribe();
}

fn retry_or_skip(attempt: u32, reason: &'static str) {
    if attempt >= ATTACH_RETRIES {
        ox_log!("{reason}; skipping");
        return;
    }

    ox_log!("{reason}; retry {attempt}/{ATTACH_RETRIES}");
    glib::timeout_add_local(ATTACH_RETRY, move || {
        attach_or_retry(attempt + 1);
        glib::ControlFlow::Break
    });
}
