mod app;
mod config;
mod worker;

use std::{
    io::{Write, stdout},
    process::Command,
    time::Duration,
};

use anyhow::{Context, Result, bail};
use app::{App, AppCommand};
use crossterm::{
    cursor::{SetCursorStyle, Show},
    event::{self, DisableMouseCapture, EnableMouseCapture, Event, KeyEventKind},
    execute,
};

fn main() -> Result<()> {
    let mut terminal = ratatui::init();
    execute!(stdout(), EnableMouseCapture)?;
    let result = run(&mut terminal);
    execute!(stdout(), DisableMouseCapture)?;
    ratatui::restore();
    result
}

fn run(terminal: &mut ratatui::DefaultTerminal) -> Result<()> {
    let mut app = App::new();
    while !app.quit {
        app.tick();
        terminal.draw(|frame| app.draw(frame))?;
        if event::poll(Duration::from_millis(100))? {
            let command = match event::read()? {
                Event::Key(key) if key.kind == KeyEventKind::Press => app.handle_key(key),
                Event::Mouse(mouse) => app.handle_mouse(mouse),
                _ => None,
            };
            if let Some(command) = command {
                run_external(&mut app, command)?;
                *terminal = ratatui::init();
                execute!(stdout(), EnableMouseCapture)?;
            }
        }
    }
    Ok(())
}

fn run_external(app: &mut App, command: AppCommand) -> Result<()> {
    ratatui::restore();
    execute!(stdout(), DisableMouseCapture)?;
    execute!(stdout(), Show, SetCursorStyle::SteadyBar)?;
    stdout().flush()?;
    let (action, argument, explanation) = match command {
        AppCommand::Login => (
            "Login",
            "--login",
            "Enter your GOG email, password, and verification code when prompted.",
        ),
        AppCommand::BrowserLogin => (
            "Browser login",
            "--browser-login",
            "Open the shown URL, then paste the completed callback URL when prompted.",
        ),
    };
    println!("LGOGDownloader {action}");
    println!("The TUI is paused so interactive input works normally.");
    println!("{explanation}\n");
    let result: Result<()> = (|| {
        let status = Command::new(app.downloader())
            .arg(argument)
            .status()
            .with_context(|| format!("failed to start {}", app.downloader().display()))?;
        if !status.success() {
            bail!("lgogdownloader exited with {status}");
        }
        let login_status = Command::new(app.downloader())
            .args(["--check-login-status", "--no-color"])
            .output()
            .context("failed to verify login status")?;
        let message = format!(
            "{}{}",
            String::from_utf8_lossy(&login_status.stdout),
            String::from_utf8_lossy(&login_status.stderr)
        );
        if !login_status.status.success() || message.to_ascii_lowercase().contains("not logged in")
        {
            bail!(
                "login did not complete and was not saved; finish every prompt and try again ({})",
                message.trim()
            );
        }
        Ok(())
    })();
    app.interactive_finished(action, result.map_err(|error| format!("{error:#}")));
    Ok(())
}
