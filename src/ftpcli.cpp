/*
   NCFTP386 - ftpcli.cpp
   FTP protocol state machine over mTCP (always PASV mode).

   The operations are implemented as blocking calls: every wait loop drives
   the mTCP stack itself (PACKET_PROCESS_SINGLE / Arp / Tcp) and bails out
   after a timeout. This matches the synchronous TUI flow, where the user
   waits for an action to complete (progress reporting comes later).

   Reference: mtcp/APPS/FTP/FTP.CPP
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>

#include "ftpcli.h"
#include "i18n.h"

/* mTCP (same order as in the reference client) */
#include "types.h"
#include "utils.h"
#include "timer.h"
#include "packet.h"
#include "arp.h"
#include "udp.h"
#include "dns.h"
#include "tcp.h"
#include "tcpsockm.h"

/* After all system/mTCP headers: short umlaut macros for German text. */
#include "umlaut.h"


/* --- Constants ----------------------------------------------------------- */
#define CTRL_TIMEOUT_MS     30000ul   /* 30s for control commands (CLAUDE.md) */
#define DATA_TIMEOUT_MS     60000ul   /* 60s for data transfers               */
/* 5s: a lost first SYN (cold start) fails fast instead of burning 15s before
 * the app-level retry (perform_connect) sends a fresh SYN. mTCP's own SYN
 * retry only kicks in after ~10s - too late for that purpose. */
#define CONNECT_TIMEOUT_MS  5000ul
#define CONTROL_RECV_SIZE    1024
#define DATA_RECV_SIZE      16384   /* mTCP maximum; large receive window      */
/* Read/write block size for transfers. On download, the TCP receive buffer
 * is drained COMPLETELY on every loop pass (see retr()), so a medium-sized
 * block is enough - it only needs to be >= MSS so we drain faster than the
 * server fills it. */
#define DATA_CHUNK           4096


/* --- File-local state ----------------------------------------------------- */
static int g_stackUp = 0;             /* did initStack succeed?       */
static uint16_t g_nextLocalPort = 4096;

static uint16_t nextLocalPort(void) {
    uint16_t p = g_nextLocalPort++;
    if (g_nextLocalPort >= 32000) g_nextLocalPort = 4096;
    return p;
}

/* Drive the mTCP stack once (needed in every wait loop). */
static void driveStack(void) {
    PACKET_PROCESS_SINGLE;
    Arp::driveArp();
    Tcp::drivePackets();
}

/* Milliseconds elapsed since the given tick value. */
static unsigned long elapsedMs(clockTicks_t start) {
    return (unsigned long)Timer_diff(start, TIMER_GET_CURRENT()) * TIMER_TICK_LEN;
}

/* Drain the data connection 'ds' into 'f' as far as possible (RETR download).
 * This keeps the receive window open; otherwise it drops below one MSS or to
 * zero, the server stalls (Silly Window Syndrome) and sends nothing further
 * until its own persist probe fires (server-dependent, often several
 * seconds). mTCP itself re-announces the window immediately once we recv()
 * out of the zero state (TcpSocket::recv -> sendPureAck, mtcp/TCPLIB/TCP.CPP);
 * mTCP's own zero-window probe interval is 1s (TCP_PROBE_INTERVAL,
 * TCPINC/TCP.H) - there is no 5s timer in mTCP. Behavior verified against
 * mTCP 2025-01-10. Returns: >0 bytes written (added to *total), 0 nothing
 * available, -1 write error, -2 connection closed. */
static int drainToFile(TcpSocket *ds, FILE *f, uint8_t *buf, int bufsz,
                       unsigned long *total) {
    int16_t n;
    int wrote = 0;
    while ((n = ds->recv(buf, (uint16_t)bufsz)) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) return -1;
        *total += (unsigned long)n;
        wrote = 1;
    }
    if (n < 0) return -2;
    return wrote;
}

/* Context for draining the data connection WHILE readReply() is waiting for
 * the RETR "150" reply - so the receive buffer doesn't fill up right at the
 * start of the transfer (otherwise a one-time stall, see drainToFile). */
struct DrainCtx {
    TcpSocket     *ds;
    FILE          *f;
    uint8_t       *buf;
    int            sz;
    unsigned long *total;
    int            err;     /* set to 1 on a write error */
};


/* ===================================================================== */
/* Stack initialization (static, one-time)                               */
/* ===================================================================== */

/* mTCP 2025-01-10: Utils::initStack installs its own Ctrl-Break (INT 1Bh)
 * and Ctrl-C (INT 23h) handlers and requires real function pointers for
 * that (no NULL, or it crashes). In the TUI, Ctrl-Break/Ctrl-C should NOT
 * abort the program; the handler just sets a flag that we deliberately
 * ignore. Utils::endStack restores the original handlers. */
static volatile uint8_t g_ctrlBreakDetected = 0;

static void __interrupt __far ctrlBreakHandler(void) {
    g_ctrlBreakDetected = 1;
}

int FtpClient::init_stack(void) {
    if (g_stackUp) return FTP_OK;
    /* Reads the MTCPCFG env variable + the configuration file. */
    if (Utils::parseEnv() != 0) return FTP_ERR_GENERAL;
    if (Utils::initStack(TCP_MAX_SOCKETS, TCP_MAX_XMIT_BUFS,
                         ctrlBreakHandler, ctrlBreakHandler) != 0) return FTP_ERR_GENERAL;
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
/* Constructor / error helpers                                           */
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

/* errmsg = "<prefix> (<code>) <reply text>", length-limited. */
void FtpClient::setErrorReply(const char *prefix) {
    sprintf(errmsg, "%.40s (%d) %.100s", prefix, lastCode, replyText);
}


/* ===================================================================== */
/* Send / receive on the control connection                              */
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
            /* no send buffers free right now -> keep driving */
            if (elapsedMs(start) > CTRL_TIMEOUT_MS) { setError(L("Send timeout", "Sende-Timeout")); return FTP_ERR_TIMEOUT; }
        } else {
            setError(L("Control connection lost", "Steuerverbindung verloren"));
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

/* Reads one complete (possibly multi-line) FTP reply.
   Returns: 3-digit code, or a negative FTP_ERR_* value.
   drainCtxv != 0 (RETR "150" only): drain the data connection while
   waiting, so its receive buffer doesn't fill up. */
int FtpClient::readReply(void *drainCtxv) {
    TcpSocket *s = (TcpSocket *)ctrl;
    if (!s) return FTP_ERR_PROTO;
    DrainCtx *dc = (DrainCtx *)drainCtxv;

    char line[FTP_LINE_MAX];
    int  len = 0;
    int  firstCode = 0;
    int  multiline = 0;
    clockTicks_t start = TIMER_GET_CURRENT();
    uint8_t ch;

    for (;;) {
        driveStack();
        if (dc) {
            int dr = drainToFile(dc->ds, dc->f, dc->buf, dc->sz, dc->total);
            if (dr == -1) dc->err = 1;
        }
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

                /* Remember the plain text of the line (without "NNN "). */
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
                    /* Line without a code before the reply -> ignore */
                } else {
                    if (haveCode && sep == ' ' && code == firstCode) {
                        lastCode = code;
                        return code;
                    }
                    /* Continuation line -> keep reading */
                }
                len = 0;
                continue;
            }
            if (len < FTP_LINE_MAX - 1) line[len++] = (char)ch;
            /* otherwise: line too long -> discard the rest */
        } else if (n == 0) {
            if (s->isRemoteClosed() && !s->recvDataWaiting()) {
                setError(L("Server closed the connection", "Server hat die Verbindung geschlossen"));
                return FTP_ERR_PROTO;
            }
            if (elapsedMs(start) > CTRL_TIMEOUT_MS) {
                setError(L("Timeout waiting for server reply", "Zeit" ue "berschreitung beim Warten auf Serverantwort"));
                return FTP_ERR_TIMEOUT;
            }
        } else {
            setError(L("Control connection lost", "Steuerverbindung verloren"));
            return FTP_ERR_PROTO;
        }
    }
}

/* Send a command + read the reply; success on 2xx. */
int FtpClient::simpleCmd(const char *cmd, const char *arg) {
    int rc = (arg && arg[0]) ? sendCmdArg(cmd, arg) : sendCmd(cmd);
    if (rc != FTP_OK) return rc;
    int code = readReply();
    if (code < 0) return code;
    if (code >= 200 && code < 300) return FTP_OK;
    return FTP_ERR_SERVER;
}


/* ===================================================================== */
/* Data connection (PASV)                                                */
/* ===================================================================== */

int FtpClient::openDataConn(void **dataSockOut) {
    *dataSockOut = 0;

    int rc = sendCmd("PASV");
    if (rc != FTP_OK) return rc;
    int code = readReply();
    if (code < 0) return code;
    if (code != 227) { setErrorReply(L("PASV refused", "PASV abgelehnt")); return FTP_ERR_DATACONN; }

    /* replyText: "...(h1,h2,h3,h4,p1,p2)" - look for the first digit. */
    const char *p = replyText;
    while (*p && !isdigit((unsigned char)*p)) p++;

    unsigned a0, a1, a2, a3, q1, q2;
    if (sscanf(p, "%u,%u,%u,%u,%u,%u", &a0, &a1, &a2, &a3, &q1, &q2) != 6) {
        setError(L("Cannot parse PASV reply", "PASV-Antwort nicht lesbar"));
        return FTP_ERR_DATACONN;
    }
    pasvAddr[0] = (unsigned char)a0; pasvAddr[1] = (unsigned char)a1;
    pasvAddr[2] = (unsigned char)a2; pasvAddr[3] = (unsigned char)a3;
    pasvPort = ((q1 & 0xFF) << 8) + (q2 & 0xFF);

    TcpSocket *d = TcpSocketMgr::getSocket();
    if (!d) { setError(L("No free socket for data connection", "Kein freier Socket f" ue "r Datenverbindung")); return FTP_ERR_DATACONN; }
    if (d->setRecvBuffer(DATA_RECV_SIZE)) {
        TcpSocketMgr::freeSocket(d);
        setError(L("Not enough memory for data buffer", "Zu wenig Speicher f" ue "r Datenpuffer"));
        return FTP_ERR_DATACONN;
    }

    IpAddr_t addr;
    addr[0] = pasvAddr[0]; addr[1] = pasvAddr[1];
    addr[2] = pasvAddr[2]; addr[3] = pasvAddr[3];

    if (d->connect(nextLocalPort(), addr, (uint16_t)pasvPort, CONNECT_TIMEOUT_MS) != 0) {
        d->close();
        TcpSocketMgr::freeSocket(d);
        setError(L("Data connection failed", "Datenverbindung fehlgeschlagen"));
        return FTP_ERR_DATACONN;
    }

    *dataSockOut = d;
    return FTP_OK;
}

void FtpClient::closeData(void *dataSock) {
    if (!dataSock) return;
    TcpSocket *d = (TcpSocket *)dataSock;
    d->close();                       /* blocking: drains the queue + FIN */
    TcpSocketMgr::freeSocket(d);
}


/* ===================================================================== */
/* Connect / disconnect                                                  */
/* ===================================================================== */

int FtpClient::connect(const char *host, unsigned port,
                       const char *user, const char *pass) {
    if (state != FTP_DISCONNECTED) disconnect();
    setError("");
    strncpy(hostname, host, FTP_HOST_MAX - 1);
    hostname[FTP_HOST_MAX - 1] = 0;

    state = FTP_CONNECTING;

    /* --- Name resolution (also accepts dotted IPs) --- */
    IpAddr_t addr;
    int8_t drc = Dns::resolve(host, addr, 1);
    if (drc < 0) {
        setError(L("Host name too long or invalid", "Hostname zu lang oder ung" ue "ltig"));
        state = FTP_DISCONNECTED; return FTP_ERR_DNS;
    }
    if (drc != 0) {
        clockTicks_t start = TIMER_GET_CURRENT();
        while (Dns::isQueryPending()) {
            PACKET_PROCESS_SINGLE;
            Arp::driveArp();
            Dns::drivePendingQuery();
            if (elapsedMs(start) > DNS_TIMEOUT) {
                setError(L("DNS timeout", "DNS-Zeit" ue "berschreitung"));
                state = FTP_DISCONNECTED; return FTP_ERR_DNS;
            }
        }
        drc = Dns::resolve(host, addr, 0);
        if (drc != 0) {
            setError(L("Host not found", "Host nicht gefunden"));
            state = FTP_DISCONNECTED; return FTP_ERR_DNS;
        }
    }

    /* --- Establish the control socket --- */
    TcpSocket *s = TcpSocketMgr::getSocket();
    if (!s) { setError(L("No free socket", "Kein freier Socket")); state = FTP_DISCONNECTED; return FTP_ERR_CONNECT; }
    if (s->setRecvBuffer(CONTROL_RECV_SIZE)) {
        TcpSocketMgr::freeSocket(s);
        setError(L("Out of memory", "Zu wenig Speicher"));
        state = FTP_DISCONNECTED; return FTP_ERR_CONNECT;
    }
    if (s->connect(nextLocalPort(), addr, (uint16_t)port, CONNECT_TIMEOUT_MS) != 0) {
        s->close();
        TcpSocketMgr::freeSocket(s);
        setError(L("Connection to server failed", "Verbindung zum Server fehlgeschlagen"));
        state = FTP_DISCONNECTED; return FTP_ERR_CONNECT;
    }
    ctrl = s;

    /* --- Banner (220) --- */
    state = FTP_BANNER;
    int code = readReply();
    if (code < 0) { disconnect(); return code; }
    if (code != 220) { setErrorReply(L("Unexpected server banner", "Server-Banner unerwartet")); disconnect(); return FTP_ERR_PROTO; }

    /* --- USER --- */
    state = FTP_AUTH_USER;
    if (sendCmdArg("USER", (user && user[0]) ? user : "anonymous") != FTP_OK) { disconnect(); return FTP_ERR_PROTO; }
    code = readReply();
    if (code < 0) { disconnect(); return code; }
    if (code == 230) { state = FTP_IDLE; return FTP_OK; }     /* logged in without a password */
    if (code != 331) { setErrorReply(L("User rejected", "Benutzer abgelehnt")); disconnect(); return FTP_ERR_AUTH; }

    /* --- PASS --- */
    state = FTP_AUTH_PASS;
    if (sendCmdArg("PASS", pass ? pass : "") != FTP_OK) { disconnect(); return FTP_ERR_PROTO; }
    code = readReply();
    if (code < 0) { disconnect(); return code; }
    if (code == 230 || code == 202) { state = FTP_IDLE; return FTP_OK; }
    if (code == 332) { setError(L("Server requires ACCT (unsupported)", "Server verlangt ACCT (nicht unterst" ue "tzt)")); disconnect(); return FTP_ERR_AUTH; }

    setErrorReply(L("Login failed", "Login fehlgeschlagen"));
    disconnect();
    return FTP_ERR_AUTH;
}

void FtpClient::disconnect(void) {
    if (ctrl) {
        TcpSocket *s = (TcpSocket *)ctrl;
        state = FTP_DISCONNECTING;

        /* Send QUIT (best effort), briefly wait for a reply. */
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
/* Keepalive / idle                                                      */
/* ===================================================================== */

int FtpClient::noop(void) {
    if (!is_connected()) return FTP_ERR_GENERAL;
    if (sendCmd("NOOP") != FTP_OK) { disconnect(); return FTP_ERR_PROTO; }
    int code = readReply();
    if (code < 0) { disconnect(); return code; }   /* connection lost */
    return FTP_OK;                                  /* 200 expected, otherwise doesn't matter */
}

int FtpClient::idle_drive(void) {
    if (!is_connected()) return 0;
    driveStack();
    TcpSocket *s = (TcpSocket *)ctrl;
    if (s && s->isRemoteClosed() && !s->recvDataWaiting()) {
        disconnect();
        return 0;
    }
    return 1;
}

/* ===================================================================== */
/* Directory listing                                                     */
/* ===================================================================== */

int FtpClient::list(const char *path, FtpLineCb cb, void *ctx) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }

    void *d = 0;
    state = FTP_LISTING;
    int rc = openDataConn(&d);
    if (rc != FTP_OK) { state = FTP_IDLE; return rc; }

    rc = (path && path[0]) ? sendCmdArg("LIST", path) : sendCmd("LIST");
    if (rc != FTP_OK) { closeData(d); state = FTP_IDLE; return rc; }

    int code = readReply();                 /* 150 / 125 expected */
    if (code < 0) { closeData(d); state = FTP_IDLE; return code; }
    if (code != 150 && code != 125) {
        closeData(d); state = FTP_IDLE;
        if (code == 226 || code == 250) return FTP_OK;   /* empty listing */
        setErrorReply(L("LIST refused", "LIST abgelehnt"));
        return (code >= 400) ? FTP_ERR_SERVER : FTP_ERR_PROTO;
    }

    /* --- Read the data connection line by line --- */
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
            break;                          /* socket closed */
        }
    }
    if (len > 0) { line[len] = 0; if (cb) cb(ctx, line); }   /* last line without LF */

    closeData(d);

    code = readReply();                     /* completion 226 */
    state = FTP_IDLE;
    if (code < 0) return code;
    if (code >= 400) { setErrorReply(L("LIST incomplete", "LIST unvollst" ae "ndig")); return FTP_ERR_SERVER; }
    return FTP_OK;
}


/* ===================================================================== */
/* Navigation                                                            */
/* ===================================================================== */

int FtpClient::change_dir(const char *path) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("CWD", path);
    if (rc == FTP_ERR_SERVER) setErrorReply(L("Directory change failed", "Verzeichniswechsel fehlgeschlagen"));
    return rc;
}

int FtpClient::parent_dir(void) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("CDUP", 0);
    if (rc == FTP_ERR_SERVER) setErrorReply(L("CDUP failed", "CDUP fehlgeschlagen"));
    return rc;
}

int FtpClient::get_cwd(char *buf, int buflen) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }
    if (buflen > 0) buf[0] = 0;

    int rc = sendCmd("PWD");
    if (rc != FTP_OK) return rc;
    int code = readReply();
    if (code < 0) return code;
    if (code != 257) { setErrorReply(L("PWD failed", "PWD fehlgeschlagen")); return FTP_ERR_SERVER; }

    /* 257 format: "/path" optional comment; "" -> " escaped. */
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
/* Download (RETR) / Upload (STOR) - always binary (TYPE I)              */
/* ===================================================================== */

int FtpClient::retr(const char *remote, const char *localpath,
                    FtpProgressCb cb, void *ctx) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }

    int rc = simpleCmd("TYPE", "I");
    if (rc != FTP_OK) { if (rc == FTP_ERR_SERVER) setError(L("TYPE I refused", "TYPE I abgelehnt")); return rc; }

    /* SIZE: query the file size for the progress bar (RFC 3659).
     * Not all servers support SIZE; errors are ignored. */
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
    if (!f) { closeData(d); state = FTP_IDLE; setError(L("Cannot create local file", "Lokale Datei nicht anlegbar")); return FTP_ERR_LOCALIO; }

    TcpSocket *ds = (TcpSocket *)d;
    unsigned long total = 0;
    uint8_t buf[DATA_CHUNK];
    int ioerr = 0;
    int code;

    rc = sendCmdArg("RETR", remote);
    if (rc != FTP_OK) { fclose(f); remove(localpath); closeData(d); state = FTP_IDLE; return rc; }

    /* Read "150/125" WHILE already draining the data connection: otherwise
     * the server fills the receive buffer during this wait, the window
     * drops below one MSS, and we get a one-time SWS stall right at the
     * start. */
    {
        DrainCtx dc;
        dc.ds = ds; dc.f = f; dc.buf = buf; dc.sz = (int)sizeof(buf);
        dc.total = &total; dc.err = 0;
        code = readReply(&dc);               /* 150 / 125 */
        if (dc.err) ioerr = 1;
        if (code < 0) { fclose(f); remove(localpath); closeData(d); state = FTP_IDLE; return code; }
        if (code != 150 && code != 125) {
            fclose(f); remove(localpath); closeData(d); state = FTP_IDLE;
            setErrorReply(L("RETR refused", "RETR abgelehnt"));
            return (code >= 400) ? FTP_ERR_SERVER : FTP_ERR_PROTO;
        }
    }

    clockTicks_t start = TIMER_GET_CURRENT();

    while (!ioerr) {
        int dr;
        driveStack();
        /* Drain the buffer completely on every pass: keeps the TCP window
         * open so the server doesn't stall due to Silly Window Syndrome. */
        dr = drainToFile(ds, f, buf, (int)sizeof(buf), &total);
        if (dr == -1) { ioerr = 1; break; }
        if (dr > 0) {
            start = TIMER_GET_CURRENT();
            if (cb) cb(ctx, total, filesize);
        }
        if (dr == -2) break;
        if (ds->isRemoteClosed() && !ds->recvDataWaiting()) break;
        if (elapsedMs(start) > DATA_TIMEOUT_MS) { ioerr = 2; break; }
    }

    fclose(f);
    closeData(d);

    if (ioerr == 1) { remove(localpath); setError(L("Write error (disk full?)", "Schreibfehler (Platte voll?)")); state = FTP_IDLE; return FTP_ERR_LOCALIO; }
    if (ioerr == 2) { setError(L("Download timeout", "Zeit" ue "berschreitung beim Download")); state = FTP_IDLE; return FTP_ERR_TIMEOUT; }

    code = readReply();                     /* 226 */
    state = FTP_IDLE;
    if (code < 0) return code;
    if (code >= 400) { setErrorReply(L("Download incomplete", "Download unvollst" ae "ndig")); return FTP_ERR_SERVER; }
    return FTP_OK;
}

int FtpClient::stor(const char *localpath, const char *remote,
                    FtpProgressCb cb, void *ctx) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }

    FILE *f = fopen(localpath, "rb");
    if (!f) { setError(L("Cannot read local file", "Lokale Datei nicht lesbar")); return FTP_ERR_LOCALIO; }

    unsigned long fsize = 0;                /* for progress reporting */
    if (fseek(f, 0L, SEEK_END) == 0) {
        long t = ftell(f);
        if (t > 0) fsize = (unsigned long)t;
        fseek(f, 0L, SEEK_SET);
    }

    int rc = simpleCmd("TYPE", "I");
    if (rc != FTP_OK) { fclose(f); if (rc == FTP_ERR_SERVER) setError(L("TYPE I refused", "TYPE I abgelehnt")); return rc; }

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
        setErrorReply(L("STOR refused", "STOR abgelehnt"));
        return (code >= 400) ? FTP_ERR_SERVER : FTP_ERR_PROTO;
    }

    TcpSocket *ds = (TcpSocket *)d;
    unsigned long sent = 0;
    uint8_t buf[DATA_CHUNK];
    int ioerr = 0;

    for (;;) {
        size_t r = fread(buf, 1, sizeof(buf), f);
        if (r == 0) break;

        uint16_t off = 0;
        clockTicks_t start = TIMER_GET_CURRENT();
        while (off < r) {
            driveStack();
            int16_t ns = ds->send(buf + off, (uint16_t)(r - off));
            if (ns > 0) {
                off += (uint16_t)ns; start = TIMER_GET_CURRENT();
            } else if (ns == 0) {
                if (elapsedMs(start) > DATA_TIMEOUT_MS) { ioerr = 2; break; }
            } else { ioerr = 3; break; }
        }
        if (ioerr) break;

        sent += (unsigned long)r;
        if (cb) cb(ctx, sent, fsize);
    }
    if (!ioerr && ferror(f)) ioerr = 1;

    fclose(f);
    closeData(d);                           /* blocking: flushes all data + FIN */

    if (ioerr == 1) { setError(L("Local read error", "Lokaler Lesefehler")); state = FTP_IDLE; return FTP_ERR_LOCALIO; }
    if (ioerr == 2) { setError(L("Upload timeout", "Zeit" ue "berschreitung beim Upload")); state = FTP_IDLE; return FTP_ERR_TIMEOUT; }
    if (ioerr == 3) { setError(L("Data connection lost", "Datenverbindung verloren")); state = FTP_IDLE; return FTP_ERR_DATACONN; }

    code = readReply();                     /* 226 */
    state = FTP_IDLE;
    if (code < 0) return code;
    if (code >= 400) { setErrorReply(L("Upload incomplete", "Upload unvollst" ae "ndig")); return FTP_ERR_SERVER; }
    return FTP_OK;
}


/* ===================================================================== */
/* Server file operations                                                */
/* ===================================================================== */

int FtpClient::remote_file_exists(const char *path) {
    if (!is_connected()) return 0;
    /* TYPE I, so SIZE also works on servers that refuse it in ASCII mode.
     * Errors here are ignored. */
    simpleCmd("TYPE", "I");
    if (sendCmdArg("SIZE", path) != FTP_OK) return 0;
    int code = readReply();
    return (code == 213) ? 1 : 0;
}

int FtpClient::make_dir(const char *path) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("MKD", path);        /* 257 */
    if (rc == FTP_ERR_SERVER) setErrorReply(L("Make directory failed", "Verzeichnis anlegen fehlgeschlagen"));
    return rc;
}

int FtpClient::remove_dir(const char *path) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("RMD", path);        /* 250 */
    if (rc == FTP_ERR_SERVER) setErrorReply(L("Remove directory failed", "Verzeichnis l" oe "schen fehlgeschlagen"));
    return rc;
}

int FtpClient::remove_file(const char *path) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }
    int rc = simpleCmd("DELE", path);       /* 250 */
    if (rc == FTP_ERR_SERVER) setErrorReply(L("Delete failed", "L" oe "schen fehlgeschlagen"));
    return rc;
}

int FtpClient::rename(const char *from, const char *to) {
    if (!is_connected()) { setError(L("Not connected", "Nicht verbunden")); return FTP_ERR_GENERAL; }

    int rc = sendCmdArg("RNFR", from);
    if (rc != FTP_OK) return rc;
    int code = readReply();
    if (code < 0) return code;
    if (code != 350) { setErrorReply(L("RNFR failed", "RNFR fehlgeschlagen")); return FTP_ERR_SERVER; }

    rc = sendCmdArg("RNTO", to);
    if (rc != FTP_OK) return rc;
    code = readReply();
    if (code < 0) return code;
    if (code >= 200 && code < 300) return FTP_OK;

    setErrorReply(L("Rename failed", "Umbenennen fehlgeschlagen"));
    return FTP_ERR_SERVER;
}
