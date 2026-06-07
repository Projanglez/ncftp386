/* =============================================================================
 * dialog.h - Modale Dialoge fuer NCFTP386
 * -----------------------------------------------------------------------------
 * Jeder Dialog zeichnet sich zentriert ueber den aktuellen Bildschirm, faehrt
 * seine eigene Tastaturschleife und stellt den Bildschirm beim Schliessen
 * wieder her - der Aufrufer muss NICHT neu zeichnen.
 *
 * Mehrzeilige Nachrichten: Zeilen mit '\n' trennen (max. DLG_MAX_LINES).
 * ===========================================================================*/
#ifndef DIALOG_H
#define DIALOG_H

#define DLG_MAX_LINES 8     /* max. Textzeilen in Message-/Confirm-Dialogen */

/* Hinweis-/Fehlerbox mit einem [ OK ]-Knopf. is_error!=0 -> rote Darstellung.
 * Schliesst bei Enter, Esc oder Leertaste. */
void dlg_message(const char *title, const char *msg, int is_error);
void dlg_error(const char *title, const char *msg);   /* = dlg_message(...,1)  */

/* Ja/Nein-Abfrage. Rueckgabe: 1 = Ja, 0 = Nein.
 * J/N als Direkttasten, Links/Rechts/Tab wechselt, Enter waehlt, Esc = Nein. */
int dlg_confirm(const char *title, const char *msg);

/* Wie dlg_confirm, aber mit waehlbarem Vorgabe-Fokus: default_yes != 0 ->
 * "Ja" ist vorausgewaehlt (fuer Opt-out-Abfragen wie "Passwort speichern?"). */
int dlg_confirm_def(const char *title, const char *msg, int default_yes);

/* Einzeiliges Eingabefeld (Basis fuer den Verbindungsdialog).
 * buf wird vorbefuellt angezeigt und editiert; maxlen = max. Zeichen (buf muss
 * maxlen+1 gross sein). is_password!=0 zeigt '*'. Rueckgabe: 1 = OK, 0 = Abbruch. */
int dlg_input(const char *title, const char *prompt,
              char *buf, int maxlen, int is_password);

/* Nicht-modaler Fortschrittsdialog fuer Datentransfers (F5).
 * Ablauf: begin() oeffnet die Box (sichert den Bildschirm), update() wird
 * waehrend des blockierenden Transfers aus dem FtpProgressCb aufgerufen,
 * end() schliesst die Box und stellt den Bildschirm wieder her.
 *   total >  0 : Fortschrittsbalken mit Prozentanzeige
 *   total == 0 : Groesse unbekannt -> nur uebertragene Bytes
 * update() zeichnet aus Effizienzgruenden nur bei sichtbarer Aenderung neu. */
void dlg_progress_begin(const char *title, const char *fromname);
void dlg_progress_update(unsigned long sofar, unsigned long total);
void dlg_progress_end(void);

/* Den angezeigten Dateinamen waehrend eines laufenden Fortschrittsdialogs
 * wechseln (fuer Stapel-/rekursive Kopiervorgaenge). Setzt den Balken auf 0%
 * zurueck. Wirkungslos, wenn kein Fortschrittsdialog offen ist. */
void dlg_progress_setfile(const char *name);

/* Vertikales Auswahlmenue (modal). Zeigt 'count' Eintraege, anfangs ist
 * 'initial' hervorgehoben. Pfeile/Pos1/Ende bewegen, Enter waehlt, Esc bricht
 * ab; ein Buchstabe springt direkt zum ersten Eintrag mit diesem Anfangs-
 * zeichen und waehlt ihn. Rueckgabe: gewaehlter Index 0..count-1 oder -1. */
int dlg_menu(const char *title, const char *const *items, int count, int initial);

/* Auswahldialog: mehrzeilige Nachricht 'msg' oben, darunter eine vertikale
 * Liste von 'count' Optionen. Pfeile bewegen, Enter waehlt, Esc bricht ab.
 * Rueckgabe: gewaehlter Optionsindex 0..count-1, oder -1 bei Esc. */
int dlg_choice(const char *title, const char *msg,
               const char *const *items, int count);

#endif /* DIALOG_H */
