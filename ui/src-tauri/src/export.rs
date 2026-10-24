//! Saving a sliced G-code where the user chose: the one place the shell writes a file on the front end's behalf.

use std::fs;
use std::path::{Path, PathBuf};

const GCODE_EXTENSIONS: [&str; 3] = ["gcode", "gco", "g"];

/// The folder the engine writes its G-code to; only files in it can be exported.
pub fn output_dir() -> PathBuf {
    std::env::temp_dir().join("ananas-slicer")
}

fn has_gcode_extension(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .map(|extension| GCODE_EXTENSIONS.contains(&extension.to_ascii_lowercase().as_str()))
        .unwrap_or(false)
}

/// Copies `from` (a G-code the engine wrote under `allowed`) to `to` (a file with a G-code extension). Nothing else
/// can be read or written this way: a path outside the output folder, a link out of it, an existing directory or another
/// extension is refused.
pub fn export_gcode(from: &Path, to: &Path, allowed: &Path) -> Result<u64, String> {
    let source = fs::canonicalize(from).map_err(|error| format!("cannot read {}: {error}", from.display()))?;
    let root = fs::canonicalize(allowed).map_err(|error| format!("no G-code has been written yet: {error}"))?;
    if !source.starts_with(&root) || !source.is_file() || !has_gcode_extension(&source) {
        return Err("only a G-code written by the slicer can be exported".to_owned());
    }
    if !has_gcode_extension(to) {
        return Err("the file to save must end in .gcode".to_owned());
    }
    if to.is_dir() {
        return Err("the file to save is a folder".to_owned());
    }
    match to.parent() {
        Some(parent) if parent.as_os_str().is_empty() || parent.is_dir() => {}
        _ => return Err("the folder to save in does not exist".to_owned()),
    }
    fs::copy(&source, to).map_err(|error| format!("cannot write {}: {error}", to.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("ananas-export-test-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn copies_a_gcode_from_the_output_folder_to_the_chosen_file() {
        let work = scratch("ok");
        let out = work.join("out");
        fs::create_dir_all(&out).unwrap();
        fs::write(out.join("cube.gcode"), "G1 X1\n").unwrap();
        let target = work.join("saved.gcode");
        assert_eq!(export_gcode(&out.join("cube.gcode"), &target, &out).unwrap(), 6);
        assert_eq!(fs::read_to_string(target).unwrap(), "G1 X1\n");
    }

    #[test]
    fn refuses_a_file_outside_the_output_folder() {
        let work = scratch("outside");
        let out = work.join("out");
        fs::create_dir_all(&out).unwrap();
        fs::write(work.join("secret.gcode"), "x").unwrap();
        assert!(export_gcode(&work.join("secret.gcode"), &work.join("copy.gcode"), &out).is_err());
        assert!(!work.join("copy.gcode").exists());
    }

    #[test]
    fn refuses_a_path_that_climbs_out_of_the_output_folder() {
        let work = scratch("climb");
        let out = work.join("out");
        fs::create_dir_all(&out).unwrap();
        fs::write(work.join("secret.gcode"), "x").unwrap();
        let sneaky = out.join("..").join("secret.gcode");
        assert!(export_gcode(&sneaky, &work.join("copy.gcode"), &out).is_err());
    }

    #[cfg(unix)]
    #[test]
    fn refuses_a_link_that_leads_out_of_the_output_folder() {
        let work = scratch("link");
        let out = work.join("out");
        fs::create_dir_all(&out).unwrap();
        fs::write(work.join("secret.gcode"), "x").unwrap();
        std::os::unix::fs::symlink(work.join("secret.gcode"), out.join("link.gcode")).unwrap();
        assert!(export_gcode(&out.join("link.gcode"), &work.join("copy.gcode"), &out).is_err());
    }

    #[test]
    fn refuses_other_file_types_on_either_side() {
        let work = scratch("types");
        let out = work.join("out");
        fs::create_dir_all(&out).unwrap();
        fs::write(out.join("notes.txt"), "x").unwrap();
        fs::write(out.join("cube.gcode"), "x").unwrap();
        assert!(export_gcode(&out.join("notes.txt"), &work.join("a.gcode"), &out).is_err());
        assert!(export_gcode(&out.join("cube.gcode"), &work.join("a.sh"), &out).is_err());
        assert!(export_gcode(&out.join("cube.gcode"), &work, &out).is_err());
    }

    #[test]
    fn refuses_a_folder_that_does_not_exist() {
        let work = scratch("nofolder");
        let out = work.join("out");
        fs::create_dir_all(&out).unwrap();
        fs::write(out.join("cube.gcode"), "x").unwrap();
        assert!(export_gcode(&out.join("cube.gcode"), &work.join("missing").join("a.gcode"), &out).is_err());
    }
}
