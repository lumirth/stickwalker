fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        winresource::WindowsResource::new()
            .set_icon("assets/icon.ico")
            .set("ProductName", "Stickwalker")
            .set("FileDescription", "Stickwalker device utility")
            .compile()
            .expect("embed Stickwalker icon");
    }
    println!("cargo:rerun-if-changed=assets/icon.ico");
}
