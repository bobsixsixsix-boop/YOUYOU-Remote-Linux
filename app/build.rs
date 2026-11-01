// libyouyou_core.so comes from the CMake build of core/ (YY_CORE_LIB_DIR) and
// is installed next to the binary, hence the $ORIGIN rpath.
fn main() {
    let dir = std::env::var("YY_CORE_LIB_DIR").unwrap_or_else(|_| "../build".into());
    println!("cargo:rerun-if-env-changed=YY_CORE_LIB_DIR");
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=dylib=youyou_core");
    println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
}
