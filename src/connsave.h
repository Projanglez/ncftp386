/* =============================================================================
 * connsave.h - Letzte FTP-Verbindung persistent merken
 * -----------------------------------------------------------------------------
 * Speichert Host/Port/Benutzer/Passwort der zuletzt erfolgreich aufgebauten
 * Verbindung in einer kleinen Textdatei NCFTP.SAV neben der EXE und laedt sie
 * beim naechsten Start. Nur EINE (die letzte) Verbindung.
 *
 * Sicherheit (bewusst "angemessen", nicht maximal): FTP uebertraegt das
 * Passwort ohnehin im Klartext, die Zielhardware ist Single-User-Retro-DOS
 * (physischer Zugriff = Vollzugriff). Das Passwort wird daher nur LEICHT
 * verschleiert (XOR mit festem Schluessel + Hex) - das schuetzt gegen
 * fluechtiges Mitlesen der Datei, ist aber KEINE echte Verschluesselung.
 * ===========================================================================*/
#ifndef CONNSAVE_H
#define CONNSAVE_H

/* Speicherpfad einmalig festlegen: Verzeichnis der EXE (aus argv[0]),
 * Fallback aktuelles Verzeichnis beim Start (vor jeder Navigation). */
void connsave_init(const char *argv0);

/* Gespeicherte Verbindung laden. Felder werden nur ueberschrieben, wenn in der
 * Datei vorhanden; fehlende bleiben unveraendert (Aufrufer setzt Defaults).
 * *savepw erhaelt die gespeicherte "Passwort merken"-Einstellung (0/1).
 * Rueckgabe: 1 = Datei gelesen, 0 = keine Datei / nicht lesbar. */
int  connsave_load(char *host, int hostsz, char *port, int portsz,
                   char *user, int usersz, char *pass, int passsz, int *savepw);

/* Verbindung speichern. Host/Port/Benutzer werden immer abgelegt; das Passwort
 * nur bei savepw != 0 (sonst bleibt die pass-Zeile leer). */
void connsave_store(const char *host, const char *port,
                    const char *user, const char *pass, int savepw);

#endif /* CONNSAVE_H */
