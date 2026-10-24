// The ANANAS SLICER shell: starts ananas-engine, forwards the front end's requests to it and its
// notifications back. All slicing knowledge stays in the engine.

mod engine;

use std::path::PathBuf;
use std::sync::Arc;

use engine::{EngineProcess, RpcError};
use serde::Serialize;
use serde_json::Value;
use tauri::{Emitter, Manager, State};

/// The engine, or why it did not start.
struct EngineState(Result<Arc<EngineProcess>, String>);

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Defaults {
    model: String,
    output_dir: String,
}

/// A path from the environment, else the one the development build tree uses (relative to this crate).
fn configured(variable: &str, development_default: &str) -> PathBuf {
    std::env::var_os(variable)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..").join(development_default))
}

#[tauri::command]
async fn engine_call(state: State<'_, EngineState>, method: String, params: Option<Value>) -> Result<Value, RpcError> {
    let engine = match &state.0 {
        Ok(engine) => Arc::clone(engine),
        Err(reason) => return Err(RpcError { code: -1, message: reason.clone() }),
    };
    let params = params.unwrap_or(Value::Null);
    let started = std::time::Instant::now();
    let name = method.clone();
    let answer = tauri::async_runtime::spawn_blocking(move || engine.call(&method, params))
        .await
        .map_err(|error| RpcError { code: -1, message: error.to_string() })?;
    #[cfg(debug_assertions)]
    eprintln!(
        "ananas-ui: engine_call {name} -> {} in {} ms",
        match &answer {
            Ok(_) => "ok".to_owned(),
            Err(error) => format!("error {}", error.code),
        },
        started.elapsed().as_millis()
    );
    #[cfg(not(debug_assertions))]
    let _ = (name, started);
    answer
}

#[tauri::command]
fn engine_defaults() -> Defaults {
    Defaults {
        model: configured("ANANAS_SAMPLE_MODEL", "tests/data/20mm_cube.obj").to_string_lossy().into_owned(),
        output_dir: std::env::temp_dir().join("ananas-slicer").to_string_lossy().into_owned(),
    }
}

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            let binary = configured(
                "ANANAS_ENGINE_BIN",
                "build/mini/src/engine/Release/ananas-engine.app/Contents/MacOS/ananas-engine",
            );
            let profiles = configured("ANANAS_PROFILES_DIR", "resources/profiles");
            let vendor = std::env::var("ANANAS_VENDOR").unwrap_or_else(|_| "Voron".to_owned());
            let handle = app.handle().clone();
            let sink = Box::new(move |notification: Value| {
                let _ = handle.emit("engine-notification", notification);
            });
            let started = EngineProcess::spawn(sink, &binary, &profiles, &vendor).map(Arc::new);
            if let Err(reason) = &started {
                eprintln!("ananas-ui: {reason}");
            }
            app.manage(EngineState(started));
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![engine_call, engine_defaults])
        .run(tauri::generate_context!())
        .expect("error while running the ANANAS SLICER shell");
}
