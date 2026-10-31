fn main() {
    println!("cargo:rerun-if-env-changed=YOUYOU_NATIVE_DIR");
    if let Some(dir) = std::env::var_os("YOUYOU_NATIVE_DIR") {
        println!(
            "cargo:rustc-link-search=native={}",
            std::path::Path::new(&dir).display()
        );
    }
    println!("cargo:rustc-link-lib=youyou_core");
    println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../lib/youyou-linux");
}
