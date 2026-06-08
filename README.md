# NCFTP386

A Norton Commander-style **dual-panel FTP client for MS-DOS** running on real
386 hardware. The left panel shows the local DOS filesystem; the right panel
connects to an FTP server via the
[mTCP](https://github.com/retrohun/mTCP) TCP/IP stack — fully
keyboard-driven in 80×25 text mode.

![NCFTP386 downloading DOOM.EXE](docs/screenshot.png)

```
+=====================+        +=====================+
| C:\GAMES            |        | ftp.example.org /pub|
+=====================+        +=====================+
| Name      Size    D |        | Name      Size    D |
| ..                  |        | ..                  |
| ULTIMA    <DIR>  07 |        | games     <DIR>  01 |
| readme.txt  1234 07 |        | readme.txt 512   01 |
+=====================+        +=====================+
 1Help  2Conn  3View  4Edit  5Copy  6Rename 7MkDir 8Del  9Drive 10Quit
```

## Features

- Two panels: local (DOS) and remote (FTP, passive mode)
- Navigate directories; view files with F3 (or Enter) — up to 32 KB displayed
- Remote view (F3) downloads to a temporary file first, then opens the viewer
- Edit local text files with F4 — minimal full-screen editor (~32 KB, local only)
- Copy in both directions (F5), including **recursive directory trees**
- Multiple selection with the **Ins key** (Norton style) for copy/delete
- Per-file overwrite prompts (Overwrite / Skip / All / Cancel)
- Create (F7), rename (F6), **recursive delete** with pre-count confirmation (F8)
- Local drive switcher (F9), lists only present drives
- Keepalive: sends NOOP every 60 s to prevent server idle timeouts
- Bilingual German/English UI (auto-detected from DOS country setting,
  or forced on the command line: `NCFTP386 /L:EN`)

## Build requirements

- [Open Watcom C/C++](http://www.openwatcom.org/) (wmake, wpp, wcc, wasm, wlink)
- Windows or DOS host for cross-compilation
- Target: 16-bit real-mode DOS, Large memory model, 8086+

## Building

mTCP is an **external dependency** and is not part of this repository.
Clone the fork used by this project into the `mtcp/` directory and check out
the exact commit against which it was built and tested:

```sh
git clone https://github.com/retrohun/mTCP mtcp
git -C mtcp checkout ad9cd0f
```

Then build with Open Watcom:

```sh
wmake          # produces NCFTP386.EXE
wmake clean    # removes objects and build artifacts
```

Note: mTCP is compiled with `-0` (8086), and the application code likewise
uses `-0` (compatible with 8086/286/386+). Details are in `MAKEFILE` and `CLAUDE.md`.

## Running (on the DOS machine)

A packet driver for your network card and an mTCP configuration file are required:

```bat
SET MTCPCFG=C:\NET\MTCP.CFG
NCFTP386.EXE
```

### Command-line parameters

```
NCFTP386 [/L:DE|EN] [/H:HOST] [/P:PORT] [/U:USER] [/W:PASS] [/S:ALL|NOPASS|OFF]   (or /?)
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
| `/S:ALL` | Save connection including password to `NCFTP386.SAV` (default) |
| `/S:NOPASS` | Save connection but not the password |
| `/S:OFF` | Do not save this connection |
| `/?` | Show brief help |

### Saved connection

After a successful connection, host/port/username (and optionally the password)
are stored in `NCFTP386.SAV` next to the EXE and pre-filled on the next launch.
Use `/S:ALL` (default), `/S:NOPASS`, or `/S:OFF` to control what gets saved;
the connect dialog offers the same three choices interactively.

**Security note:** The stored password is lightly obfuscated (XOR + hex), not
encrypted — and FTP transmits passwords in plain text anyway.

## Key bindings

| Key | Action |
|-----|--------|
| Tab | Switch active panel |
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
| F6 | Rename / move |
| F7 | Create directory |
| F8 | Delete (recursive with confirmation) |
| F9 | Switch local drive |
| F10 | Quit |

## License

This project is licensed under the **GNU General Public License v3.0**
(see [`LICENSE`](LICENSE)). It statically links the mTCP library, which is
also licensed under the GPLv3.

### Third-party code / corresponding source

mTCP © Michael B. Brutman — <https://www.brutman.com/mTCP/>

This project uses the fork **[retrohun/mTCP](https://github.com/retrohun/mTCP)**,
unmodified at commit
[`ad9cd0f`](https://github.com/retrohun/mTCP/commit/ad9cd0f87ab151a1ce6f1a279f306bdf94163b21)
(2018-08-20). That exact revision, together with the source in this repository,
constitutes the complete corresponding source for any distributed binary
(GPLv3 §6). Published releases include a copy of those mTCP sources as an
additional release asset.

## Disclaimer

This software is provided without any warranty; use at your own risk. See [LICENSE](LICENSE) (GPLv3, §15–16).
