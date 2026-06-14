use std::{
    collections::BTreeSet,
    path::Path,
    sync::mpsc::{self, Receiver},
};

use crossterm::event::{KeyCode, KeyEvent, KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::{
    Frame,
    layout::{Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, List, ListItem, ListState, Paragraph, Tabs, Wrap},
};

use crate::{
    config::Config,
    worker::{self, Task, WorkerEvent},
};

const SETTINGS: [&str; 13] = [
    "Downloader",
    "Download directory",
    "Threads",
    "Platform",
    "Language",
    "Include",
    "Exclude",
    "Create ZIP archive",
    "Archive directory",
    "Move archive to vault",
    "Vault directory",
    "Delete source after archive",
    "Save settings",
];

const LIST_SCROLL_PADDING: usize = 3;
const MAX_LOG_LINES: usize = 1_000;
const MAX_EVENTS_PER_TICK: usize = 64;

const ACTIONS: [&str; 11] = [
    "Refresh library",
    "Open library",
    "Download selected games",
    "Repair selected games",
    "Verify selected downloads",
    "Login with email/password + verification code",
    "Browser login",
    "Update game details cache",
    "Open settings",
    "Open logs",
    "Quit",
];

#[derive(Clone, Copy, PartialEq, Eq)]
enum Tab {
    Home,
    Library,
    Settings,
    Logs,
}

pub enum AppCommand {
    Login,
    BrowserLogin,
}

pub struct App {
    tab: Tab,
    pub quit: bool,
    config: Config,
    games: Vec<String>,
    selected_games: BTreeSet<String>,
    action_index: usize,
    action_offset: usize,
    action_height: usize,
    game_index: usize,
    game_offset: usize,
    game_height: usize,
    setting_index: usize,
    setting_offset: usize,
    setting_height: usize,
    editing: bool,
    input: String,
    logs: Vec<String>,
    log_scroll: usize,
    busy: bool,
    receiver: Option<Receiver<WorkerEvent>>,
    status: String,
    tab_area: Rect,
    content_area: Rect,
}

impl App {
    pub fn new() -> Self {
        let (config, status) = match Config::load() {
            Ok(config) => (
                config,
                "Ready. Use arrows and Enter to choose an action.".into(),
            ),
            Err(error) => (Config::default(), format!("Using defaults: {error:#}")),
        };
        Self {
            tab: Tab::Home,
            quit: false,
            config,
            games: Vec::new(),
            selected_games: BTreeSet::new(),
            action_index: 0,
            action_offset: 0,
            action_height: 0,
            game_index: 0,
            game_offset: 0,
            game_height: 0,
            setting_index: 0,
            setting_offset: 0,
            setting_height: 0,
            editing: false,
            input: String::new(),
            logs: Vec::new(),
            log_scroll: 0,
            busy: false,
            receiver: None,
            status,
            tab_area: Rect::default(),
            content_area: Rect::default(),
        }
    }

    pub fn tick(&mut self) {
        let mut events = Vec::new();
        if let Some(receiver) = &self.receiver {
            events.extend(receiver.try_iter().take(MAX_EVENTS_PER_TICK));
        }
        for event in events {
            match event {
                WorkerEvent::Log(line) => {
                    self.logs.push(line);
                    if self.logs.len() > MAX_LOG_LINES {
                        self.logs.drain(..self.logs.len() - MAX_LOG_LINES);
                    }
                }
                WorkerEvent::Progress(line) => self.status = line,
                WorkerEvent::Library(games) => {
                    self.games = games;
                    self.game_index = 0;
                    self.game_offset = 0;
                    self.tab = Tab::Library;
                }
                WorkerEvent::Finished(result) => {
                    self.busy = false;
                    self.receiver = None;
                    self.status = match result {
                        Ok(()) => "Task completed successfully".into(),
                        Err(error) => {
                            self.logs.push(format!("ERROR: {error}"));
                            format!("Task failed: {error}")
                        }
                    };
                }
            }
        }
    }

    pub fn handle_key(&mut self, key: KeyEvent) -> Option<AppCommand> {
        if self.editing {
            self.handle_edit_key(key);
            return None;
        }
        match key.code {
            KeyCode::Char('q') if !self.busy => self.quit = true,
            KeyCode::Char('1') => self.tab = Tab::Home,
            KeyCode::Char('2') => self.tab = Tab::Library,
            KeyCode::Char('3') => self.tab = Tab::Settings,
            KeyCode::Char('4') => self.tab = Tab::Logs,
            KeyCode::Esc => self.tab = Tab::Home,
            KeyCode::Tab | KeyCode::Right => self.next_tab(),
            KeyCode::BackTab | KeyCode::Left => self.previous_tab(),
            KeyCode::Up => self.move_up(),
            KeyCode::Down => self.move_down(),
            KeyCode::Char(' ') if self.tab == Tab::Library => self.toggle_game(),
            KeyCode::Enter if self.tab == Tab::Home => return self.activate_action(),
            KeyCode::Enter if self.tab == Tab::Library => self.toggle_game(),
            KeyCode::Enter if self.tab == Tab::Settings => self.edit_setting(),
            KeyCode::Char('s') if key.modifiers.contains(KeyModifiers::CONTROL) => {
                self.save_config()
            }
            KeyCode::Home if self.tab == Tab::Logs => self.log_scroll = self.logs.len(),
            KeyCode::End if self.tab == Tab::Logs => self.log_scroll = 0,
            KeyCode::Char('c') if self.tab == Tab::Logs => {
                self.logs.clear();
                self.log_scroll = 0;
            }
            _ => {}
        }
        None
    }

    pub fn handle_mouse(&mut self, mouse: MouseEvent) -> Option<AppCommand> {
        match mouse.kind {
            MouseEventKind::ScrollUp => {
                self.move_up();
                return None;
            }
            MouseEventKind::ScrollDown => {
                self.move_down();
                return None;
            }
            _ => {}
        }
        if mouse.kind != MouseEventKind::Down(MouseButton::Left) {
            return None;
        }
        if self.tab_area.contains((mouse.column, mouse.row).into()) {
            self.tab = match mouse.column.saturating_sub(self.tab_area.x + 2) {
                0..=5 => Tab::Home,
                6..=15 => Tab::Library,
                16..=26 => Tab::Settings,
                _ => Tab::Logs,
            };
            return None;
        }

        let inner_y = self.content_area.y + 1;
        if mouse.row < inner_y || mouse.row >= self.content_area.bottom().saturating_sub(1) {
            return None;
        }
        let row = (mouse.row - inner_y) as usize;
        match self.tab {
            Tab::Home
                if mouse.column < self.content_area.x + self.content_area.width * 48 / 100 =>
            {
                let index = self.action_offset + row;
                if index < ACTIONS.len() {
                    self.action_index = index;
                    return self.activate_action();
                }
            }
            Tab::Library => {
                let index = self.game_offset + row;
                if index < self.games.len() {
                    self.game_index = index;
                    self.toggle_game();
                }
            }
            Tab::Settings => {
                let index = self.setting_offset + row;
                if index < SETTINGS.len() {
                    self.setting_index = index;
                    self.edit_setting();
                }
            }
            _ => {}
        }
        None
    }

    pub fn downloader(&self) -> &Path {
        &self.config.downloader
    }

    pub fn interactive_finished(&mut self, action: &str, result: Result<(), String>) {
        self.status = match result {
            Ok(()) => format!("{action} completed successfully"),
            Err(error) => format!("{action} failed: {error}"),
        };
    }

    fn selected(&self) -> Vec<String> {
        self.selected_games.iter().cloned().collect()
    }

    fn start(&mut self, task: Task) {
        if self.busy {
            self.status = "A task is already running".into();
            return;
        }
        let (sender, receiver) = mpsc::channel();
        worker::spawn(task, self.config.clone(), sender);
        self.receiver = Some(receiver);
        self.busy = true;
        self.status = "Task running in background. Open Logs to inspect output.".into();
    }

    fn activate_action(&mut self) -> Option<AppCommand> {
        match self.action_index {
            0 => self.start(Task::List),
            1 => self.tab = Tab::Library,
            2 => self.start(Task::Download(self.selected())),
            3 => self.start(Task::Repair(self.selected())),
            4 => self.start(Task::Status(self.selected())),
            5 if !self.busy => return Some(AppCommand::Login),
            5 => self.status = "Wait for the running task to finish before logging in".into(),
            6 if !self.busy => return Some(AppCommand::BrowserLogin),
            6 => self.status = "Wait for the running task to finish before logging in".into(),
            7 => self.start(Task::UpdateCache),
            8 => self.tab = Tab::Settings,
            9 => self.tab = Tab::Logs,
            10 if !self.busy => self.quit = true,
            10 => self.status = "Wait for the running task to finish before quitting".into(),
            _ => {}
        }
        None
    }

    fn next_tab(&mut self) {
        self.tab = match self.tab {
            Tab::Home => Tab::Library,
            Tab::Library => Tab::Settings,
            Tab::Settings => Tab::Logs,
            Tab::Logs => Tab::Home,
        };
    }

    fn previous_tab(&mut self) {
        self.tab = match self.tab {
            Tab::Home => Tab::Logs,
            Tab::Library => Tab::Home,
            Tab::Settings => Tab::Library,
            Tab::Logs => Tab::Settings,
        };
    }

    fn move_up(&mut self) {
        match self.tab {
            Tab::Home => move_selection_up(&mut self.action_index, &mut self.action_offset),
            Tab::Library => move_selection_up(&mut self.game_index, &mut self.game_offset),
            Tab::Settings => move_selection_up(&mut self.setting_index, &mut self.setting_offset),
            Tab::Logs => self.log_scroll = (self.log_scroll + 1).min(self.logs.len()),
        }
    }

    fn move_down(&mut self) {
        match self.tab {
            Tab::Home => move_selection_down(
                &mut self.action_index,
                &mut self.action_offset,
                ACTIONS.len(),
                self.action_height,
            ),
            Tab::Library => move_selection_down(
                &mut self.game_index,
                &mut self.game_offset,
                self.games.len(),
                self.game_height,
            ),
            Tab::Settings => move_selection_down(
                &mut self.setting_index,
                &mut self.setting_offset,
                SETTINGS.len(),
                self.setting_height,
            ),
            Tab::Logs => self.log_scroll = self.log_scroll.saturating_sub(1),
        }
    }

    fn toggle_game(&mut self) {
        if let Some(game) = self.games.get(self.game_index)
            && !self.selected_games.remove(game)
        {
            self.selected_games.insert(game.clone());
        }
    }

    fn edit_setting(&mut self) {
        match self.setting_index {
            7 => self.config.archive.enabled = !self.config.archive.enabled,
            9 => self.config.archive.move_to_vault = !self.config.archive.move_to_vault,
            11 => self.config.archive.delete_source = !self.config.archive.delete_source,
            12 => self.save_config(),
            _ => {
                self.input = self.setting_value(self.setting_index);
                self.editing = true;
            }
        }
    }

    fn handle_edit_key(&mut self, key: KeyEvent) {
        match key.code {
            KeyCode::Esc => self.editing = false,
            KeyCode::Enter => {
                self.apply_input();
                self.editing = false;
            }
            KeyCode::Backspace => {
                self.input.pop();
            }
            KeyCode::Char(character) => self.input.push(character),
            _ => {}
        }
    }

    fn apply_input(&mut self) {
        let value = self.input.clone();
        match self.setting_index {
            0 => self.config.downloader = value.into(),
            1 => self.config.download_dir = value.into(),
            2 => {
                if let Ok(threads) = value.parse::<u16>() {
                    self.config.threads = threads.max(1);
                }
            }
            3 => self.config.platform = value,
            4 => self.config.language = value,
            5 => self.config.include = value,
            6 => self.config.exclude = value,
            8 => self.config.archive.output_dir = value.into(),
            10 => self.config.archive.vault_dir = value.into(),
            _ => {}
        }
    }

    fn save_config(&mut self) {
        self.status = match self.config.save() {
            Ok(()) => format!("Saved {}", Config::path().display()),
            Err(error) => format!("Failed to save config: {error:#}"),
        };
    }

    fn setting_value(&self, index: usize) -> String {
        match index {
            0 => self.config.downloader.display().to_string(),
            1 => self.config.download_dir.display().to_string(),
            2 => self.config.threads.to_string(),
            3 => self.config.platform.clone(),
            4 => self.config.language.clone(),
            5 => self.config.include.clone(),
            6 => self.config.exclude.clone(),
            7 => on_off(self.config.archive.enabled).into(),
            8 => self.config.archive.output_dir.display().to_string(),
            9 => on_off(self.config.archive.move_to_vault).into(),
            10 => self.config.archive.vault_dir.display().to_string(),
            11 => on_off(self.config.archive.delete_source).into(),
            12 => Config::path().display().to_string(),
            _ => String::new(),
        }
    }

    pub fn draw(&mut self, frame: &mut Frame) {
        let areas = Layout::default()
            .direction(Direction::Vertical)
            .constraints([
                Constraint::Length(3),
                Constraint::Min(5),
                Constraint::Length(2),
            ])
            .split(frame.area());
        self.tab_area = areas[0];
        self.content_area = areas[1];
        self.draw_tabs(frame, areas[0]);
        match self.tab {
            Tab::Home => self.draw_home(frame, areas[1]),
            Tab::Library => self.draw_library(frame, areas[1]),
            Tab::Settings => self.draw_settings(frame, areas[1]),
            Tab::Logs => self.draw_logs(frame, areas[1]),
        }
        let style = if self.busy {
            Style::default().fg(Color::Yellow)
        } else {
            Style::default().fg(Color::DarkGray)
        };
        frame.render_widget(Paragraph::new(self.status.as_str()).style(style), areas[2]);
    }

    fn draw_tabs(&self, frame: &mut Frame, area: Rect) {
        let selected = match self.tab {
            Tab::Home => 0,
            Tab::Library => 1,
            Tab::Settings => 2,
            Tab::Logs => 3,
        };
        let tabs = Tabs::new(["Home", "Library", "Settings", "Logs"])
            .select(selected)
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" LGOGDownloader TUI "),
            )
            .highlight_style(
                Style::default()
                    .fg(Color::Cyan)
                    .add_modifier(Modifier::BOLD),
            );
        frame.render_widget(tabs, area);
    }

    fn draw_home(&mut self, frame: &mut Frame, area: Rect) {
        let selected = self.selected_games.len();
        let archive = if self.config.archive.enabled {
            let destination = if self.config.archive.move_to_vault {
                self.config.archive.vault_dir.display().to_string()
            } else {
                self.config.archive.output_dir.display().to_string()
            };
            format!("enabled -> {destination}")
        } else {
            "disabled".into()
        };
        let areas = Layout::default()
            .direction(Direction::Horizontal)
            .constraints([Constraint::Percentage(48), Constraint::Percentage(52)])
            .split(area);
        let items: Vec<ListItem> = ACTIONS
            .iter()
            .map(|action| ListItem::new(*action))
            .collect();
        let actions = List::new(items)
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" Actions: Up/Down + Enter "),
            )
            .highlight_symbol("> ")
            .highlight_style(
                Style::default()
                    .bg(Color::DarkGray)
                    .fg(Color::Cyan)
                    .add_modifier(Modifier::BOLD),
            );
        let mut state = ListState::default()
            .with_selected(Some(self.action_index))
            .with_offset(self.action_offset);
        frame.render_stateful_widget(actions, areas[0], &mut state);
        self.action_offset = state.offset();
        self.action_height = areas[0].height.saturating_sub(2) as usize;

        let text = vec![
            Line::from(Span::styled(
                "Current configuration",
                Style::default()
                    .fg(Color::Cyan)
                    .add_modifier(Modifier::BOLD),
            )),
            Line::from(""),
            Line::from(format!(
                "Library: {} games, {selected} selected",
                self.games.len()
            )),
            Line::from(format!(
                "Download directory: {}",
                self.config.download_dir.display()
            )),
            Line::from(format!("Archive: {archive}")),
            Line::from(""),
            Line::from(Span::styled(
                action_description(self.action_index),
                Style::default().fg(Color::Yellow),
            )),
        ];
        frame.render_widget(
            Paragraph::new(text)
                .block(Block::default().borders(Borders::ALL).title(" Summary "))
                .wrap(Wrap { trim: false }),
            areas[1],
        );
    }

    fn draw_library(&mut self, frame: &mut Frame, area: Rect) {
        let items: Vec<ListItem> = self
            .games
            .iter()
            .map(|game| {
                let marker = if self.selected_games.contains(game) {
                    "[x]"
                } else {
                    "[ ]"
                };
                ListItem::new(format!("{marker} {game}"))
            })
            .collect();
        let list = List::new(items)
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" Library: Up/Down select, Enter/Space toggle "),
            )
            .highlight_symbol("> ")
            .highlight_style(Style::default().bg(Color::DarkGray).fg(Color::White));
        let mut state = ListState::default()
            .with_selected(if self.games.is_empty() {
                None
            } else {
                Some(self.game_index)
            })
            .with_offset(self.game_offset);
        frame.render_stateful_widget(list, area, &mut state);
        self.game_offset = state.offset();
        self.game_height = area.height.saturating_sub(2) as usize;
    }

    fn draw_settings(&mut self, frame: &mut Frame, area: Rect) {
        let items: Vec<ListItem> = SETTINGS
            .iter()
            .enumerate()
            .map(|(index, label)| {
                let value = if self.editing && index == self.setting_index {
                    format!("{}_", self.input)
                } else {
                    self.setting_value(index)
                };
                ListItem::new(Line::from(vec![
                    Span::styled(format!("{label:<28}"), Style::default().fg(Color::Cyan)),
                    Span::raw(value),
                ]))
            })
            .collect();
        let list = List::new(items)
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" Settings: Enter edit/toggle, Ctrl+S save "),
            )
            .highlight_symbol("> ")
            .highlight_style(Style::default().bg(Color::DarkGray).fg(Color::White));
        let mut state = ListState::default()
            .with_selected(Some(self.setting_index))
            .with_offset(self.setting_offset);
        frame.render_stateful_widget(list, area, &mut state);
        self.setting_offset = state.offset();
        self.setting_height = area.height.saturating_sub(2) as usize;
    }

    fn draw_logs(&self, frame: &mut Frame, area: Rect) {
        let height = area.height.saturating_sub(2) as usize;
        let end = self.logs.len().saturating_sub(self.log_scroll);
        let start = end.saturating_sub(height);
        let text = self.logs[start..end].join("\n");
        frame.render_widget(
            Paragraph::new(text)
                .block(
                    Block::default()
                        .borders(Borders::ALL)
                        .title(" Live output: Up/Down scroll, c clear, Esc Home "),
                )
                .wrap(Wrap { trim: false }),
            area,
        );
    }
}

fn on_off(value: bool) -> &'static str {
    if value { "ON" } else { "OFF" }
}

fn action_description(index: usize) -> &'static str {
    match index {
        0 => "Refresh the games available in your GOG account.",
        1 => "Browse and select games from the loaded library.",
        2 => "Download the files for every selected game.",
        3 => "Check selected downloads and repair damaged files.",
        4 => "Verify files: healthy, missing, incomplete, or different version.",
        5 => "Sign in using email, password, and a verification code.",
        6 => "Sign in through a browser as a fallback.",
        7 => "Refresh LGOGDownloader's local metadata cache.",
        8 => "Configure downloads and optional archive processing.",
        9 => "Inspect task messages and problem details.",
        10 => "Exit LGOGDownloader TUI.",
        _ => "",
    }
}

fn move_selection_up(selected: &mut usize, offset: &mut usize) {
    *selected = selected.saturating_sub(1);
    let top_margin = offset.saturating_add(LIST_SCROLL_PADDING);
    if *selected < top_margin {
        *offset = selected.saturating_sub(LIST_SCROLL_PADDING);
    }
}

fn move_selection_down(selected: &mut usize, offset: &mut usize, len: usize, height: usize) {
    if len == 0 {
        return;
    }
    *selected = (*selected + 1).min(len - 1);
    if height == 0 {
        return;
    }
    let bottom_margin_row = height.saturating_sub(LIST_SCROLL_PADDING + 1);
    if selected.saturating_sub(*offset) > bottom_margin_row {
        *offset = selected.saturating_sub(bottom_margin_row);
    }
    *offset = (*offset).min(len.saturating_sub(height));
}

#[cfg(test)]
mod tests {
    use super::{move_selection_down, move_selection_up};

    #[test]
    fn reverse_direction_moves_cursor_before_scrolling_view() {
        let (mut selected, mut offset) = (17, 10);
        move_selection_up(&mut selected, &mut offset);
        assert_eq!((selected, offset), (16, 10));
        move_selection_up(&mut selected, &mut offset);
        assert_eq!((selected, offset), (15, 10));
        move_selection_up(&mut selected, &mut offset);
        assert_eq!((selected, offset), (14, 10));
        move_selection_up(&mut selected, &mut offset);
        assert_eq!((selected, offset), (13, 10));
        move_selection_up(&mut selected, &mut offset);
        assert_eq!((selected, offset), (12, 9));
    }

    #[test]
    fn downward_navigation_keeps_three_visible_items_below() {
        let (mut selected, mut offset) = (5, 0);
        move_selection_down(&mut selected, &mut offset, 30, 10);
        assert_eq!((selected, offset), (6, 0));
        move_selection_down(&mut selected, &mut offset, 30, 10);
        assert_eq!((selected, offset), (7, 1));
    }
}
