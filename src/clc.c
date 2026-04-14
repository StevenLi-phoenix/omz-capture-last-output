/* Feature test macros — needed for popen/pclose with strict -std=c17 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

/*
 * clc — Copy Last Command output to clipboard
 *
 * Reads the captured output from POSIX shared memory written by
 * zsh-capture-wrapper, optionally strips ANSI escape sequences,
 * and pipes the result to pbcopy (macOS).
 *
 * Usage:
 *   clc              Copy last command output (raw, with colors)
 *   clc --strip      Copy with ANSI escapes stripped (plain text)
 *   clc --print      Print to stdout instead of clipboard
 *   clc --info       Show buffer stats
 *
 * Build: cc -O2 -o clc clc.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define SHM_NAME     "/zsh_cap2"
#define BUF_CAPACITY (4 * 1024 * 1024)

typedef struct {
    atomic_size_t last_len;
    atomic_int    last_ready;
    char          last_data[BUF_CAPACITY];

    atomic_size_t cur_len;
    char          cur_data[BUF_CAPACITY];
} CapBuf;

/* ── ANSI escape stripper ────────────────────────────────────── */

/*
 * Strips ANSI/VT100 escape sequences from `src` into `dst`.
 * Returns the length of stripped output.
 *
 * Handles:
 *   CSI sequences:  ESC [ ... <final byte 0x40-0x7E>
 *   OSC sequences:  ESC ] ... (ST | BEL)
 *   Simple escapes: ESC <single char>
 *   Carriage returns (\r) followed by content (overwrite lines)
 */
static size_t strip_ansi(const char *src, size_t src_len,
                         char *dst, size_t dst_cap) {
    size_t j = 0;
    size_t i = 0;

    while (i < src_len && j < dst_cap - 1) {
        if (src[i] == '\033') {
            i++;
            if (i >= src_len) break;

            if (src[i] == '[') {
                /* CSI sequence: skip until 0x40-0x7E */
                i++;
                while (i < src_len && (src[i] < 0x40 || src[i] > 0x7E))
                    i++;
                if (i < src_len) i++; /* skip final byte */
            } else if (src[i] == ']') {
                /* OSC sequence: skip until BEL or ST */
                i++;
                while (i < src_len) {
                    if (src[i] == '\007') { i++; break; }
                    if (src[i] == '\033' && i + 1 < src_len &&
                        src[i + 1] == '\\') {
                        i += 2; break;
                    }
                    i++;
                }
            } else if (src[i] == '(') {
                /* Character set designation: ESC ( X */
                i += 2;
            } else {
                /* Other single-char escape */
                i++;
            }
        } else if (src[i] == '\r') {
            /* Handle \r\n (keep as \n) vs bare \r (line overwrite) */
            if (i + 1 < src_len && src[i + 1] == '\n') {
                dst[j++] = '\n';
                i += 2;
            } else {
                /* Bare \r — rewind to start of current line in output */
                while (j > 0 && dst[j - 1] != '\n')
                    j--;
                i++;
            }
        } else {
            dst[j++] = src[i++];
        }
    }

    dst[j] = '\0';
    return j;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    bool opt_strip = false;
    bool opt_print = false;
    bool opt_info  = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strip") == 0 || strcmp(argv[i], "-s") == 0)
            opt_strip = true;
        else if (strcmp(argv[i], "--print") == 0 || strcmp(argv[i], "-p") == 0)
            opt_print = true;
        else if (strcmp(argv[i], "--info") == 0 || strcmp(argv[i], "-i") == 0)
            opt_info = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "clc — Copy Last Command output to clipboard\n"
                "\n"
                "Usage:\n"
                "  clc              Copy to clipboard (raw)\n"
                "  clc -s, --strip  Strip ANSI escapes first\n"
                "  clc -p, --print  Print to stdout instead\n"
                "  clc -i, --info   Show buffer info\n"
                "  clc -sp          Strip + print\n"
                "  clc -h, --help   This message\n"
            );
            return 0;
        } else {
            /* Handle combined short flags like -sp */
            if (argv[i][0] == '-' && argv[i][1] != '-') {
                for (int k = 1; argv[i][k]; k++) {
                    switch (argv[i][k]) {
                        case 's': opt_strip = true; break;
                        case 'p': opt_print = true; break;
                        case 'i': opt_info  = true; break;
                        default:
                            fprintf(stderr, "clc: unknown flag '-%c'\n",
                                    argv[i][k]);
                            return 1;
                    }
                }
            }
        }
    }

    /* Open shared memory */
    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "clc: no capture buffer found "
                        "(is zsh-capture-wrapper running?)\n");
        return 1;
    }

    CapBuf *buf = mmap(NULL, sizeof(CapBuf), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (buf == MAP_FAILED) {
        perror("clc: mmap");
        return 1;
    }

    size_t len   = atomic_load(&buf->last_len);
    int    ready = atomic_load(&buf->last_ready);

    if (opt_info) {
        printf("shm:      %s\n", SHM_NAME);
        printf("capacity: %d bytes (%.1f MiB)\n",
               BUF_CAPACITY, BUF_CAPACITY / (1024.0 * 1024.0));
        printf("used:     %zu bytes\n", len);
        printf("ready:    %s\n", ready ? "yes" : "no (capture in progress)");
        munmap(buf, sizeof(CapBuf));
        return 0;
    }

    if (len == 0) {
        fprintf(stderr, "clc: capture buffer is empty\n");
        munmap(buf, sizeof(CapBuf));
        return 1;
    }

    if (!ready) {
        fprintf(stderr, "clc: ⚠ capture still in progress, "
                        "output may be incomplete\n");
    }

    /* Prepare output data */
    const char *out_data;
    size_t      out_len;
    char       *stripped = NULL;

    if (opt_strip) {
        stripped = malloc(len + 1);
        if (!stripped) {
            perror("clc: malloc");
            munmap(buf, sizeof(CapBuf));
            return 1;
        }
        out_len  = strip_ansi(buf->last_data, len, stripped, len + 1);
        out_data = stripped;
    } else {
        out_data = buf->last_data;
        out_len  = len;
    }

    if (opt_print) {
        (void)write(STDOUT_FILENO, out_data, out_len);
        /* Ensure terminal ends on a fresh line so zsh PROMPT_SP does
         * not draw the "%" partial-line indicator. Only add if the
         * captured content does not already end with \n. */
        if (out_len == 0 || out_data[out_len - 1] != '\n')
            (void)write(STDOUT_FILENO, "\n", 1);
    } else {
        /* Pipe to pbcopy */
        FILE *pb = popen("pbcopy", "w");
        if (!pb) {
            perror("clc: popen pbcopy");
            free(stripped);
            munmap(buf, sizeof(CapBuf));
            return 1;
        }
        fwrite(out_data, 1, out_len, pb);
        int ret = pclose(pb);
        if (ret == 0) {
            fprintf(stderr, "✓ copied %zu bytes%s\n",
                    out_len, opt_strip ? " (ansi stripped)" : "");
        } else {
            fprintf(stderr, "clc: pbcopy failed (exit %d)\n", ret);
        }
    }

    free(stripped);
    munmap(buf, sizeof(CapBuf));
    return 0;
}
