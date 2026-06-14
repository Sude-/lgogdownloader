# LGOGDownloader TUI

A Ratatui frontend for the `lgogdownloader` command-line application.

## Features

- List and multi-select games from your GOG library
- Download, repair, and verify selected games
- Native email/password login with email-code and TOTP verification support
- Visible cursor and masked password typing feedback during native login
- Browser login fallback with a clean terminal handoff for URL copying/pasting
- Configure downloader path, download directory, threads, platform, language,
  includes, and excludes
- Optionally create ZIP archives after downloads
- Optionally move archives to a separate GameVault directory
- Optionally remove source downloads after a successful archive
- Persistent configuration and live command output
- Margin-aware list scrolling that keeps nearby items visible while navigating
- Responsive background tasks with bounded log processing
- Clickable tabs, actions, games, and settings
- Mouse-wheel navigation for lists and logs
- Clean single-line cache-update progress instead of terminal-control log spam

## Build and run

Install `lgogdownloader`, then:

```sh
cd tui
cargo run --release
```

The executable can also be installed from this checkout:

```sh
cargo install --path tui
lgogdownloader-tui
```

## Keys

| Key | Action |
| --- | --- |
| `Left` / `Right`, `Tab` / `Shift+Tab` | Switch tabs |
| `Up` / `Down` | Navigate actions, games, settings, or logs |
| `Enter` | Activate an action, select a game, or edit a setting |
| `Space` | Select a game in the Library |
| `Ctrl+S` | Save settings immediately |
| `Home` / `End` | Oldest/latest log output |
| `c` | Clear the Logs tab |
| `Esc` | Return to Home |
| `q` | Quit |

The standard login action runs `lgogdownloader --login` and is the default
choice. It prompts for the GOG email, password, and any required email or TOTP
verification code. Browser login remains available as a fallback.
After either login flow, the TUI verifies that LGOGDownloader actually saved a
working session before reporting success.

Actions that require authentication check login status first and direct the
user back to the Home tab instead of showing a generic downloader failure.

**Verify selected downloads** checks local files against GOG and summarizes
them as healthy, missing, incomplete, or a different version. Only problematic
file details are added to Logs.

Archiving, moving to GameVault, and deleting the downloaded source are disabled
by default and configured independently in the Settings tab. Settings are saved
with the selectable **Save settings** row or `Ctrl+S`, to the platform
configuration directory under `lgogdownloader-tui/config.toml`.
