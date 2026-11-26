use shinki_emu_build::{
    android_ndk, configure_cmake, generate_import_library, inputs, link_apple_static_core, vcpkg,
    write_core_build_manifest, BuildAsset, CoreBuildManifest,
};
use std::{
    env,
    error::Error,
    fs,
    path::{Path, PathBuf},
};

type Result<T, E = Box<dyn Error>> = std::result::Result<T, E>;

fn require_file(path: &Path) -> Result<()> {
    if !path.is_file() {
        return Err(format!("missing Impacto build prerequisite: {}", path.display()).into());
    }
    Ok(())
}

fn copy_tree(source: &Path, dest: &Path) -> Result<()> {
    fs::create_dir_all(dest)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        let output = dest.join(entry.file_name());
        if kind.is_dir() {
            copy_tree(&entry.path(), &output)?;
        } else if kind.is_file() {
            fs::copy(entry.path(), output)?;
        } else {
            return Err(format!(
                "dependency source contains a link or special file: {}",
                entry.path().display()
            )
            .into());
        }
    }
    Ok(())
}

fn build() -> Result<()> {
    for name in [
        "ANDROID_NDK_HOME",
        "NUM_JOBS",
        "PROFILE",
        "HOST",
        "TARGET",
    ] {
        println!("cargo:rerun-if-env-changed={name}");
    }
    let target = env::var("TARGET")?;
    let android = target.ends_with("-linux-android");
    let host = env::var("HOST")?;
    let supported = matches!(
        target.as_str(),
        "x86_64-pc-windows-msvc"
            | "x86_64-unknown-linux-gnu"
            | "x86_64-apple-darwin"
            | "aarch64-apple-darwin"
            | "aarch64-apple-ios"
            | "aarch64-apple-ios-sim"
            | "x86_64-apple-ios"
            | "aarch64-linux-android"
            | "x86_64-linux-android"
    );
    if !supported {
        return Err(format!("Impacto's adapter does not support HOST={host}, TARGET={target}").into());
    }
    if cfg!(feature = "static") && android {
        return Err("The Android Impacto core must be built as a shared library".into());
    }
    // configure_cmake reads NUM_JOBS. Cap the native build even when Cargo has
    // been started outside xtask with a higher job count.
    let jobs = env::var("NUM_JOBS")
        .ok()
        .and_then(|v| v.parse::<usize>().ok())
        .unwrap_or(4)
        .clamp(1, 4);
    unsafe { env::set_var("NUM_JOBS", jobs.to_string()) };

    let adapter = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").ok_or("CARGO_MANIFEST_DIR is required")?);
    let source = std::path::absolute(adapter.parent().ok_or("adapter must be in the Impacto checkout")?)?;
    // Use the product subtitle manifest's libass ABI on Android even though
    // Impacto's manifest uses an older vcpkg baseline.
    let overlays: &[_] = if android {
        &[vcpkg::PortOverlay::Vcpkg("ports/libass")]
    } else {
        &[]
    };
    let installation = vcpkg::install_core_with_overlays(&source, "impacto", overlays)?;
    let installed = installation.install_root.join("installed");
    let triplet = installation.triplet;
    let vcpkg = installation.root;
    let toolchain = vcpkg.join("scripts/buildsystems/vcpkg.cmake");
    require_file(&toolchain)?;
    println!("cargo:rerun-if-changed={}", toolchain.display());
    for path in ["include", "lib", "bin", "debug/lib", "debug/bin", "share"] {
        let input = installed.join(&triplet).join(path);
        if input.exists() {
            println!("cargo:rerun-if-changed={}", input.display());
        }
    }
    let share = installed.join(&triplet).join("share");
    for port in ["sdl3", "avcpp", "openal-soft", "libvorbis", "libwebp", "freetype", "harfbuzz"] {
        require_file(&share.join(port).join("copyright"))?;
    }
    require_file(&share.join("ffmpeg").join("copyright"))?;
    let (atrac_source, magic) = inputs::impacto_sources()?;
    require_file(&atrac_source.join("C/src/libatrac9.c"))?;
    require_file(&magic.join("CMakeLists.txt"))?;
    require_file(&magic.join("include/magic_enum/magic_enum.hpp"))?;
    println!("cargo:rerun-if-changed={}", atrac_source.display());
    println!("cargo:rerun-if-changed={}", magic.display());
    let atrac_patch = source.join("vendor/patches/LibAtrac9/CMakeLists.txt");
    require_file(&atrac_patch)?;
    println!("cargo:rerun-if-changed={}", atrac_patch.display());
    let atrac = PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is required")?)
        .join("src/libatrac9");
    if atrac.exists() {
        fs::remove_dir_all(&atrac)?;
    }
    copy_tree(&atrac_source, &atrac)?;
    fs::copy(atrac_patch, atrac.join("CMakeLists.txt"))?;

    let mut definitions = vec![
        (
            "CMAKE_TOOLCHAIN_FILE".into(),
            toolchain.to_string_lossy().into_owned(),
        ),
        ("VCPKG_INSTALLED_DIR".into(), installed.to_string_lossy().into_owned()),
        ("VCPKG_TARGET_TRIPLET".into(), triplet.clone()),
        ("IMPACTO_SHINKI_EMU".into(), "ON".into()),
        (
            "IMPACTO_SHINKI_EMU_STATIC".into(),
            if cfg!(feature = "static") { "ON" } else { "OFF" }.into(),
        ),
        (
            "SHINKI_EMU_ABI_INCLUDE_DIR".into(),
            source.join("vendor/shinki-emu-abi/include").to_string_lossy().into_owned(),
        ),
        (
            "FETCHCONTENT_SOURCE_DIR_LIBATRAC9".into(),
            atrac.to_string_lossy().into_owned(),
        ),
        (
            "FETCHCONTENT_SOURCE_DIR_MAGIC_ENUM".into(),
            magic.to_string_lossy().into_owned(),
        ),
        ("CMAKE_INSTALL_BINDIR".into(), ".".into()),
        ("CMAKE_INSTALL_LIBDIR".into(), "lib".into()),
        ("CMAKE_INSTALL_INCLUDEDIR".into(), "include".into()),
    ];
    if target.ends_with("-linux-android") {
        let ndk = android_ndk()?;
        let android_toolchain = ndk.join("build/cmake/android.toolchain.cmake");
        require_file(&android_toolchain)?;
        println!("cargo:rerun-if-changed={}", android_toolchain.display());
        let abi = match target.as_str() {
            "aarch64-linux-android" => "arm64-v8a",
            "x86_64-linux-android" => "x86_64",
            _ => unreachable!(),
        };
        // vcpkg remains the primary toolchain so its target package wrappers
        // are available. Chainload the NDK before CMake selects any compiler.
        definitions.extend([
            ("VCPKG_CHAINLOAD_TOOLCHAIN_FILE".into(), android_toolchain.to_string_lossy().into_owned()),
            ("ANDROID_ABI".into(), abi.into()),
            ("ANDROID_PLATFORM".into(), "android-28".into()),
            ("ANDROID_STL".into(), "c++_shared".into()),
            ("CMAKE_POSITION_INDEPENDENT_CODE".into(), "ON".into()),
        ]);
    }
    let native = configure_cmake(&source, &definitions, &["install"])?;
    let stage = native.join("install");
    let library = if cfg!(feature = "static") {
        let name = if target == "x86_64-pc-windows-msvc" {
            "shinki_emu_impacto.lib"
        } else {
            "libshinki_emu_impacto.a"
        };
        stage.join("lib").join(name)
    } else {
        let name = if target == "x86_64-pc-windows-msvc" {
            "shinki_emu_impacto.dll"
        } else if target.ends_with("-linux-android") {
            "libshinki_emu_impacto.so"
        } else if target == "x86_64-unknown-linux-gnu" {
            "shinki_emu_impacto.so"
        } else {
            "shinki_emu_impacto.dylib"
        };
        stage.join(name)
    };
    require_file(&library)?;
    let resources = stage.join("Resources");
    for path in ["profiles/cc/game.lua", "src/shaders", "resources"] {
        if !resources.join(path).exists() {
            return Err(format!("native Impacto install omitted Resources/{path}").into());
        }
    }
    println!("cargo:rustc-link-search=native={}", stage.join("lib").display());
    let library = if cfg!(feature = "static") && target.contains("apple") {
        link_apple_static_core("impacto", &native, &[installed.join(&triplet)])?
    } else if cfg!(feature = "static") {
        println!("cargo:rustc-link-lib=static=shinki_emu_impacto");
        library
    } else {
        println!("cargo:rustc-link-search=native={}", stage.display());
        if target == "x86_64-pc-windows-msvc" {
            let import = generate_import_library(&library, &native.join("imports"), &target)?;
            println!(
                "cargo:rustc-link-search=native={}",
                import.parent().ok_or("import library parent")?.display()
            );
        }
        println!("cargo:rustc-link-lib=dylib=shinki_emu_impacto");
        library
    };

    let mut manifest = CoreBuildManifest::new(
        "impacto", if cfg!(feature = "static") { "native-static" } else { "native-shared" },
    )?;
    manifest.native_library = Some(library);
    manifest.resources.push(BuildAsset {
        source: resources,
        dest: "Resources".into(),
    });
    manifest.licenses.push(BuildAsset {
        source: source.join("LICENSE"),
        dest: "licenses/impacto/LICENSE".into(),
    });
    manifest.licenses.push(BuildAsset {
        source: source.join("THIRDPARTY.md"),
        dest: "licenses/impacto/THIRDPARTY.md".into(),
    });
    for (path, name) in [
        ("README.md", "README.md"),
        ("licenses/NotoSansCJK-OFL.txt", "NotoSansCJK-OFL.txt"),
    ] {
        manifest.licenses.push(BuildAsset {
            source: source.join(path),
            dest: format!("licenses/impacto/{name}"),
        });
    }
    // vcpkg's per-port notices include both DLL and header/static dependencies.
    // Keep the full prepared target installation's notices to cover transitive
    // dependencies without guessing their filenames or linkage.
    let mut ports = fs::read_dir(&share)?.collect::<std::io::Result<Vec<_>>>()?;
    ports.sort_by_key(|entry| entry.file_name());
    for port in ports {
        if !port.file_type()?.is_dir() {
            continue;
        }
        let copyright = port.path().join("copyright");
        if copyright.is_file() {
            let name = port
                .file_name()
                .into_string()
                .map_err(|_| "non-UTF-8 vcpkg port name")?;
            manifest.licenses.push(BuildAsset {
                source: copyright,
                dest: format!("licenses/impacto/vcpkg/{name}/copyright"),
            });
        }
    }
    write_core_build_manifest(&manifest)?;
    Ok(())
}

fn main() {
    if let Err(error) = build() {
        panic!("Impacto native build failed: {error}");
    }
}
