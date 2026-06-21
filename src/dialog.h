/* =============================================================================
 * dialog.h - Modal dialogs for NCFTP386
 * -----------------------------------------------------------------------------
 * Every dialog draws itself centered over the current screen, runs its own
 * keyboard loop, and restores the screen when it closes - the caller does
 * NOT need to redraw.
 *
 * Multi-line messages: separate lines with '\n' (max. DLG_MAX_LINES).
 * ===========================================================================*/
#ifndef DIALOG_H
#define DIALOG_H

#define DLG_MAX_LINES 8     /* max. text lines in message/confirm dialogs */

/* Notice/error box with one [ OK ] button. is_error!=0 -> red styling.
 * Closes on Enter, Esc, or Space. */
void dlg_message(const char *title, const char *msg, int is_error);
void dlg_error(const char *title, const char *msg);   /* = dlg_message(...,1)  */

/* Yes/No prompt. Returns: 1 = Yes, 0 = No.
 * Y/N as direct keys, Left/Right/Tab switches focus, Enter selects, Esc = No.
 * Default focus is on "Yes". */
int dlg_confirm(const char *title, const char *msg);

/* Single-line input field (basis for the connect dialog).
 * buf is shown pre-filled and edited; maxlen = max. characters (buf must be
 * maxlen+1 in size). is_password!=0 shows '*'. Returns: 1 = OK, 0 = Cancel. */
int dlg_input(const char *title, const char *prompt,
              char *buf, int maxlen, int is_password);

/* Non-modal progress dialog for data transfers (F5).
 * Flow: begin() opens the box (saves the screen), update() is called from
 * the FtpProgressCb during the blocking transfer, end() closes the box and
 * restores the screen.
 *   total >  0 : progress bar with percentage
 *   total == 0 : size unknown -> just the bytes transferred
 * update() only redraws on a visible change, for efficiency. */
void dlg_progress_begin(const char *title, const char *fromname);
void dlg_progress_update(unsigned long sofar, unsigned long total);
void dlg_progress_end(void);

/* Change the displayed file name while a progress dialog is running (for
 * batch/recursive copy operations). Resets the bar to 0%. No effect if no
 * progress dialog is open. */
void dlg_progress_setfile(const char *name);

/* Vertical selection menu (modal). Shows 'count' entries, 'initial' is
 * highlighted at first. Arrows/Home/End move, Enter selects, Esc cancels;
 * a letter key jumps straight to the first entry starting with that
 * character and selects it. Returns: selected index 0..count-1, or -1. */
int dlg_menu(const char *title, const char *const *items, int count, int initial);

/* Choice dialog: multi-line message 'msg' on top, below it a vertical list
 * of 'count' options. Arrows move, Enter selects, Esc cancels.
 * Returns: selected option index 0..count-1, or -1 on Esc. */
int dlg_choice(const char *title, const char *msg,
               const char *const *items, int count);

/* FTP connect form in a single dialog:
 * Host/Port/User/Pass fields + two "save" checkboxes.
 * Tab/Down/Up navigate between fields; Space/Enter toggles checkboxes.
 * *save_conn and *save_pass are used in/out (pre-filled from NCFTP.SAV).
 * Returns: 1 = Connect, 0 = Cancel. On 0, *save_conn/*save_pass are left
 * unchanged. */
int dlg_connect(const char *title,
                char *host, int host_max,
                char *port, int port_max,
                char *user, int user_max,
                char *pass, int pass_max,
                int *save_conn,
                int *save_pass);

/* Splash screen at program start: centered box with version info.
 * Disappears after ~2 seconds or on a key press. */
void dlg_splash(const char *version);

#endif /* DIALOG_H */
