/* =============================================================================
 * panel.h - Abstrakte Panel-Basisklasse fuer NCFTP386
 * -----------------------------------------------------------------------------
 * Gemeinsame Logik der beiden Dateilisten-Panels (lokal & FTP-remote):
 *   - Eintragsliste (Name, Groesse, Datum, Verzeichnis-Flag)
 *   - Cursor (markierter Eintrag) + Scroll-Offset
 *   - Tastatur-Navigation (auf/ab, seitenweise, Pos1/Ende)
 *   - Komplettes Zeichnen in eine Bildschirmregion (Norton-Commander-Look)
 *
 * refresh() ist rein virtuell: lokales Panel liest das DOS-Dateisystem,
 * FTP-Panel die Remote-LIST-Ausgabe. Der Rest ist hier gemeinsam.
 *
 * Speicherhinweis: jedes Panel-Objekt enthaelt das Eintrags-Array direkt
 * (~25 KB). Beim Linken (Schritt 3) ggf. -zt<n> setzen, damit Open Watcom die
 * grossen Objekte in FAR-Datensegmente legt und DGROUP (64 KB) nicht ueberlaeuft.
 * ===========================================================================*/
#ifndef PANEL_H
#define PANEL_H

#define PANEL_NAME_MAX    40   /* haelt 8.3 lokal UND laengere FTP-Namen      */
#define PANEL_MAX_ENTRIES 512  /* max. Eintraege pro Verzeichnis              */
#define PANEL_HEADER_MAX  80   /* Pfad-/Titelzeile (DOS-Pfad max. ~64+Drive)  */

/* Ein Verzeichniseintrag (POD - per qsort umsortierbar). */
struct PanelEntry {
    char          name[PANEL_NAME_MAX];
    unsigned long size;       /* Groesse in Bytes (bei Verzeichnis 0)         */
    unsigned      date;       /* DOS-Datumswort (Bits: JJJJJJJ MMMM DDDDD)    */
    unsigned      time;       /* DOS-Zeitwort  (Bits: HHHHH MMMMMM SSSSS)     */
    unsigned char is_dir;     /* 1 = Verzeichnis                              */
    unsigned char is_parent;  /* 1 = ".."-Eintrag (sortiert immer zuerst)     */
    unsigned char marked;     /* 1 = vom Benutzer markiert (Einfg-Taste)      */
};

class Panel {
public:
    Panel();
    virtual ~Panel();

    /* Bildschirmregion festlegen (Zeile/Spalte oben-links, Hoehe, Breite). */
    void set_region(int top_, int left_, int height_, int width_);

    /* Aktiv-Status (Cursor-Hervorhebung + Header-Farbe). */
    void set_active(int a) { active = a; }
    int  is_active() const { return active; }

    /* Inhalt neu einlesen - von der Unterklasse implementiert.
     * Rueckgabe: Anzahl Eintraege. */
    virtual int refresh() = 0;

    /* Aktionen (von der Unterklasse ueberschrieben; Basis = wirkungslos).
     * enter_selected(): Enter auf markiertem Eintrag.
     *   Rueckgabe 1 = Verzeichnis betreten (Liste neu), 0 = Datei/keine Aktion.
     * go_parent(): ins uebergeordnete Verzeichnis wechseln. */
    virtual int  enter_selected();
    virtual void go_parent();

    /* Gesamtes Panel zeichnen (Rahmen, Header, Spalten, Eintraege). */
    void draw();

    /* Navigation (aktualisieren nur den Zustand; Aufrufer ruft danach draw()). */
    void move_up();
    void move_down();
    void page_up();
    void page_down();
    void move_home();
    void move_end();

    /* Cursor um delta (+1/-1) bewegen und FLIMMERFREI zeichnen: bei reiner
     * Bewegung innerhalb des sichtbaren Bereichs werden nur die alte und die
     * neue Cursorzeile neu gemalt; nur beim Scrollen am Rand der Vollaufbau. */
    void move_step(int delta);

    /* Aktuell markierter Eintrag (0 falls Liste leer). */
    PanelEntry *selected();
    int         selected_index() const { return cursor; }
    int         entry_count()    const { return count; }

    /* Cursor auf den Eintrag mit diesem Namen setzen (case-insensitiv). Wird er
     * nicht gefunden, bleibt der Cursor im gueltigen Bereich. Fuer "Cursor nach
     * der Operation auf dem Element halten". */
    void select_by_name(const char *name);
    /* Cursor direkt auf einen Index setzen (wird in den gueltigen Bereich
     * geklemmt). Fuer "nach dem Loeschen in der Naehe bleiben". */
    void set_cursor_index(int idx);

    /* Eintrag per Index (0 ausserhalb des gueltigen Bereichs). */
    PanelEntry *entry_at(int i);

    /* --- Mehrfachauswahl (Norton-Commander-Manier, Einfg-Taste) --- */
    /* Markierung des aktuellen Eintrags umschalten und Cursor nach unten.
     * Der ".."-Eintrag laesst sich nicht markieren. */
    void          toggle_mark();
    void          clear_marks();
    int           marked_count()     const; /* Anzahl markierter Eintraege      */
    unsigned long marked_size()      const; /* Summe der Groessen (nur Dateien) */
    int           marked_dir_count() const; /* Anzahl markierter Verzeichnisse  */

    /* 1, falls ein Eintrag mit diesem Namen existiert (".." ausgenommen),
     * case-insensitiv. Fuer die Ueberschreiben-Abfrage beim Upload. */
    int         has_entry(const char *name) const;

    /* Titel-/Pfadzeile des Panels (lokal: Verzeichnis, remote: FTP-Pfad). */
    const char *title() const { return header; }

protected:
    /* --- Bildschirmregion --- */
    int top, left, height, width;

    /* --- Inhalt --- */
    PanelEntry entries[PANEL_MAX_ENTRIES];
    int        count;       /* Anzahl gueltiger Eintraege          */
    int        cursor;      /* Index des markierten Eintrags       */
    int        topentry;    /* Index des ersten sichtbaren Eintrags*/
    int        active;      /* 1 = aktives Panel                   */
    char       header[PANEL_HEADER_MAX];  /* Pfad-/Titelzeile      */

    /* --- Hilfsfunktionen --- */
    int  visible_rows() const;   /* Anzahl sichtbarer Eintragszeilen      */
    void clamp_scroll();         /* topentry so anpassen, dass cursor sichtbar */
    void draw_entry_row(int idx);/* nur eine Eintragszeile (idx) neu zeichnen */
    void format_entry(const PanelEntry *e, char *out, int inner) const;
    virtual unsigned char frame_attr() const;  /* Rahmenfarbe (ueberschreibbar) */
};

#endif /* PANEL_H */
