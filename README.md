# FTP4DOS

A Norton Commander-style **dual-panel FTP client for MS-DOS** running on any
x86 machine. The left panel shows the local DOS filesystem; the right panel
connects to an FTP server via the
[mTCP](http://www.brutman.com/mTCP/mTCP.html) TCP/IP stack — fully
keyboard-driven in 80×25 text mode.

![FTP4DOS downloading DOOM.EXE](docs/screenshot.png)

Download latest release here: <https://github.com/Projanglez/ftp4dos/releases/latest>

## Features

- Two panels: local (DOS) and remote (FTP, passive mode)
- Copy in both directions (F5), including **recursive directory trees**
- Multiple selection with the **Ins key** (Norton style) for copy/delete
- Move (F6) and rename (Alt+F6); **recursive** move/copy/delete for whole directory trees
- Create directories (F7), **recursive delete** with pre-count confirmation (F8)
- Swap the two panels left/right with Ctrl+U (remembered across launches)
- Navigate directories; view files with F3 (or Enter) — up to 32 KB displayed
- Remote view (F3) downloads to a temporary file first, then opens the viewer
- Edit local text files with F4 — minimal full-screen editor (~32 KB, local only)
- FTP connection keepalive: sends NOOP every 60 s to prevent server idle timeouts
- Bilingual German/English UI (auto-detected from DOS country setting, or forced on the command line: `FTP4DOS /L:EN`)

## Build requirements

- [Open Watcom C/C++](http://www.openwatcom.org/) (wmake, wpp, wcc, wasm, wlink)
- Windows or DOS host for cross-compilation
- Target: 16-bit real-mode DOS, Large memory model, 8086+

## Building

mTCP is an **external dependency** and is not part of this repository.
This project builds against the **official mTCP, version 2025-01-10**.
Download the source archive from the official mTCP home page (there is no
official git repository) and extract it into the `mtcp/` directory:

- mTCP home page: <http://www.brutman.com/mTCP/mTCP.html>

After extracting, the layout must be `mtcp/TCPLIB/`, `mtcp/TCPINC/`,
`mtcp/INCLUDE/` and `mtcp/APPS/FTP/`. Always use the current official
release rather than a third-party mirror.

Then build with Open Watcom:

```sh
wmake          # produces FTP4DOS.EXE
wmake clean    # removes objects and build artifacts
```

Note: mTCP is compiled with `-0` (8086), and the application code likewise
uses `-0` (compatible with 8086/286/386+). Details are in `MAKEFILE` and `CLAUDE.md`.

## Running (on the DOS machine)

A packet driver for your network card and an valid mTCP configuration file are required, as well as a valid IP-adress (static or dynamic) via mTCP.

```bat
FTP4DOS.EXE
```

### Command-line parameters

```
FTP4DOS [/L:DE|EN] [/H:HOST] [/P:PORT] [/U:USER] [/W:PASS] [/S:ALL|NOPASS|OFF] [/MONO|/COLOR]   (or /?)
```

Both `/` and `-` are accepted as the flag prefix. Flags are **case-insensitive**;
values are passed through as-is (username and password are case-sensitive).

| Parameter | Description |
|-----------|-------------|
| `/L:DE` / `/L:EN` | Force German or English UI |
| `/H:HOST` | Connect to HOST automatically on startup |
| `/P:PORT` | Port (default 21) |
| `/U:USER` | Username (default `anonymous`) |
| `/W:PASS` | Password |
| `/S:ALL` | Save connection including password to `FTP4DOS.SAV` (default) |
| `/S:NOPASS` | Save connection but not the password |
| `/S:OFF` | Do not save this connection |
| `/?` | Show brief help |

### Saved connection

After a successful connection, host/port/username (and optionally the password)
are stored in `FTP4DOS.SAV` next to the EXE and pre-filled on the next launch.
Use `/S:ALL` (default), `/S:NOPASS`, or `/S:OFF` to control what gets saved;
the connect dialog offers the same three choices interactively.

**Security note:** The stored password is lightly obfuscated (XOR + hex), not
encrypted — and FTP transmits passwords in plain text anyway.

## Key bindings

| Key | Action |
|-----|--------|
| Tab | Switch active panel |
| Ctrl+U | Swap panels left/right (remembered) |
| Arrow keys / PgUp PgDn | Move selection |
| Ins | Mark entry (for multi-file copy/delete) |
| * (numpad) | Invert selection |
| + (numpad) | Mark files missing or different in the other panel |
| Enter | Enter directory / view file (same as F3) |
| Backspace | Go to parent directory |
| F1 | Help |
| F2 | FTP connect / disconnect |
| F3 | View file (local or remote; max 32 KB) |
| F4 | Edit local file (minimal editor, ~32 KB, no undo/search) |
| F5 | Copy (recursive for directories) |
| F6 | Move (copy then delete source; recursive) |
| Alt+F6 | Rename (in place) |
| F7 | Create directory |
| F8 | Delete (recursive with confirmation) |
| F9 / Alt+F1 | Switch local drive |
| F10 | Quit |

## License

This project is licensed under the **GNU General Public License v3.0**
(see [`LICENSE`](LICENSE)). It statically links the mTCP library, which is
also licensed under the GPLv3.

### Third-party code / corresponding source

mTCP © Michael B. Brutman — official home page:
<http://www.brutman.com/mTCP/mTCP.html>

This project builds against the **official mTCP, version 2025-01-10**,
unmodified, obtained from the mTCP home page above. That exact source,
together with the source in this repository, constitutes the complete
corresponding source for any distributed binary (GPLv3 §6). Published
releases include a copy of those mTCP sources as an additional release asset.

Please download mTCP from the official home page to obtain the current
version; always prefer the official release over any third-party mirror.

## Disclaimer

This software is provided without any warranty; use at your own risk. See [LICENSE](LICENSE) (GPLv3, §15–16).

## Development note

This software was developed with the help of an AI coding assistant (Claude Code).
