/* Feature test macros */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

/*
 * zsh-capture-wrapper
 *
 * Spawns zsh inside a pseudo-terminal, transparently forwarding all I/O.
 * Parses OSC marker sequences emitted by the OMZ plugin to delimit
 * per-command output boundaries. Captured output is stored in POSIX
 * shared memory for zero-disk-I/O retrieval by `clc`.
 *
 * Architecture:
 *   Terminal <---> wrapper (master pty) <---> zsh (slave pty)
 *                    |
 *                    +---> shm ring buffer (last command output)
 *
 * Build: cc -O2 -o zsh-capture-wrapper wrapper.c
 * macOS: needs <util.h> for forkpty()
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <util.h>       /* macOS forkpty() */

/* ── Shared memory layout ────────────────────────────────────── */

#define SHM_NAME     "/zsh_cap2"
#define BUF_CAPACITY (4 * 1024 * 1024)  /* 4 MiB per-command cap */

/*
 * Double-buffered: `cur_*` is the in-progress capture, `last_*` holds the
 * most recently finalized command output. `clc` reads `last_*`, so the
 * preexec of `clc` itself (which resets `cur_*`) does not wipe what the
 * user wants to read.
 */
typedef struct {
    atomic_size_t last_len;
    atomic_int    last_ready;
    char          last_data[BUF_CAPACITY];

    atomic_size_t cur_len;
    char          cur_data[BUF_CAPACITY];
} CapBuf;

/* ── OSC marker protocol ─────────────────────────────────────── */
/*
 * The OMZ plugin emits these invisible sequences:
 *   preexec  → \033]7770;B\007           (BEGIN, no command)
 *              \033]7770;B;<cmd>\007     (BEGIN, with command)
 *   precmd   → \033]7770;E\007           (END)
 *
 * Payload is terminated by BEL (\a) — variable length so we can carry
 * the command line as a transcript header.
 */

#define MARKER_PREFIX     "\033]7770;"
#define MARKER_PREFIX_LEN 7
#define PENDING_MAX       4096

/* ── Globals ─────────────────────────────────────────────────── */

static int            g_master_fd   = -1;
static pid_t          g_child_pid   = 0;
static struct termios g_orig_tios;
static bool           g_tios_saved  = false;
static CapBuf        *g_buf         = NULL;

/* Marker parser state */
typedef enum {
    S_NORMAL,       /* forwarding bytes, watching for ESC        */
    S_IN_MARKER,    /* accumulating a potential marker sequence   */
} ParseState;

static ParseState  g_state     = S_NORMAL;
static char        g_pending[PENDING_MAX];
static size_t      g_pending_n = 0;

/* Capture state */
static bool g_capturing = false;

/* ── Terminal helpers ─────────────────────────────────────────── */

static void restore_terminal(void) {
    if (g_tios_saved)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_tios);
}

static void set_raw_mode(void) {
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &g_orig_tios) == 0) {
        g_tios_saved = true;
        raw = g_orig_tios;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
}

/* ── Signal handlers ─────────────────────────────────────────── */

static void on_sigwinch(int sig) {
    (void)sig;
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && g_master_fd >= 0)
        ioctl(g_master_fd, TIOCSWINSZ, &ws);
}

static void on_sigchld(int sig) {
    (void)sig;
    /* handled in main loop via waitpid */
}

/* Restore TTY before the default action takes the process down, so a
 * crash does not leave the user's terminal in raw mode. */
static void on_fatal(int sig) {
    restore_terminal();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ── Shared memory ───────────────────────────────────────────── */

static CapBuf *shm_init(void) {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return NULL; }

    /* macOS allows ftruncate only once per shm object — skip if already sized. */
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(CapBuf)) {
        if (ftruncate(fd, sizeof(CapBuf)) < 0) {
            perror("ftruncate");
            close(fd);
            return NULL;
        }
    }

    CapBuf *buf = mmap(NULL, sizeof(CapBuf),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (buf == MAP_FAILED) { perror("mmap"); return NULL; }

    atomic_store(&buf->last_len, 0);
    atomic_store(&buf->last_ready, 1);
    atomic_store(&buf->cur_len, 0);
    return buf;
}

static void shm_cleanup(void) {
    if (g_buf) {
        munmap(g_buf, sizeof(CapBuf));
        /* don't unlink — clc might still read it after wrapper exits */
    }
}

/* ── Capture buffer operations ───────────────────────────────── */

static void cap_reset(void) {
    /* Only resets the in-progress buffer; last_* is preserved so clc
       (whose preexec triggers this reset) can still read the previous
       command's output. */
    atomic_store(&g_buf->cur_len, 0);
}

static void cap_append(const char *data, size_t n) {
    size_t cur = atomic_load(&g_buf->cur_len);
    size_t space = BUF_CAPACITY - cur;
    size_t to_copy = n < space ? n : space;
    if (to_copy > 0) {
        memcpy(g_buf->cur_data + cur, data, to_copy);
        atomic_store(&g_buf->cur_len, cur + to_copy);
    }
}

static void cap_finalize(void) {
    /* Publish cur → last atomically. Raw bytes, no trailing-newline strip. */
    size_t len = atomic_load(&g_buf->cur_len);
    atomic_store(&g_buf->last_ready, 0);
    memcpy(g_buf->last_data, g_buf->cur_data, len);
    atomic_store(&g_buf->last_len, len);
    atomic_store(&g_buf->last_ready, 1);
}

/* ── Output processing with marker parsing ───────────────────── */

static void emit_byte_to_terminal(char c) {
    (void)write(STDOUT_FILENO, &c, 1);
}

static void emit_to_terminal(const char *data, size_t n) {
    (void)write(STDOUT_FILENO, data, n);
}

/*
 * Dispatch a completed marker payload (bytes after MARKER_PREFIX, BEL
 * excluded). First byte is the type: 'B' for begin, 'E' for end.
 * 'B' may be followed by ";<cmd>" carrying the command line.
 */
static void parse_marker(const char *payload, size_t len) {
    if (len == 0) return;
    char type = payload[0];

    if (type == 'B') {
        cap_reset();
        g_capturing = true;
        if (len >= 2 && payload[1] == ';') {
            cap_append("$ ", 2);
            cap_append(payload + 2, len - 2);
            cap_append("\n", 1);
        }
    } else if (type == 'E') {
        g_capturing = false;
        cap_finalize();
    }
    /* Unknown type: silently drop the marker. */
}

static void flush_pending(void) {
    emit_to_terminal(g_pending, g_pending_n);
    if (g_capturing)
        cap_append(g_pending, g_pending_n);
    g_pending_n = 0;
    g_state = S_NORMAL;
}

/*
 * Process a single byte through the marker detection state machine.
 * Markers are swallowed; everything else is forwarded to the terminal
 * and (when capturing) appended to the shm buffer.
 */
static void process_byte(char c) {
    switch (g_state) {

    case S_NORMAL:
        if (c == '\033') {
            g_pending[0] = c;
            g_pending_n = 1;
            g_state = S_IN_MARKER;
        } else {
            emit_byte_to_terminal(c);
            if (g_capturing)
                cap_append(&c, 1);
        }
        break;

    case S_IN_MARKER:
        if (g_pending_n >= PENDING_MAX) {
            /* Too long for a real marker — flush and re-handle this byte. */
            flush_pending();
            process_byte(c);
            return;
        }
        g_pending[g_pending_n++] = c;

        if (g_pending_n <= MARKER_PREFIX_LEN) {
            /* Still inside the fixed \e]7770; prefix. */
            if (c != MARKER_PREFIX[g_pending_n - 1]) {
                flush_pending();
            }
        } else if (c == '\a') {
            /* Payload complete: bytes between prefix and BEL. */
            size_t payload_len = g_pending_n - MARKER_PREFIX_LEN - 1;
            parse_marker(g_pending + MARKER_PREFIX_LEN, payload_len);
            g_pending_n = 0;
            g_state = S_NORMAL;
        }
        /* else: keep accumulating payload until BEL or overflow. */
        break;
    }
}

static void process_output(const char *data, size_t n) {
    for (size_t i = 0; i < n; i++)
        process_byte(data[i]);
}

/* ── Main loop ───────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* --check: verify we can init shared memory, then exit 0. Used by
     * .zshrc to decide whether it's safe to `exec` us. Keeps failures at
     * open time rather than mid-session. */
    if (argc >= 2 && strcmp(argv[1], "--check") == 0) {
        CapBuf *b = shm_init();
        if (!b) return 1;
        munmap(b, sizeof(CapBuf));
        return 0;
    }

    /* 1. Init shared memory */
    g_buf = shm_init();
    if (!g_buf) return 1;

    /* Install crash-safe handlers ASAP so TTY is restored on fatal sigs. */
    {
        struct sigaction fsa;
        memset(&fsa, 0, sizeof(fsa));
        fsa.sa_handler = on_fatal;
        sigaction(SIGSEGV, &fsa, NULL);
        sigaction(SIGBUS,  &fsa, NULL);
        sigaction(SIGABRT, &fsa, NULL);
        sigaction(SIGTERM, &fsa, NULL);
        sigaction(SIGHUP,  &fsa, NULL);
    }

    /* 2. Get current window size */
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0) {
        ws.ws_col = 80;
        ws.ws_row = 24;
    }

    /* 3. Fork with pty */
    g_child_pid = forkpty(&g_master_fd, NULL, NULL, &ws);
    if (g_child_pid < 0) {
        perror("forkpty");
        shm_cleanup();
        return 1;
    }

    if (g_child_pid == 0) {
        /* ── Child: exec zsh ── */
        setenv("ZSH_CAPTURE_ACTIVE", "1", 1);

        /* Prefer user's SHELL, fall back to zsh */
        const char *shell = getenv("SHELL");
        if (!shell) shell = "/bin/zsh";
        execlp(shell, shell, "--login", NULL);
        perror("exec");
        _exit(127);
    }

    /* ── Parent: relay I/O ── */

    /* Set terminal to raw mode so keystrokes pass through immediately */
    set_raw_mode();
    atexit(restore_terminal);
    atexit(shm_cleanup);

    /* Handle SIGWINCH for terminal resize */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigwinch;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    /* Make master_fd non-blocking for cleaner select() usage */
    int flags = fcntl(g_master_fd, F_GETFL);
    fcntl(g_master_fd, F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char iobuf[8192];
    bool running = true;

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(g_master_fd, &rfds);

        int maxfd = g_master_fd > STDIN_FILENO ? g_master_fd : STDIN_FILENO;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* stdin → master (user typing) */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, iobuf, sizeof(iobuf));
            if (n > 0) {
                (void)write(g_master_fd, iobuf, n);
            } else if (n == 0) {
                running = false;
            }
            /* EAGAIN is fine for non-blocking */
        }

        /* master → stdout (shell output) */
        if (FD_ISSET(g_master_fd, &rfds)) {
            ssize_t n = read(g_master_fd, iobuf, sizeof(iobuf));
            if (n > 0) {
                process_output(iobuf, n);
            } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                running = false;
            }
        }

        /* Check if child exited */
        int status;
        pid_t w = waitpid(g_child_pid, &status, WNOHANG);
        if (w > 0) {
            /* Drain remaining output */
            for (;;) {
                ssize_t n = read(g_master_fd, iobuf, sizeof(iobuf));
                if (n <= 0) break;
                process_output(iobuf, n);
            }
            running = false;
        }
    }

    restore_terminal();
    shm_cleanup();

    return 0;
}
