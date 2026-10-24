//! The engine as a child process: one JSON-RPC request per line in, one response or notification per line out
//! (docs/HLSD/engine.md). Responses are matched to their request by id; notifications become Tauri events.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::path::Path;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex};
use std::thread;

use serde::Serialize;
use serde_json::{json, Value};

/// An error answer of the engine, or a failure to reach it. Serialized as `{code, message}` for the front end.
#[derive(Debug, Clone, Serialize)]
pub struct RpcError {
    pub code: i64,
    pub message: String,
}

impl RpcError {
    /// The shell itself failed (the engine is not running, a pipe broke).
    fn shell(message: impl Into<String>) -> Self {
        RpcError { code: -1, message: message.into() }
    }
}

type Pending = Arc<Mutex<HashMap<u64, Sender<Result<Value, RpcError>>>>>;

/// Receives the engine's notifications as `{method, params}`.
pub type NotificationSink = Box<dyn Fn(Value) + Send>;

pub struct EngineProcess {
    child: Mutex<Child>,
    stdin: Mutex<ChildStdin>,
    pending: Pending,
    next_id: Mutex<u64>,
}

impl EngineProcess {
    /// Starts `binary --profiles <profiles> [--vendor <vendor>]`. `sink` receives the engine's notifications.
    pub fn spawn(sink: NotificationSink, binary: &Path, profiles: &Path, vendor: &str) -> Result<Self, String> {
        let mut command = Command::new(binary);
        command.arg("--profiles").arg(profiles);
        if !vendor.is_empty() {
            command.args(["--vendor", vendor]);
        }
        let mut child = command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .map_err(|error| format!("cannot start the engine {}: {error}", binary.display()))?;
        let stdin = child.stdin.take().ok_or("the engine has no stdin")?;
        let stdout = child.stdout.take().ok_or("the engine has no stdout")?;

        let pending: Pending = Arc::new(Mutex::new(HashMap::new()));
        let reader_pending = Arc::clone(&pending);
        thread::spawn(move || {
            for line in BufReader::new(stdout).lines().map_while(Result::ok) {
                dispatch(&sink, &reader_pending, &line);
            }
            // The engine closed its output: nobody is going to answer what is still waiting.
            for (_, sender) in reader_pending.lock().expect("pending lock").drain() {
                let _ = sender.send(Err(RpcError::shell("the engine has exited")));
            }
        });

        Ok(EngineProcess { child: Mutex::new(child), stdin: Mutex::new(stdin), pending, next_id: Mutex::new(1) })
    }

    /// Sends a request and waits for its answer. Blocks, so call it off the UI thread.
    pub fn call(&self, method: &str, params: Value) -> Result<Value, RpcError> {
        let id = {
            let mut next = self.next_id.lock().expect("id lock");
            let id = *next;
            *next += 1;
            id
        };
        let (sender, receiver) = channel();
        self.pending.lock().expect("pending lock").insert(id, sender);

        let request = json!({ "jsonrpc": "2.0", "id": id, "method": method, "params": params });
        let written = {
            let mut stdin = self.stdin.lock().expect("stdin lock");
            writeln!(stdin, "{request}").and_then(|_| stdin.flush())
        };
        if let Err(error) = written {
            self.pending.lock().expect("pending lock").remove(&id);
            return Err(RpcError::shell(format!("cannot reach the engine: {error}")));
        }
        receiver.recv().map_err(|_| RpcError::shell("the engine did not answer"))?
    }
}

impl Drop for EngineProcess {
    fn drop(&mut self) {
        // Closing stdin ends the engine; kill is the fallback if it is stuck in a slice.
        if let Ok(mut child) = self.child.lock() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn dispatch(sink: &NotificationSink, pending: &Pending, line: &str) {
    let Ok(message) = serde_json::from_str::<Value>(line) else {
        return;
    };
    match message.get("id").and_then(Value::as_u64) {
        Some(id) => {
            let Some(sender) = pending.lock().expect("pending lock").remove(&id) else {
                return;
            };
            let answer = match message.get("error") {
                Some(error) => Err(RpcError {
                    code: error.get("code").and_then(Value::as_i64).unwrap_or(-32000),
                    message: error.get("message").and_then(Value::as_str).unwrap_or("engine error").to_owned(),
                }),
                None => Ok(message.get("result").cloned().unwrap_or(Value::Null)),
            };
            let _ = sender.send(answer);
        }
        None => {
            if let Some(method) = message.get("method").and_then(Value::as_str) {
                sink(json!({ "method": method, "params": message.get("params") }));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn repo() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    /// The engine of the development build tree, or None when it has not been built (the test then does not run).
    fn engine() -> Option<(PathBuf, PathBuf)> {
        let binary = std::env::var_os("ANANAS_ENGINE_BIN").map(PathBuf::from).unwrap_or_else(|| {
            repo().join("build/mini/src/engine/Release/ananas-engine.app/Contents/MacOS/ananas-engine")
        });
        let profiles = std::env::var_os("ANANAS_PROFILES_DIR").map(PathBuf::from).unwrap_or_else(|| repo().join("resources/profiles"));
        if binary.exists() && profiles.exists() {
            Some((binary, profiles))
        } else {
            eprintln!("skipped: no engine at {}", binary.display());
            None
        }
    }

    fn spawn_collecting() -> Option<(EngineProcess, Arc<Mutex<Vec<Value>>>)> {
        let (binary, profiles) = engine()?;
        let seen = Arc::new(Mutex::new(Vec::new()));
        let sink_seen = Arc::clone(&seen);
        let sink: NotificationSink = Box::new(move |notification| sink_seen.lock().unwrap().push(notification));
        Some((EngineProcess::spawn(sink, &binary, &profiles, "Voron").expect("engine starts"), seen))
    }

    #[test]
    fn an_answer_reaches_the_caller_that_asked() {
        let Some((engine, _)) = spawn_collecting() else { return };
        let info = engine.call("engine.info", Value::Null).expect("engine.info answers");
        assert_eq!(info["name"], "ananas-engine");
    }

    #[test]
    fn an_engine_error_keeps_its_code_and_message() {
        let Some((engine, _)) = spawn_collecting() else { return };
        let error = engine.call("no.such.method", Value::Null).expect_err("unknown method is refused");
        assert_eq!(error.code, -32601);
        assert!(error.message.contains("no.such.method"));
        let error = engine.call("config.resolve", json!({ "printer": "No Such Printer" })).expect_err("unknown printer");
        assert_eq!(error.code, -32001);
    }

    #[test]
    fn concurrent_calls_get_their_own_answers() {
        let Some((engine, _)) = spawn_collecting() else { return };
        let engine = Arc::new(engine);
        let handles: Vec<_> = (0..8)
            .map(|index| {
                let engine = Arc::clone(&engine);
                thread::spawn(move || {
                    if index % 2 == 0 {
                        engine.call("engine.info", Value::Null).map(|value| value["name"].clone())
                    } else {
                        engine.call("no.such.method", Value::Null).map(|_| Value::Null)
                    }
                })
            })
            .collect();
        for (index, handle) in handles.into_iter().enumerate() {
            let answer = handle.join().expect("thread finishes");
            if index % 2 == 0 {
                assert_eq!(answer.expect("info answers"), "ananas-engine");
            } else {
                assert_eq!(answer.expect_err("refused").code, -32601);
            }
        }
    }

    #[test]
    fn a_slice_delivers_progress_notifications_before_its_answer() {
        let Some((engine, seen)) = spawn_collecting() else { return };
        let output = std::env::temp_dir().join("ananas-ui-rust-test").join("cube.gcode");
        let result = engine
            .call(
                "slice",
                json!({
                    "printer": "Voron 2.4 350 0.4 nozzle",
                    "process": "0.20mm Standard @Voron",
                    "filament": "Generic PLA @System",
                    "model": repo().join("tests/data/20mm_cube.obj"),
                    "output": output,
                }),
            )
            .expect("slice answers");
        assert_eq!(result["layers"], 100);
        assert!(output.exists());
        let seen = seen.lock().unwrap();
        let progress: Vec<i64> = seen
            .iter()
            .filter(|notification| notification["method"] == "slice.progress")
            .filter_map(|notification| notification["params"]["percent"].as_i64())
            .collect();
        assert!(progress.len() > 5, "progress notifications arrive: {progress:?}");
        assert!(progress.windows(2).all(|pair| pair[0] <= pair[1]), "progress never goes back");
        assert_eq!(progress.last(), Some(&100));
    }

    #[test]
    fn calls_fail_cleanly_once_the_engine_has_exited() {
        let Some((binary, profiles)) = engine() else { return };
        let sink: NotificationSink = Box::new(|_| {});
        let engine = EngineProcess::spawn(sink, &binary, &profiles, "Voron").expect("engine starts");
        engine.child.lock().unwrap().kill().expect("kill");
        let error = loop {
            match engine.call("engine.info", Value::Null) {
                Err(error) => break error,
                Ok(_) => thread::yield_now(),
            }
        };
        assert_eq!(error.code, -1);
    }
}
