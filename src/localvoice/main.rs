//! Native CPU decoder. Models are downloaded separately from Tencent.
use std::io::{self, Read, Write};
use std::sync::{
    atomic::{AtomicU64, Ordering},
    mpsc, Arc,
};
use std::time::{Duration, Instant};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;
trait Context<T> {
    fn context(self, message: impl Into<String>) -> Result<T>;
    fn with_context(self, message: impl FnOnce() -> String) -> Result<T>;
}
impl<T> Context<T> for Option<T> {
    fn context(self, message: impl Into<String>) -> Result<T> {
        self.ok_or_else(|| message.into().into())
    }
    fn with_context(self, message: impl FnOnce() -> String) -> Result<T> {
        self.ok_or_else(|| message().into())
    }
}
impl<T, E: std::fmt::Display> Context<T> for std::result::Result<T, E> {
    fn context(self, message: impl Into<String>) -> Result<T> {
        self.map_err(|e| format!("{}: {e}", message.into()).into())
    }
    fn with_context(self, message: impl FnOnce() -> String) -> Result<T> {
        self.map_err(|e| format!("{}: {e}", message()).into())
    }
}
macro_rules! bail { ($($arg:tt)*) => { return Err(format!($($arg)*).into()) }; }
mod fbank;
mod kernels;
mod model;
mod parallel;
mod stream;
mod xnet;

fn quoted(value: &str) -> String {
    let mut out = String::from("\"");
    for c in value.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c < ' ' => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}
fn emit(kind: &str, token: &str, text: &str) -> bool {
    let mut out = io::stdout().lock();
    writeln!(
        out,
        "{{\"event\":{},\"token\":{},\"text\":{},\"message\":{}}}",
        quoted(kind),
        quoted(token),
        quoted(text),
        quoted(text)
    )
    .and_then(|_| out.flush())
    .is_ok()
}
fn limit_reached(token: &str) -> bool {
    let mut out = io::stdout().lock();
    writeln!(
        out,
        "{{\"event\":\"state\",\"token\":{},\"state\":2,\"limit\":true}}",
        quoted(token)
    )
    .and_then(|_| out.flush())
    .is_ok()
}
static GENERATION: AtomicU64 = AtomicU64::new(1);
thread_local! { static ACTIVE: std::cell::Cell<u64> = std::cell::Cell::new(0); }
pub fn interrupted() -> bool {
    ACTIVE.with(|value| value.get() != GENERATION.load(Ordering::SeqCst))
}
struct Command {
    generation: u64,
    op: u8,
    data: Vec<u8>,
}
fn serve(model: Arc<model::Model>) {
    let generation = &GENERATION;
    let (tx, rx) = mpsc::sync_channel(100);
    let current = &GENERATION;
    // Read independently of inference: Esc/focus loss invalidates queued audio
    // immediately, even on slow CPUs. Audio is bounded to ten minutes/session.
    std::thread::spawn(move || {
        let mut input = io::stdin().lock();
        let mut samples = 0usize;
        let mut rejected = false;
        loop {
            let mut header = [0u8; 5];
            if input.read_exact(&mut header).is_err() {
                break;
            }
            let size = u32::from_le_bytes(header[1..].try_into().unwrap()) as usize;
            if size > 131072 {
                break;
            }
            let mut data = vec![0; size];
            if input.read_exact(&mut data).is_err() {
                break;
            }
            let op = header[0];
            if op == b'S' || op == b'C' {
                current.fetch_add(1, Ordering::SeqCst);
                samples = 0;
                rejected = false;
            }
            if op == b'P' {
                if rejected {
                    continue;
                }
                if size % 2 != 0 {
                    break;
                }
                if samples + size / 2 > 16000 * 600 {
                    data.truncate((16000 * 600 - samples) * 2);
                    let value = current.load(Ordering::SeqCst);
                    if !data.is_empty()
                        && tx
                            .send(Command {
                                generation: value,
                                op: b'P',
                                data,
                            })
                            .is_err()
                    {
                        return;
                    }
                    let _ = tx.send(Command {
                        generation: value,
                        op: b'L',
                        data: vec![],
                    });
                    rejected = true;
                    continue;
                }
                samples += size / 2;
            }
            let command = Command {
                generation: current.load(Ordering::SeqCst),
                op,
                data,
            };
            if tx.send(command).is_err() || op == b'Q' {
                return;
            }
        }
        current.fetch_add(1, Ordering::SeqCst);
        let _ = tx.send(Command {
            generation: 0,
            op: b'Q',
            data: vec![],
        });
    });
    let mut token = String::new();
    let mut audio: Option<stream::Stream> = None;
    let mut stable = stream::Stabilizer::default();
    let mut shown = String::new();
    let mut refreshed = Instant::now();
    let mut cost = Duration::ZERO;
    for command in rx {
        if command.op == b'Q' {
            break;
        }
        if command.generation != generation.load(Ordering::SeqCst) {
            continue;
        }
        ACTIVE.with(|value| value.set(command.generation));
        match command.op {
            b'S' => {
                token = String::from_utf8_lossy(&command.data).into_owned();
                if token.len() > 256 {
                    break;
                }
                audio = Some(stream::Stream::new(model.clone()));
                stable.clear();
                shown.clear();
                cost = Duration::ZERO;
                refreshed = Instant::now() - Duration::from_secs(1);
                if !emit("started", &token, "") {
                    break;
                }
            }
            b'C' => {
                audio = None;
                shown.clear();
            }
            b'P' => {
                if let Some(audio) = audio.as_mut() {
                    let pcm: Vec<i16> = command
                        .data
                        .chunks_exact(2)
                        .map(|b| i16::from_le_bytes([b[0], b[1]]))
                        .collect();
                    audio.push(&pcm);
                    if audio.fresh() >= 30
                        && refreshed.elapsed() >= Duration::from_millis(300).max(cost * 3)
                    {
                        let begin = Instant::now();
                        let text = stable.update(&audio.partial());
                        cost = begin.elapsed();
                        refreshed = Instant::now();
                        if command.generation == generation.load(Ordering::SeqCst) && text != shown
                        {
                            shown = text;
                            if !emit("partial", &token, &shown) {
                                break;
                            }
                        }
                    }
                }
            }
            b'T' | b'F' | b'L' => {
                if let Some(mut audio) = audio.take() {
                    if command.op == b'L' && !limit_reached(&token) {
                        break;
                    }
                    // Keep settled blocks for long dictations; the model's full
                    // utterance pass is limited to twenty seconds by Stream.
                    let text = if command.op == b'F' && !shown.is_empty() {
                        shown.clone()
                    } else {
                        audio.finish()
                    };
                    if text.len() > 65536 {
                        if !emit("error", &token, "识别结果过长，请分段输入") {
                            break;
                        }
                        if !emit("final", &token, "") {
                            break;
                        }
                    } else if command.generation == generation.load(Ordering::SeqCst)
                        && !emit("final", &token, &text)
                    {
                        break;
                    }
                }
            }
            _ => {}
        }
    }
}
fn main() {
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 3 || !["--model", "--check"].contains(&args[1].as_str()) {
        eprintln!("wetypex-local-voice --model MODEL_DIR | --check MODEL_DIR");
        std::process::exit(2);
    }
    match model::Model::load(std::path::Path::new(&args[2])) {
        Ok(model) => {
            if emit("ready", "", "") && args[1] == "--model" {
                serve(Arc::new(model));
            }
        }
        Err(error) => {
            emit("error", "", &error.to_string());
            std::process::exit(1);
        }
    }
}
