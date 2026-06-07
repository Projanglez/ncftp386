/*
   NCFTP386 - ftpcli.h
   FTP-Protokoll-Statemachine ueber mTCP (PASV-Modus).

   Bewusst OHNE mTCP-Header: die TcpSocket-Zeiger sind als `void*`
   gekapselt. Dadurch koennen ncftp.cpp / rpanel.cpp diese Klasse nutzen,
   ohne CFG_H und die mTCP-Includes zu benoetigen. Nur ftpcli.cpp zieht
   die mTCP-Header herein.
*/

#ifndef FTPCLI_H
#define FTPCLI_H


/* --- Rueckgabe-Codes -------------------------------------------------- */
#define FTP_OK             0
#define FTP_ERR_GENERAL   -1
#define FTP_ERR_DNS       -2   /* Hostname nicht aufloesbar              */
#define FTP_ERR_CONNECT   -3   /* TCP-Connect fehlgeschlagen             */
#define FTP_ERR_TIMEOUT   -4   /* Zeitueberschreitung                    */
#define FTP_ERR_AUTH      -5   /* Login abgelehnt                        */
#define FTP_ERR_PROTO     -6   /* Protokollfehler / Verbindung verloren  */
#define FTP_ERR_DATACONN  -7   /* Datenverbindung fehlgeschlagen         */
#define FTP_ERR_LOCALIO   -8   /* Lokaler Datei-Fehler                   */
#define FTP_ERR_SERVER    -9   /* Server antwortete mit 4xx/5xx          */
#define FTP_ERR_ABORT    -10   /* Benutzer hat den Vorgang abgebrochen   */


/* --- Zustaende (vgl. CLAUDE.md) --------------------------------------- */
enum FtpState {
    FTP_DISCONNECTED = 0,
    FTP_CONNECTING,
    FTP_BANNER,
    FTP_AUTH_USER,
    FTP_AUTH_PASS,
    FTP_IDLE,
    FTP_LISTING,
    FTP_TRANSFERRING,
    FTP_ERROR,
    FTP_DISCONNECTING
};


/* Callback: einmal pro Roh-Textzeile einer LIST-Ausgabe (ohne CRLF). */
typedef void (*FtpLineCb)(void *ctx, const char *line);

/* Callback: Fortschritt waehrend Transfers. total==0 => unbekannt. */
typedef void (*FtpProgressCb)(void *ctx, unsigned long sofar, unsigned long total);


#define FTP_HOST_MAX    64
#define FTP_LINE_MAX   256
#define FTP_ERRMSG_MAX 160


class FtpClient {
public:
    FtpClient();

    /* Einmalige mTCP-Stack-Initialisierung (parseEnv + initStack).
       MUSS vor dem ersten connect() aufgerufen werden, genau einmal.
       Gibt FTP_OK oder einen Fehlercode zurueck. */
    static int  init_stack(void);
    static void shutdown_stack(void);

    /* Den mTCP-Stack 'ms' Millisekunden lang treiben, ohne etwas zu senden.
     * Zum "Warmlaufen" direkt nach init_stack (Treiber/Link setteln lassen),
     * bevor die allererste Verbindung aufgebaut wird. */
    static void stack_poll(unsigned ms);

    /* Steuerverbindung aufbauen + einloggen (USER/PASS). */
    int  connect(const char *host, unsigned port,
                 const char *user, const char *pass);
    void disconnect(void);

    int      is_connected(void) const {
        return state >= FTP_IDLE && state < FTP_DISCONNECTING;
    }
    FtpState get_state(void) const { return state; }

    /* Verzeichnisliste abrufen. Jede Roh-Textzeile -> cb(ctx, line). */
    int  list(const char *path, FtpLineCb cb, void *ctx);

    /* Navigation auf dem Server. */
    int  change_dir(const char *path);          /* CWD          */
    int  parent_dir(void);                       /* CDUP         */
    int  get_cwd(char *buf, int buflen);         /* PWD -> Pfad  */

    /* Transfers (immer Binaer/TYPE I). */
    int  retr(const char *remote, const char *localpath,
              FtpProgressCb cb, void *ctx);
    int  stor(const char *localpath, const char *remote,
              FtpProgressCb cb, void *ctx);

    /* 1, falls auf dem Server eine Datei mit diesem Pfad existiert (per SIZE).
     * Fuer die Ueberschreiben-Abfrage beim rekursiven Upload. */
    int  remote_file_exists(const char *path);

    /* Dateioperationen auf dem Server. */
    int  make_dir(const char *path);             /* MKD          */
    int  remove_dir(const char *path);           /* RMD          */
    int  remove_file(const char *path);          /* DELE         */
    int  rename(const char *from, const char *to); /* RNFR+RNTO  */

    const char *host_name(void) const  { return hostname; }
    const char *last_error(void) const { return errmsg; }
    int         last_code(void) const  { return lastCode; }

private:
    FtpState state;
    void    *ctrl;                  /* TcpSocket* (opak)              */
    char     hostname[FTP_HOST_MAX];
    int      lastCode;              /* letzter 3-stelliger Reply-Code */
    char     replyText[FTP_LINE_MAX]; /* Text der letzten Reply-Zeile */
    char     errmsg[FTP_ERRMSG_MAX];

    unsigned char pasvAddr[4];      /* aus 227-Antwort geparst        */
    unsigned      pasvPort;

    /* --- interne Helfer (in ftpcli.cpp) --- */
    int  sendCmd(const char *cmd);                  /* haengt CRLF an   */
    int  sendCmdArg(const char *cmd, const char *arg);
    int  sendRaw(const char *buf, int len);         /* alles senden     */
    int  readReply(void);                           /* Code oder <0     */
    int  openDataConn(void **dataSockOut);          /* PASV + connect   */
    void closeData(void *dataSock);
    int  simpleCmd(const char *cmd, const char *arg); /* senden + Reply */
    void setError(const char *msg);
    void setErrorReply(const char *prefix);
};


#endif
