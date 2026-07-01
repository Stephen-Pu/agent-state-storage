//! Locate the sibling `libkvcache` shared library and wire the linker.
//!
//! Search order, mirroring `kvcache_core._ffi` on the Python side:
//!   1. `$KVCACHE_LIB` — a file path (use its parent dir) or a directory.
//!   2. Walk up from this crate to the repo root, looking for a built
//!      `build/core-abi/` or `build-zstd/core-abi/` containing the dylib/so.
//!
//! We emit an rpath as well as a link-search so `cargo test` binaries resolve
//! the dylib at run time without the caller having to set DYLD/LD_LIBRARY_PATH.

use std::env;
use std::path::PathBuf;

fn main() {
    let dir = locate_lib_dir();
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=dylib=kvcache");
    // rpath so the test/bench binaries find libkvcache at runtime.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
    // Rerun if the caller repoints the library.
    println!("cargo:rerun-if-env-changed=KVCACHE_LIB");
}

fn locate_lib_dir() -> PathBuf {
    const NAMES: [&str; 2] = ["libkvcache.dylib", "libkvcache.so"];

    if let Ok(p) = env::var("KVCACHE_LIB") {
        let pb = PathBuf::from(&p);
        if pb.is_file() {
            if let Some(parent) = pb.parent() {
                return parent.to_path_buf();
            }
        }
        if pb.is_dir() {
            return pb;
        }
    }

    // Walk up from the crate manifest dir toward the repo root.
    let mut d = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    loop {
        for build in ["build/core-abi", "build-zstd/core-abi"] {
            let base = d.join(build);
            for name in NAMES {
                if base.join(name).is_file() {
                    return base;
                }
            }
        }
        if !d.pop() {
            break;
        }
    }

    panic!(
        "libkvcache not found. Build the C/C++ tree first \
         (cmake --build build --target kvcache) or set KVCACHE_LIB to the \
         .dylib/.so path."
    );
}
