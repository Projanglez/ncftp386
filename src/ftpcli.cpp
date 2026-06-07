/*
   NCFTP386 - ftpcli.cpp
   FTP-Protokoll-Statemachine ueber mTCP (immer PASV-Modus).

   Die Operationen sind blockierend implementiert: Jede Warteschleife
   treibt den mTCP-Stack (PACKET_PROCESS_SINGLE / Arp / Tcp) selbst an und
   bricht nach Timeout ab. Das passt zum synchronen TUI-Ablauf, bei dem der
   Benutzer auf den Abschluss einer Aktion wartet (spaeter mit Fortschritt).

   Referenz: mtcp/APPS/FTP/FTP.CPP
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>

#include "ftpcli.h"
#include "i18n.h"

/* mTCP (Reihenfolge wie im Referenz-Client) */
#include "types.h"
#include "utils.h"
#include "timer.h"
#include "packet.h"
#include "arp.h"
#include "udp.h"
#include "dns.h"
#include "tcp.h"
#include "tcpsockm.h"


/* --- Konstanten ------------------------------------------------------- */
#define CTRL_TIMEOUT_MS     30000ul   /* 30s fuer Kontrollbefehle (CLAUDE.md) */
#define DATA_TIMEOUT_MS     60000ul   /* 60s fuer Datentransfer               */
#define CONNECT_TIMEOUT_MS  15000ul
#define CONTROL_RECV_SIZE    1024
#define DATA_RECV_SIZE       4096
#define DATA_CHUNK           1024


/* --- Datei-lokaler Zustand ------------------------------------------- */
static int g_stackUp = 0;             /* wurde initStack erfolgreich? */
static uint16_t g_nextLocalPort = 4096;

static uint16_t nextLocalPort(void) {
    uint16_t p = g_nextLocalPort++;
    if (g_nextLocalPort >= 32000) g_nextLocalPort = 4096;
    return p;
}

/* Den mTCP-Stack einmal antreiben (in jeder Warteschleife noetig). */
static void driveStack(void) {
    PACKET_PROCESS_SINGLE;
    Arp::driveArp();
    Tcp::drivePackets();
}

/* Verstrichene Millisekunden seit Ticks-Wert. */
static unsigned long elapsedMs(clockTicks_t start) {
    return (unsigned long)Timer_diff(start, TIMER_GET_CURRENT()) * TIMER_TICK_LEN;
}


/* ===================================================================== */
/* Stack-Initialisierung (statisch, einmalig)                            */
/* ===================================================================== */

int FtpClient::init_stack(void) {
    if (g_stackUp) return FTP_OK;
    /* Liest MTCPCFG env-Variable + Konfigurationsdatei. */
    if (Utils::parseEnv() != 0) return FTP_ERR_GENERAL;
    if (Utils::initStack(TCP_MAX_SOCKETS, TCP_MAX_XMIT_BUFS) != 0) return FTP_ERR_GENERAL;
    g_stackUp = 1;
    return FTP_OK;
}

void FtpClient::shutdown_stack(void) {
    if (g_stackUp) {
        Utils::endStack();
        g_stackUp = 0;
    }
}

void FtpClient::stack_poll(unsigned ms) {
    clockTicks_t start = TIMER_GET_CURRENT();
    while (elapsedMs(start) < (unsigned long)ms)
        driveStack();
}


/* ===================================================================== */
/* Konstruktor / Fehlerhilfen                                            */
/* ===================================================================== */

FtpClient::FtpClient() {
    state = FTP_DISCONNECTED;
    ctrl = 0;
    hostname[0] = 0;
    lastCode = 0;
    replyText[0] = 0;
    errmsg[0] = 0;
    pasvAddr[0] = pasvAddr[1] = pasvAddr[2] = pasvAddr[3] = 0;
    pasvPort = 0;
}

void FtpClient::setError(const char *msg) {
    strncpy(errmsg, msg, FTP_ERRMSG_MAX - 1);
    errmsg[FTP_ERRMSG_MAX - 1] = 0;
}

/* errmsg = "<prefix> (<code>) <reply-text>", laengenbegrenzt. */
void FtpClient::setErrorReply(const char *prefix) {
    sprintf(errmsg, "%.40s (%d) %.100s", prefix, lastCode, replyText);
}


/* ===================================================================== */
/* Senden / Empfangen auf der Steuerverbindung                           */
/* ===================================================================== */

int FtpClient::sendRaw(const char *buf, int len) {
    TcpSocket *s = (TcpSocket *)ctrl;
    if (!s) return FTP_ERR_PROTO;

    int off = 0;
    clockTicks_t start = TIMER_GET_CURRENT();
    while (off < len) {
        driveStack();
        int16_t n = s->send((uint8_t *)(buf + off), (uint16_t)(len - off));
        if (n > 0) {
            off += n;
            start = TIMER_GET_CURRENT();
        } else if (n == 0) {
            /* derzeit keine Sendepuffer frei -> weiter antreiben */
            if (elapsedMs(start) > CTRL_TIMEOUT_MS) { setError(L("Sende-Timeout", "Send timeout")); return FTP_ERR_TIMEOUT; }
        } else {
            setError(L("Steuerverbindung verloren", "Control connection lost"));
            return FTP_ERR_PROTO;
        }
    }
    return FTP_OK;
}

int FtpClient::sendCmd(const char *cmd) {
    char buf[FTP_LINE_MAX];
    int n = (int)strlen(cmd);
    if (n > FTP_LINE_MAX - 3) n = FTP_LINE_MAX - 3;
    memcpy(buf, cmd, n);
    buf[n++] = '\r';
    buf[n++] = '\n';
    return sendRaw(buf, n);
}

int FtpClient::sendCmdArg(const char *cmd, const char *arg) {
    char buf[FTP_LINE_MAX];
    int n = sprintf(buf, "%.8s %.230s", cmd, arg ? arg : "");
    if (n < 0) return FTP_ERR_GENERAL;
    if (n > FTP_LINE_MAX - 3) n = FTP_LINE_MAX - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    return sendRaw(buf, n);
}

/* Liest eine vollstaendige (ggf. mehrzeilige) FTP-Antwort.
   Rueckgabe: 3-stelliger Code, oder negativer FTP_ERR_* Wert. */
int FtpClient::readReply(void) {
    TcpSocket *s = (TcpSocket *)ctrl;
    if (!s) return FTP_ERR_PROTO;

    char line[FTP_LINE_MAX];
    int  len = 0;
    int  firstCode = 0;
    int  multiline = 0;
    clockTicks_t start = TIMER_GET_CURRENT();
    uint8_t ch;

    for (;;) {
        driveStack();
        int16_t n = s->recv(&ch, 1);

        if (n > 0) {
            start = TIMER_GET_CURRENT();
            if (ch == '\r') continue;
            if (ch == '\n') {
                line[len] = 0;

                int haveCode = (len >= 3 &&
                                isdigit((unsigned char)line[0]) &&
                                isdigit((unsigned char)line[1]) &&
                                isdigit((unsigned char)line[2]));
                char sep  = (len >= 4) ? line[3] : ' ';
                int  code = 0;
                if (haveCode)
                    code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');

                /* Klartext der Zeile (ohne "NNN ") merken. */
                if (len > 4) {
                    strncpy(replyText, line + 4, FTP_LINE_MAX - 1);
                } else {
                    strncpy(replyText, line, FTP_LINE_MAX - 1);
                }
                replyText[FTP_LINE_MAX - 1] = 0;

                if (!multiline) {
                    if (haveCode && sep == '-') {
                        firstCode = code;
                        multiline = 1;
                    } else if (haveCode) {
                        lastCode = code;
                        return code;
                    }
                    /* Zeile ohne Code vor dem Reply -> ignorieren */
                } else {
                    if (haveCode && sep == ' ' && code == firstCode) {
                        lastCode = code;
                        return code;
                    }
                    /* Fortsetzungszeile -> weiterlesen */
                }
                len = 0;
                continue;
            }
            if (len < FTP_LINE_MAX - 1) line[len++] = (char)ch;
            /* sonst: ueberlange Zeile -> Rest verwerfen */
        } else if (n == 0) {
            if (s->isRemoteClosed() && !s->recvDataWaiting()) {
                setError(L("Server hat die Verbindung geschlossen", "Server closed the connection"));
                return FTP_ERR_PROTO;
            }
            if (elapsedMs(start) > CTRL_TIMEOUT_MS) {
                setError(L("Zeitueberschreitung beim Warten auf Serverantwort", "Timeout waiting for server reply"));
                return FTP_ERR_TIMEOUT;
            }
        } else {
            setError(L("Steuerverbindung verloren", "Control connection lost"));
            return FTP_ERR_PROTO;
        }
    }
}

/* Befehl senden + Antwort lesen; Erfolg bei 2xx. */
int FtpClient::simpleCmd(const char *cmd, const char *arg) {
    int rc = (arg && arg[0]) ? sendCmdArg(cmd, arg) : sendCmd(cmd);
    if (rc != FTP_OK) return rc;
    int code = readReply();
    if (code < 0) return code;
    if (code >= 200 && code < 300) return FTP_OK;
    return FTP_ERR_SERVER;
}


/* ===================================================================== */
/* Datenverbindung (PASV)                                                */
/* ===================================================================== */

int FtpClient::openDataConn(void **dataSockOut) {
    *dataSockOut = 0;

    int rc = sendCmd("PASV");
    if (rc != FTP_OK) return rc;
    int code = readReply();
    if (code < 0) return code;
    if (code != 227) { setErrorReply(L("PASV abgelehnt", "PASV refused")); return FTP_ERR_DATACONN; }

    /* replyText: "...(h1,h2,h3,h4,p1,p2)" - erste Ziffer suchen. */
    const char *p = replyText;
    while (*p && !isdigit((unsigned char)*p)) p++;

    unsigned a0, a1, a2, a3, q1, q2;
    if (sscanf(p, "%u,%u,%u,%u,%u,%u", &a0, &a1, &a2, &a3, &q1, &q2) != 6) {
        setError(L("PASV-Antwort nicht lesbar", "Cannot parse PASV reply"));
        return FTP_ERR_DATACONN;
    }
    pasvAddr[0] = (unsigned char)a0; pasvAddr[1] = (unsigned char)a1;
    pasvAddr[2] = (unsigned char)a2; pasvAddr[3] = (unsigned char)a3;
    pasvPort = ((q1 & 0xFF) << 8) + (q2 & 0xFF);

    TcpSocket *d = TcpSocketMgr::getSocket();
    if (!d) { setError(L("Kein freier Socket fuer Datenverbindung", "No free socket for data connection")); return FTP_ERR_DATACONN; }
    if (d->setRecvBuffer(DATA_RECV_SIZE)) {
        TcpSocketMgr::freeSocket(d);
        setError(L("Zu wenig Speicher fuer Datenpuffer", "Not enough memory for data buffer"));
        return FTP_ERR_DATACONN;
    }

    IpAddr_t addr;
    addr[0] = pasvAddr[0]; addr[1] = pasvAddr[1];
    addr[2] = pasvAddr[2]; addr[3] = pasvAddr[3];

    if (d->connect(nextLocalPort(), addr, (uint16_t)pasvPort, CONNECT_TIMEOUT_MS) != 0) {
        d->close();
        TcpSocketMgr::freeSocket(d);
        setError(L("Datenverbindung fehlgeschlagen", "Data connection failed"));
        return FTP_ERR_DATACONN;
    }

    *dataSockOut = d;
    return FTP_OK;
}

void FtpClient::closeData(void *dataSock) {
    if (!dataSock) return;
    TcpSocket *d = (TcpSocket *)dataSock;
    d->close();                       /* blockierend: leert Queue + FIN */
    TcpSocketMgr::freeSocket(d);
}


/* ===================================================================== */
/* Verbindung aufbauen / trennen                                         */
/* ===================================================================== */

int FtpClient::connect(const char *host, unsigned port,
                       const char *user, const char *pass) {
    if (state != FTP_DISCONNECTED) disconnect();
    setError("");
    strncpy(hostname, host, FTP_HOST_MAX - 1);
    hostname[FTP_HOST_MAX - 1] = 0;

    state = FTP_CONNECTING;

    /* --- Namensaufloesung (akzeptiert auch gepunktete IPs) --- */
    IpAddr_t addr;
    int8_t drc = Dns::resolve(host, addr, 1);
    if (drc < 0) {
        setError(L("Hostname zu lang oder ungueltig", "Host name too long or invalid"));
        state = FTP_DISCONNECTED; return FTP_ERR_DNS;
    }
    if (drc != 0) {
        clockTicks_t start = TIMER_GET_CURRENT();
        while (Dns::isQueryPending()) {
            PACKET_PROCESS_SINGLE;
            Arp::driveArp();
            Dns::drivePendingQuery();
            if (elapsedMs(start) > DNS_TIMEOUT) {
                setError(L("DNS-Zeitueberschreitung", "DNS timeout"));
                state = FTP_DISCONNECTED; return FTP_ERR_DNS;
            }
        }
        drc = Dns::resolve(host, addr, 0);
        if (drc != 0) {
            setError(L("Host nicht gefunden", "Host not found"));
            state = FTP_DISCONNECTED; return FTP_ERR_DNS;
        }
    }

    /* --- Steuersocket aufbauen --- */
    TcpSocket *s = TcpSocketMgr::getSocket();
    if (!s) { setError(L("Kein freier Socket", "No free socket")); state = FTP_DISCONNECTED; return FTP_ERR_CONNECT; }
    if (s->setRecvBuffer(CONTROL_RECV_SIZE)) {
        TcpSocketMgr::freeSocket(s);
        setError(L("Zu wenig Speicher", "Out of memory"));
        state = FTP_DISCONNECTED; return FTP_ERR_CONNECT;
    }
    if (s->connect(nextLocalPort(), addr, (uint16_t)port, CONNECT_TIMEOUT_MS) != 0) {
        s->close();
        TcpSocketMgr::freeSocket(s);
        setError(L("Verbindung zum Server fehlgeschlagen", "Connection to server failed"));
        state = FTP_DISCONNECTED; return FTP_ERR_CONNECT;
    }
    ctrl = s;

    /* --- Banner (220) --- */
    state = FTP_BANNER;
    int code = readReply();
    if (code < 0) { disconnect(); return code; }
    if (code != 220) { setErrorReply(L("Server-Banner unerwartet", "Unexpected server banner")); disconnect(); return FTP_ERR_PROTO; }

    /* --- USER --- */
    state = FTP_AUTH_USER;
    if (sendCmdArg("USER", (user && user[0]) ? user : "anonymous") != FTP_OK) { disconnect(); return FTP_ERR_PROTO; }
    code = readReply();
    if (code < 0) { disconnect(); return code; }
    if (code == 230) { state = FTP_IDLE; return FTP_OK; }     /* ohne Passwort drin */
    if (code != 331) { setErrorReply(L("Benutzer abgelehnt", "User rejected")); disconnect(); return FTP_ERR_AUTH; }

    /* --- PASS --- */
    state = FTP_AUTH_PASS;
    if (sendCmdArg("PASS", pass ? pass : "") != FTP_OK) { disconnect(); return FTP_ERR_PROTO; }
    code = readReply();
    if (code < 0) { disconnect(); return code; }
    if (code == 230 || code == 202) { state = FTP_IDLE; return FTP_OK; }
    if (code == 332) { setError(L("Server verlangt ACCT (nicht unterstuetzt)", "Server requires ACCT (unsupported)")); disconnect(); return FTP_ERR_AUTH; }

    setErrorReply(L("Login fehlgeschlagen", "Login failed"));
    disconnect();
    return FTP_ERR_AUTH;
}

void FtpClient::disconnect(void) {
    if (ctrl) {
        TcpSocket *s = (TcpSocket *)ctrl;
        state = FTP_DISCONNECTING;

        /* QUIT senden (best effort), Antwort kurz abwarten. */
        sendCmd("QUIT");
        clockTicks_t start = TIMER_GET_CURRENT();
        while (elapsedMs(start) < 1000ul) {
            driveStack();
            uint8_t tmp[64];
            int16_t n = s->recv(tmp, sizeof(tmp));
            if (n < 0) break;
            if (s->isRemoteClosed() && !s->recvDataWaiting()) break;
        }

        s->close();
        TcpSocketMgr::freeSocket(s);
        ctrl = 0;
    }
    state = FTP_DISCONNECTED;
}


/* ===================================================================== */
/* Verzeichnisliste                                                      */
/* ===================================================================== */

int FtpClient::list(const char *path, FtpLineCb cb, void *ctx) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }

    void *d = 0;
    state = FTP_LISTING;
    int rc = openDataConn(&d);
    if (rc != FTP_OK) { state = FTP_IDLE; return rc; }

    rc = (path && path[0]) ? sendCmdArg("LIST", path) : sendCmd("LIST");
    if (rc != FTP_OK) { closeData(d); state = FTP_IDLE; return rc; }

    int code = readReply();                 /* 150 / 125 erwartet */
    if (code < 0) { closeData(d); state = FTP_IDLE; return code; }
    if (code != 150 && code != 125) {
        closeData(d); state = FTP_IDLE;
        if (code == 226 || code == 250) return FTP_OK;   /* leeres Listing */
        setErrorReply(L("LIST abgelehnt", "LIST refused"));
        return (code >= 400) ? FTP_ERR_SERVER : FTP_ERR_PROTO;
    }

    /* --- Datenverbindung zeilenweise lesen --- */
    TcpSocket *ds = (TcpSocket *)d;
    char line[FTP_LINE_MAX];
    int  len = 0;
    uint8_t buf[DATA_CHUNK];
    clockTicks_t start = TIMER_GET_CURRENT();

    for (;;) {
        driveStack();
        int16_t n = ds->recv(buf, sizeof(buf));
        if (n > 0) {
            start = TIMER_GET_CURRENT();
            for (int i = 0; i < n; i++) {
                uint8_t c = buf[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    line[len] = 0;
                    if (cb) cb(ctx, line);
                    len = 0;
                } else if (len < FTP_LINE_MAX - 1) {
                    line[len++] = (char)c;
                }
            }
        } else if (n == 0) {
            if (ds->isRemoteClosed() && !ds->recvDataWaiting()) break;
            if (elapsedMs(start) > DATA_TIMEOUT_MS) break;
        } else {
            break;                          /* Socket geschlossen */
        }
    }
    if (len > 0) { line[len] = 0; if (cb) cb(ctx, line); }   /* letzte Zeile ohne LF */

    closeData(d);

    code = readReply();                     /* Abschluss 226 */
    state = FTP_IDLE;
    if (code < 0) return code;
    if (code >= 400) { setErrorReply(L("LIST unvollstaendig", "LIST incomplete")); return FTP_ERR_SERVER; }
    return FTP_OK;
}


/* ===================================================================== */
/* Navigation                                                            */
/* ===================================================================== */

int FtpClient::change_dir(const char *path) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("CWD", path);
    if (rc == FTP_ERR_SERVER) setErrorReply(L("Verzeichniswechsel fehlgeschlagen", "Directory change failed"));
    return rc;
}

int FtpClient::parent_dir(void) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("CDUP", 0);
    if (rc == FTP_ERR_SERVER) setErrorReply(L("CDUP fehlgeschlagen", "CDUP failed"));
    return rc;
}

int FtpClient::get_cwd(char *buf, int buflen) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }
    if (buflen > 0) buf[0] = 0;

    int rc = sendCmd("PWD");
    if (rc != FTP_OK) return rc;
    int code = readReply();
    if (code < 0) return code;
    if (code != 257) { setErrorReply(L("PWD fehlgeschlagen", "PWD failed")); return FTP_ERR_SERVER; }

    /* 257-Format: "/pfad" optionaler Kommentar; "" -> " escaped. */
    const char *q = strchr(replyText, '"');
    if (q) {
        q++;
        int o = 0;
        while (*q && o < buflen - 1) {
            if (*q == '"') {
                if (q[1] == '"') { buf[o++] = '"'; q += 2; continue; }
                break;
            }
            buf[o++] = *q++;
        }
        buf[o] = 0;
    } else {
        strncpy(buf, replyText, buflen - 1);
        buf[buflen - 1] = 0;
    }
    return FTP_OK;
}


/* ===================================================================== */
/* Download (RETR) / Upload (STOR) - immer Binaer (TYPE I)               */
/* ===================================================================== */

int FtpClient::retr(const char *remote, const char *localpath,
                    FtpProgressCb cb, void *ctx) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }

    int rc = simpleCmd("TYPE", "I");
    if (rc != FTP_OK) { if (rc == FTP_ERR_SERVER) setError(L("TYPE I abgelehnt", "TYPE I refused")); return rc; }

    /* SIZE: Dateigroesse abfragen fuer Fortschrittsbalken (RFC 3659).
     * Nicht alle Server unterstuetzen SIZE; Fehler werden ignoriert. */
    unsigned long filesize = 0;
    if (sendCmdArg("SIZE", remote) == FTP_OK) {
        int sc = readReply();
        if (sc == 213)
            filesize = strtoul(replyText, 0, 10);
    }

    void *d = 0;
    state = FTP_TRANSFERRING;
    rc = openDataConn(&d);
    if (rc != FTP_OK) { state = FTP_IDLE; return rc; }

    FILE *f = fopen(localpath, "wb");
    if (!f) { closeData(d); state = FTP_IDLE; setError(L("Lokale Datei nicht anlegbar", "Cannot create local file")); return FTP_ERR_LOCALIO; }

    rc = sendCmdArg("RETR", remote);
    if (rc != FTP_OK) { fclose(f); remove(localpath); closeData(d); state = FTP_IDLE; return rc; }

    int code = readReply();                 /* 150 / 125 */
    if (code < 0) { fclose(f); remove(localpath); closeData(d); state = FTP_IDLE; return code; }
    if (code != 150 && code != 125) {
        fclose(f); remove(localpath); closeData(d); state = FTP_IDLE;
        setErrorReply(L("RETR abgelehnt", "RETR refused"));
        return (code >= 400) ? FTP_ERR_SERVER : FTP_ERR_PROTO;
    }

    TcpSocket *ds = (TcpSocket *)d;
    unsigned long total = 0;
    uint8_t buf[DATA_CHUNK];
    int ioerr = 0;
    clockTicks_t start = TIMER_GET_CURRENT();

    for (;;) {
        driveStack();
        int16_t n = ds->recv(buf, sizeof(buf));
        if (n > 0) {
            start = TIMER_GET_CURRENT();
            if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ioerr = 1; break; }
            total += (unsigned long)n;
            if (cb) cb(ctx, total, filesize);
        } else if (n == 0) {
            if (ds->isRemoteClosed() && !ds->recvDataWaiting()) break;
            if (elapsedMs(start) > DATA_TIMEOUT_MS) { ioerr = 2; break; }
        } else {
            break;
        }
    }

    fclose(f);
    closeData(d);

    if (ioerr == 1) { remove(localpath); setError(L("Schreibfehler (Platte voll?)", "Write error (disk full?)")); state = FTP_IDLE; return FTP_ERR_LOCALIO; }
    if (ioerr == 2) { setError(L("Zeitueberschreitung beim Download", "Download timeout")); state = FTP_IDLE; return FTP_ERR_TIMEOUT; }

    code = readReply();                     /* 226 */
    state = FTP_IDLE;
    if (code < 0) return code;
    if (code >= 400) { setErrorReply(L("Download unvollstaendig", "Download incomplete")); return FTP_ERR_SERVER; }
    return FTP_OK;
}

int FtpClient::stor(const char *localpath, const char *remote,
                    FtpProgressCb cb, void *ctx) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }

    FILE *f = fopen(localpath, "rb");
    if (!f) { setError(L("Lokale Datei nicht lesbar", "Cannot read local file")); return FTP_ERR_LOCALIO; }

    unsigned long fsize = 0;                /* fuer Fortschritt */
    if (fseek(f, 0L, SEEK_END) == 0) {
        long t = ftell(f);
        if (t > 0) fsize = (unsigned long)t;
        fseek(f, 0L, SEEK_SET);
    }

    int rc = simpleCmd("TYPE", "I");
    if (rc != FTP_OK) { fclose(f); if (rc == FTP_ERR_SERVER) setError(L("TYPE I abgelehnt", "TYPE I refused")); return rc; }

    void *d = 0;
    state = FTP_TRANSFERRING;
    rc = openDataConn(&d);
    if (rc != FTP_OK) { fclose(f); state = FTP_IDLE; return rc; }

    rc = sendCmdArg("STOR", remote);
    if (rc != FTP_OK) { fclose(f); closeData(d); state = FTP_IDLE; return rc; }

    int code = readReply();                 /* 150 / 125 */
    if (code < 0) { fclose(f); closeData(d); state = FTP_IDLE; return code; }
    if (code != 150 && code != 125) {
        fclose(f); closeData(d); state = FTP_IDLE;
        setErrorReply(L("STOR abgelehnt", "STOR refused"));
        return (code >= 400) ? FTP_ERR_SERVER : FTP_ERR_PROTO;
    }

    TcpSocket *ds = (TcpSocket *)d;
    unsigned long sent = 0;
    uint8_t buf[DATA_CHUNK];
    int ioerr = 0;                          /* 1=lokal lesen, 2=timeout, 3=conn */

    for (;;) {
        size_t r = fread(buf, 1, sizeof(buf), f);
        if (r == 0) break;                  /* EOF (ggf. Lesefehler -> ferror) */

        uint16_t off = 0;
        clockTicks_t start = TIMER_GET_CURRENT();
        while (off < r) {
            driveStack();
            int16_t ns = ds->send(buf + off, (uint16_t)(r - off));
            if (ns > 0) { off += (uint16_t)ns; start = TIMER_GET_CURRENT(); }
            else if (ns == 0) {
                if (elapsedMs(start) > DATA_TIMEOUT_MS) { ioerr = 2; break; }
            } else { ioerr = 3; break; }
        }
        if (ioerr) break;

        sent += (unsigned long)r;
        if (cb) cb(ctx, sent, fsize);
    }
    if (!ioerr && ferror(f)) ioerr = 1;

    fclose(f);
    closeData(d);                           /* blockierend: alle Daten + FIN raus */

    if (ioerr == 1) { setError(L("Lokaler Lesefehler", "Local read error")); state = FTP_IDLE; return FTP_ERR_LOCALIO; }
    if (ioerr == 2) { setError(L("Zeitueberschreitung beim Upload", "Upload timeout")); state = FTP_IDLE; return FTP_ERR_TIMEOUT; }
    if (ioerr == 3) { setError(L("Datenverbindung verloren", "Data connection lost")); state = FTP_IDLE; return FTP_ERR_DATACONN; }

    code = readReply();                     /* 226 */
    state = FTP_IDLE;
    if (code < 0) return code;
    if (code >= 400) { setErrorReply(L("Upload unvollstaendig", "Upload incomplete")); return FTP_ERR_SERVER; }
    return FTP_OK;
}


/* ===================================================================== */
/* Server-Dateioperationen                                               */
/* ===================================================================== */

int FtpClient::remote_file_exists(const char *path) {
    if (!is_connected()) return 0;
    /* TYPE I, damit SIZE auch bei Servern funktioniert, die es im ASCII-Modus
     * ablehnen. Fehler hier ignorieren. */
    simpleCmd("TYPE", "I");
    if (sendCmdArg("SIZE", path) != FTP_OK) return 0;
    int code = readReply();
    return (code == 213) ? 1 : 0;
}

int FtpClient::make_dir(const char *path) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("MKD", path);        /* 257 */
    if (rc == FTP_ERR_SERVER) setErrorReply(L("Verzeichnis anlegen fehlgeschlagen", "Make directory failed"));
    return rc;
}

int FtpClient::remove_dir(const char *path) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("RMD", path);        /* 250 */
    if (rc == FTP_ERR_SERVER) setErrorReply(L("Verzeichnis loeschen fehlgeschlagen", "Remove directory failed"));
    return rc;
}

int FtpClient::remove_file(const char *path) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("DELE", path);       /* 250 */
    if (rc == FTP_ERR_SERVER) setErrorReply(L("Loeschen fehlgeschlagen", "Delete failed"));
    return rc;
}

int FtpClient::rename(const char *from, const char *to) {
    if (!is_connected()) { setError(L("Nicht verbunden", "Not connected")); return FTP_ERR_GENERAL; }

    int rc = sendCmdArg("RNFR", from);
    if (rc != FTP_OK) return rc;
    int code = readReply();
    if (code < 0) return code;
    if (code != 350) { setErrorReply(L("RNFR fehlgeschlagen", "RNFR failed")); return FTP_ERR_SERVER; }

    rc = sendCmdArg("RNTO", to);
    if (rc != FTP_OK) return rc;
    code = readReply();
    if (code < 0) return code;
    if (code >= 200 && code < 300) return FTP_OK;

    setErrorReply(L("Umbenennen fehlgeschlagen", "Rename failed"));
    return FTP_ERR_SERVER;
}
