use std::{
    fs::{self, File},
    io::{Read, Write},
    path::Path,
    process::{Command, Stdio},
    sync::mpsc::Sender,
    thread,
};

use anyhow::{Context, Result, bail};
use regex::Regex;
use zip::{CompressionMethod, ZipWriter, write::SimpleFileOptions};

use crate::config::Config;

#[derive(Debug, Clone)]
pub enum Task {
    List,
    UpdateCache,
    Download(Vec<String>),
    Repair(Vec<String>),
    Status(Vec<String>),
}

#[derive(Debug)]
pub enum WorkerEvent {
    Log(String),
    Progress(String),
    Library(Vec<String>),
    Finished(Result<(), String>),
}

pub fn spawn(task: Task, config: Config, sender: Sender<WorkerEvent>) {
    thread::spawn(move || {
        let result = run_task(task, &config, &sender).map_err(|error| format!("{error:#}"));
        let _ = sender.send(WorkerEvent::Finished(result));
    });
}

fn run_task(task: Task, config: &Config, sender: &Sender<WorkerEvent>) -> Result<()> {
    ensure_logged_in(config, sender)?;
    match task {
        Task::List => list_games(config, sender),
        Task::UpdateCache => run_command(config, &["--update-cache".into()], sender),
        Task::Download(games) => process_games(config, games, GameAction::Download, sender),
        Task::Repair(games) => process_games(config, games, GameAction::Repair, sender),
        Task::Status(games) => verify_games(config, games, sender),
    }
}

fn ensure_logged_in(config: &Config, sender: &Sender<WorkerEvent>) -> Result<()> {
    let output = Command::new(&config.downloader)
        .args(["--check-login-status", "--no-color"])
        .output()
        .with_context(|| format!("failed to start {}", config.downloader.display()))?;
    let message = format!(
        "{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    if login_required(&message) {
        let error = "Login required. Choose a login action from the Home tab first.";
        log(sender, error);
        bail!(error);
    }
    if !output.status.success() {
        bail!("unable to check login status: {}", message.trim());
    }
    Ok(())
}

fn login_required(message: &str) -> bool {
    message.to_ascii_lowercase().contains("not logged in")
}

fn list_games(config: &Config, sender: &Sender<WorkerEvent>) -> Result<()> {
    let output = Command::new(&config.downloader)
        .args(["--list", "--no-color"])
        .output()
        .with_context(|| format!("failed to start {}", config.downloader.display()))?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    for line in stderr.lines() {
        log(sender, line);
    }
    if !output.status.success() {
        bail!("lgogdownloader --list exited with {}", output.status);
    }

    let ansi = Regex::new(r"\x1b\[[0-9;]*[mK]")?;
    let mut games: Vec<String> = stdout
        .lines()
        .map(|line| ansi.replace_all(line, "").trim().to_owned())
        .filter(|line| {
            !line.is_empty()
                && !line.starts_with("Getting product data")
                && !line.starts_with("Getting game names")
        })
        .collect();
    games.sort_unstable();
    games.dedup();
    log(sender, &format!("Loaded {} games", games.len()));
    sender.send(WorkerEvent::Library(games)).ok();
    Ok(())
}

#[derive(Clone, Copy)]
enum GameAction {
    Download,
    Repair,
}

fn process_games(
    config: &Config,
    games: Vec<String>,
    action: GameAction,
    sender: &Sender<WorkerEvent>,
) -> Result<()> {
    if games.is_empty() {
        bail!("select at least one game in the Library tab");
    }
    for game in games {
        let safe_name = safe_name(&game);
        let game_dir = config.download_dir.join(&safe_name);
        let mut args = match action {
            GameAction::Download => download_args(config, &game, &game_dir),
            GameAction::Repair => vec![
                "--repair".into(),
                "--game".into(),
                exact_game(&game),
                "--directory".into(),
                game_dir.to_string_lossy().into_owned(),
            ],
        };
        args.push("--no-color".into());
        run_command(config, &args, sender)?;

        if matches!(action, GameAction::Download) && config.archive.enabled {
            post_process(config, &game_dir, &safe_name, sender)?;
        }
    }
    Ok(())
}

fn verify_games(config: &Config, games: Vec<String>, sender: &Sender<WorkerEvent>) -> Result<()> {
    if games.is_empty() {
        bail!("select at least one game in the Library tab");
    }
    let mut total = StatusCounts::default();
    for game in games {
        let game_dir = config.download_dir.join(safe_name(&game));
        let args = vec![
            "--status".into(),
            "--game".into(),
            exact_game(&game),
            "--directory".into(),
            game_dir.to_string_lossy().into_owned(),
            "--no-color".into(),
        ];
        log(sender, &format!("Verifying downloads for {game}..."));
        total.add(&run_status_command(config, &args, sender)?);
    }
    log(sender, &total.summary());
    Ok(())
}

fn run_status_command(
    config: &Config,
    args: &[String],
    sender: &Sender<WorkerEvent>,
) -> Result<StatusCounts> {
    let output = Command::new(&config.downloader)
        .args(args)
        .output()
        .with_context(|| format!("failed to start {}", config.downloader.display()))?;
    for record in parse_output(&output.stderr) {
        send_record(sender, record);
    }
    if !output.status.success() {
        bail!("lgogdownloader verification exited with {}", output.status);
    }

    let mut counts = StatusCounts::default();
    for record in parse_output(&output.stdout) {
        if let Some((label, code)) = readable_status(&record) {
            counts.record(code);
            if code != "OK" {
                log(sender, &format!("{label}: {}", strip_status_code(&record)));
            }
        } else {
            send_record(sender, record);
        }
    }
    Ok(counts)
}

#[derive(Default)]
struct StatusCounts {
    ok: usize,
    missing: usize,
    incomplete: usize,
    different: usize,
}

impl StatusCounts {
    fn record(&mut self, code: &str) {
        match code {
            "OK" => self.ok += 1,
            "ND" => self.missing += 1,
            "FS" => self.incomplete += 1,
            "MD5" => self.different += 1,
            _ => {}
        }
    }

    fn add(&mut self, other: &Self) {
        self.ok += other.ok;
        self.missing += other.missing;
        self.incomplete += other.incomplete;
        self.different += other.different;
    }

    fn summary(&self) -> String {
        let checked = self.ok + self.missing + self.incomplete + self.different;
        if checked == 0 {
            return "Verification complete: no matching downloadable files found.".into();
        }
        format!(
            "Verification complete: {} healthy, {} missing, {} incomplete, {} different version.",
            self.ok, self.missing, self.incomplete, self.different
        )
    }
}

fn readable_status(record: &str) -> Option<(&'static str, &str)> {
    let code = record.split_whitespace().next()?;
    let label = match code {
        "OK" => "Healthy",
        "ND" => "Missing",
        "FS" => "Incomplete",
        "MD5" => "Different version",
        _ => return None,
    };
    Some((label, code))
}

fn strip_status_code(record: &str) -> &str {
    record.split_once(' ').map_or(record, |(_, detail)| detail)
}

fn download_args(config: &Config, game: &str, game_dir: &Path) -> Vec<String> {
    let mut args = vec![
        "--download".into(),
        "--game".into(),
        exact_game(game),
        "--threads".into(),
        config.threads.to_string(),
        "--directory".into(),
        game_dir.to_string_lossy().into_owned(),
        "--platform".into(),
        config.platform.clone(),
        "--language".into(),
        config.language.clone(),
    ];
    if !config.include.trim().is_empty() {
        args.extend(["--include".into(), config.include.clone()]);
    }
    if !config.exclude.trim().is_empty() {
        args.extend(["--exclude".into(), config.exclude.clone()]);
    }
    args
}

fn run_command(config: &Config, args: &[String], sender: &Sender<WorkerEvent>) -> Result<()> {
    log(
        sender,
        &format!("$ {} {}", config.downloader.display(), args.join(" ")),
    );
    let mut child = Command::new(&config.downloader)
        .args(args)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("failed to start {}", config.downloader.display()))?;

    let stdout = child.stdout.take().context("failed to capture stdout")?;
    let stderr = child.stderr.take().context("failed to capture stderr")?;
    let out_sender = sender.clone();
    let err_sender = sender.clone();
    let out_thread = thread::spawn(move || stream_lines(stdout, out_sender));
    let err_thread = thread::spawn(move || stream_lines(stderr, err_sender));
    let status = child.wait()?;
    out_thread.join().ok();
    err_thread.join().ok();
    if !status.success() {
        bail!("lgogdownloader exited with {status}");
    }
    Ok(())
}

fn stream_lines(stream: impl Read, sender: Sender<WorkerEvent>) {
    let mut parser = OutputParser::default();
    let mut stream = stream;
    let mut buffer = [0_u8; 4096];
    while let Ok(read) = stream.read(&mut buffer) {
        if read == 0 {
            break;
        }
        for record in parser.push(&buffer[..read]) {
            send_record(&sender, record);
        }
    }
    if let Some(record) = parser.finish() {
        send_record(&sender, record);
    }
}

fn parse_output(output: &[u8]) -> Vec<String> {
    let mut parser = OutputParser::default();
    let mut records = parser.push(output);
    if let Some(record) = parser.finish() {
        records.push(record);
    }
    records
}

fn send_record(sender: &Sender<WorkerEvent>, record: String) {
    let event = if is_progress(&record) {
        WorkerEvent::Progress(record)
    } else {
        WorkerEvent::Log(record)
    };
    let _ = sender.send(event);
}

fn is_progress(record: &str) -> bool {
    let record = record.trim_start();
    record.starts_with("Getting product data ")
        || record.starts_with("Getting game names ")
        || record.starts_with("Getting game info ")
        || record.starts_with("Clearing update flags ")
        || record.starts_with("Checking for orphaned files ")
        || record.starts_with("Chunks hashed ")
        || record.starts_with("Chunk ")
        || record.starts_with('#')
        || record.starts_with("Total:")
        || record.starts_with("Remaining:")
        || record
            .split_whitespace()
            .next()
            .is_some_and(|word| word.ends_with('%') && word[..word.len() - 1].parse::<u8>().is_ok())
}

#[derive(Default)]
struct OutputParser {
    record: Vec<u8>,
    state: ControlState,
}

#[derive(Default)]
enum ControlState {
    #[default]
    Text,
    Escape,
    Csi,
    Osc,
    OscEscape,
}

impl OutputParser {
    fn push(&mut self, input: &[u8]) -> Vec<String> {
        let mut records = Vec::new();
        for &byte in input {
            match self.state {
                ControlState::Text => match byte {
                    0x1b => self.state = ControlState::Escape,
                    b'\r' | b'\n' => {
                        if let Some(record) = self.take_record() {
                            records.push(record);
                        }
                    }
                    _ => self.record.push(byte),
                },
                ControlState::Escape => {
                    self.state = match byte {
                        b'[' => ControlState::Csi,
                        b']' => ControlState::Osc,
                        _ => ControlState::Text,
                    };
                }
                ControlState::Csi => {
                    if (0x40..=0x7e).contains(&byte) {
                        self.state = ControlState::Text;
                    }
                }
                ControlState::Osc => {
                    self.state = match byte {
                        0x07 => ControlState::Text,
                        0x1b => ControlState::OscEscape,
                        _ => ControlState::Osc,
                    };
                }
                ControlState::OscEscape => {
                    self.state = if byte == b'\\' {
                        ControlState::Text
                    } else {
                        ControlState::Osc
                    };
                }
            }
        }
        records
    }

    fn finish(&mut self) -> Option<String> {
        self.take_record()
    }

    fn take_record(&mut self) -> Option<String> {
        let record = String::from_utf8_lossy(&self.record).trim().to_owned();
        self.record.clear();
        (!record.is_empty()).then_some(record)
    }
}

fn post_process(
    config: &Config,
    game_dir: &Path,
    safe_name: &str,
    sender: &Sender<WorkerEvent>,
) -> Result<()> {
    if !game_dir.exists() {
        bail!("download directory {} was not created", game_dir.display());
    }
    fs::create_dir_all(&config.archive.output_dir)?;
    let archive = config.archive.output_dir.join(format!("{safe_name}.zip"));
    if archive.starts_with(game_dir) {
        bail!("archive output directory cannot be inside the downloaded game directory");
    }
    log(sender, &format!("Creating {}", archive.display()));
    zip_directory(game_dir, &archive)?;

    if config.archive.move_to_vault {
        fs::create_dir_all(&config.archive.vault_dir)?;
        let destination = config.archive.vault_dir.join(format!("{safe_name}.zip"));
        if archive != destination {
            if destination.exists() {
                fs::remove_file(&destination)?;
            }
            fs::rename(&archive, &destination).or_else(|_| {
                fs::copy(&archive, &destination)?;
                fs::remove_file(&archive)
            })?;
            log(
                sender,
                &format!("Moved archive to {}", destination.display()),
            );
        }
    }

    if config.archive.delete_source {
        fs::remove_dir_all(game_dir)?;
        log(sender, &format!("Deleted {}", game_dir.display()));
    }
    Ok(())
}

fn zip_directory(source: &Path, destination: &Path) -> Result<()> {
    let file = File::create(destination)?;
    let mut zip = ZipWriter::new(file);
    let options = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
    let root = source.parent().unwrap_or(source);
    add_to_zip(&mut zip, source, root, options)?;
    zip.finish()?;
    Ok(())
}

fn add_to_zip(
    zip: &mut ZipWriter<File>,
    path: &Path,
    root: &Path,
    options: SimpleFileOptions,
) -> Result<()> {
    let relative = path
        .strip_prefix(root)?
        .to_string_lossy()
        .replace('\\', "/");
    if path.is_dir() {
        if !relative.is_empty() {
            zip.add_directory(format!("{relative}/"), options)?;
        }
        for entry in fs::read_dir(path)? {
            add_to_zip(zip, &entry?.path(), root, options)?;
        }
    } else {
        zip.start_file(relative, options)?;
        let mut source = File::open(path)?;
        std::io::copy(&mut source, zip)?;
        zip.flush()?;
    }
    Ok(())
}

fn exact_game(game: &str) -> String {
    format!("^{}$", regex::escape(game))
}

fn safe_name(game: &str) -> String {
    game.chars()
        .map(|character| match character {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' => '_',
            _ => character,
        })
        .collect()
}

fn log(sender: &Sender<WorkerEvent>, message: &str) {
    let _ = sender.send(WorkerEvent::Log(message.to_owned()));
}

#[cfg(test)]
mod tests {
    use super::{
        OutputParser, StatusCounts, exact_game, is_progress, login_required, readable_status,
        safe_name,
    };

    #[test]
    fn sanitizes_unsafe_filename_characters() {
        assert_eq!(safe_name("a/b:c*?"), "a_b_c__");
    }

    #[test]
    fn creates_exact_regex_filter() {
        assert_eq!(exact_game("game+dlc"), r"^game\+dlc$");
    }

    #[test]
    fn recognizes_logged_out_status() {
        assert!(login_required("Login status: Not logged in"));
        assert!(!login_required("Login status: Logged in"));
    }

    #[test]
    fn parses_terminal_progress_without_control_text() {
        let mut parser = OutputParser::default();
        let records = parser.push(
            b"\x1b[1A\x1b[JGetting game info 151 / 152\n\
              \x1b[KGetting product data 1 / 2\r\
              \x1b[KGetting game names 17 / 152\r",
        );
        assert_eq!(
            records,
            [
                "Getting game info 151 / 152",
                "Getting product data 1 / 2",
                "Getting game names 17 / 152"
            ]
        );
        assert!(records.iter().all(|record| is_progress(record)));
    }

    #[test]
    fn parses_controls_split_across_reads() {
        let mut parser = OutputParser::default();
        assert!(parser.push(b"\x1b[").is_empty());
        assert_eq!(
            parser.push(b"KGetting game names 2 / 3\r"),
            ["Getting game names 2 / 3"]
        );
    }

    #[test]
    fn summarizes_download_verification() {
        let mut counts = StatusCounts::default();
        for line in ["OK game file.zip", "ND game extra.pdf", "FS game setup.bin"] {
            let (_, code) = readable_status(line).unwrap();
            counts.record(code);
        }
        assert_eq!(
            counts.summary(),
            "Verification complete: 1 healthy, 1 missing, 1 incomplete, 0 different version."
        );
    }
}
