# NCFTP386

Ein Norton-Commander-artiger **Dual-Panel-FTP-Client für MS-DOS** auf echter
386-Hardware. Links das lokale DOS-Dateisystem, rechts ein FTP-Server über den
[mTCP](https://github.com/mbbrutman/mTCP)-TCP/IP-Stack — vollständig
tastaturgesteuert im 80×25-Textmodus.

```
+=====================+        +=====================+
| C:\GAMES            |        | ftp.example.org /pub|
+=====================+        +=====================+
| Name      Groesse D |        | Name      Groesse D |
| ..                  |        | ..                  |
| ULTIMA    <DIR>  07 |        | games     <DIR>  01 |
| readme.txt  1234 07 |        | readme.txt 512   01 |
+=====================+        +=====================+
 1Hilfe 2Verb 3Anzeig 4Edit 5Kopier 6Umben 7MkDir 8Loesch 9Laufw 10Ende
```

## Funktionen

- Zwei Panels: lokal (DOS) und remote (FTP, PASV-Modus)
- Navigieren, Verzeichnisse betreten, Dateien ansehen (F3-Viewer)
- Kopieren in beide Richtungen (F5), **inkl. rekursiver Verzeichnisbäume**
- Mehrfachauswahl mit der **Einfg-Taste** (Norton-Stil) für Kopieren/Löschen
- Per-Datei-Abfrage bei Überschreib-Konflikten
  (Überschreiben / Überspringen / Alle / Abbrechen)
- Anlegen (F7), Umbenennen (F6), **rekursives Löschen** mit Vorab-Zählung (F8)
- Lokaler Laufwerkswechsel (F9), zeigt nur vorhandene Laufwerke
- Zweisprachig Deutsch/Englisch (automatisch über die DOS-Ländereinstellung,
  oder erzwungen per Kommandozeile: `NCFTP EN`)

## Voraussetzungen zum Bauen

- [Open Watcom C/C++](http://www.openwatcom.org/) (wmake, wpp, wcc, wasm, wlink)
- Windows- oder DOS-Host für die Cross-Kompilierung
- Ziel: 16-bit Real-Mode DOS, Large Memory Model, 386

## Bauen

mTCP ist eine **externe Abhängigkeit** und nicht Teil dieses Repositorys.
Zuerst den verwendeten Fork in den Ordner `mtcp/` klonen:

```sh
git clone https://github.com/retrohun/mTCP mtcp
```

Dann mit Open Watcom bauen:

```sh
wmake          # baut NCFTP.EXE
wmake clean    # entfernt Objekte und Build-Artefakte
```

Wichtig: mTCP wird mit `-0` (8086) übersetzt, der eigene Anwendungscode mit
`-3` (386). Details und Begründung stehen im `MAKEFILE` und in `CLAUDE.md`.

## Ausführen (auf dem DOS-Rechner)

Ein laufender Packet-Treiber für die Netzwerkkarte und eine mTCP-Konfiguration
werden vorausgesetzt:

```bat
SET MTCPCFG=C:\NET\MTCP.CFG
NCFTP.EXE
REM Englische Oberflaeche erzwingen:
NCFTP.EXE EN
```

## Tastenbelegung (Auszug)

| Taste | Funktion |
|-------|----------|
| Tab | Aktives Panel wechseln |
| Pfeile / Bild ↑↓ | Auswahl bewegen |
| Einfg | Eintrag markieren (mehrere kopieren/löschen) |
| Enter | Verzeichnis betreten / Datei anzeigen |
| Backspace | Übergeordnetes Verzeichnis |
| F2 | FTP verbinden / trennen |
| F3 | Anzeigen · F5 Kopieren · F6 Umbenennen |
| F7 | Verzeichnis erstellen · F8 Löschen · F9 Laufwerk |
| F10 | Beenden |

## Lizenz

Dieses Projekt steht unter der **GNU General Public License v3.0** (siehe
[`LICENSE`](LICENSE)). Es linkt statisch die mTCP-Bibliothek, die ebenfalls
unter der GPLv3 steht.

mTCP © Michael B. Brutman — <https://www.brutman.com/mTCP/>
