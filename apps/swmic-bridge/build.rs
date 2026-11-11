// Embeds the Windows exe icon (assets/app.ico) and version info.
fn main() {
    println!("cargo:rerun-if-changed=assets/app.ico");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        let mut res = winresource::WindowsResource::new();
        res.set_icon("assets/app.ico");
        res.set("ProductName", "StopWatch Mic");
        res.set("FileDescription", "StopWatch Mic");
        res.compile().expect("embed Windows resources");
    }
}
