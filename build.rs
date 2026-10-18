fn main() {
    println!("cargo:rerun-if-changed=native");
    println!("cargo:rerun-if-changed=translations");
    println!("cargo:rerun-if-env-changed=LRELEASE");
    println!("cargo:rerun-if-changed=assets/fonts");
    println!("cargo:rerun-if-changed=assets/icons");
    println!("cargo:rerun-if-changed=assets/io.github.ranslator.Ranslator.svg");
    println!("cargo:rustc-check-cfg=cfg(core_only)");
    println!("cargo:rerun-if-env-changed=RANSLATOR_CORE_ONLY");
    if std::env::var_os("RANSLATOR_CORE_ONLY").is_some() {
        println!("cargo:rustc-cfg=core_only");
        return;
    }
    println!("cargo:rerun-if-env-changed=RANSLATOR_REQUIRE_LINUX_INTEGRATIONS");
    let mut native = cmake::Config::new("native");
    native.profile("Release");
    if std::env::var_os("RANSLATOR_REQUIRE_LINUX_INTEGRATIONS").is_some() {
        native.define("RANSLATOR_REQUIRE_LINUX_INTEGRATIONS", "ON");
    }
    let dst = native.build();
    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    println!("cargo:rustc-link-lib=static=ranslator_ui");
    // CMake resolves the platform-specific Qt/KDE libraries and writes their absolute paths.
    let links = std::fs::read_to_string(dst.join("lib/link-libraries.txt")).unwrap();
    let macos = std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos");
    let mut framework_directories = std::collections::BTreeSet::new();
    for path in links.lines().filter(|line| !line.is_empty()) {
        println!("cargo:rustc-link-arg={path}");
        if let Some(directory) = path
            .split_once(".framework/")
            .filter(|_| macos)
            .and_then(|(prefix, _)| std::path::Path::new(prefix).parent())
        {
            framework_directories.insert(directory.to_path_buf());
        }
    }
    // cargo run/test must find Homebrew/SDK Qt before macdeployqt relocates it into an .app.
    for directory in framework_directories {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", directory.display());
    }
    if macos {
        println!("cargo:rustc-link-lib=c++");
    } else {
        println!("cargo:rustc-link-lib=stdc++");
    }
}
