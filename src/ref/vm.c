/* src/ref/vm.c -- fixed: execute SPA->BF as interpreter instead of dumping BF text */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* isatty */
#include <sys/types.h>
#include <sys/stat.h>

/* Windows-specific headers if needed */
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// src/ref/vm.c の変更
#define TAPE_SIZE 1048576          /* 1MB に拡張 */
#define MAX_FILE_SIZE 1073741824   /* 1GB limit に拡張 */

unsigned char tape[TAPE_SIZE];
int ptr = 0;

/* Mapping: 3-bit code -> BF op (ASCII) */
char op_map[8] = {'>', '<', '+', '-', '.', ',', '[', ']'};

void panic(const char *msg) {
    fprintf(stderr, "[VM Error] %s\n", msg);
    exit(1);
}

/* UTF-8 full-width space detection with Bounds Checking */
int is_full_space(const unsigned char *s, int idx, int len) {
    if (idx + 2 >= len) return 0;
    if (s[idx] == 0xE3 && s[idx+1] == 0x80 && s[idx+2] == 0x80) {
        return 1;
    }
    return 0;
}

/* parse_line: convert line with spaces/full-width spaces into BF source (ASCII) */
int parse_line(const char *input, int input_len, char *output, int max_out) {
    int out_idx = 0;
    int bit_buf = 0;
    int bit_cnt = 0;

    for (int i = 0; i < input_len && input[i] != 0; i++) {
        int bit = -1;
        unsigned char uc = (unsigned char)input[i];

        if (uc == 0x20) {
            bit = 0;
        } else if (uc == 0xE3) {
            if (is_full_space((const unsigned char*)input, i, input_len)) {
                bit = 1;
                i += 2; /* skip the remaining bytes of full-width space */
            }
        }

        if (bit != -1) {
            bit_buf = (bit_buf << 1) | (bit & 1);
            bit_cnt++;
            if (bit_cnt == 3) {
                if (out_idx >= max_out - 1) panic("Output buffer overflow");
                output[out_idx++] = (char)op_map[bit_buf & 0x7];
                bit_buf = 0;
                bit_cnt = 0;
            }
        }
    }
    output[out_idx] = 0;
    return out_idx;
}

/* Brainfuck runner -- unchanged except small safety checks */
void run_bf(char *code) {
    char *pc = code;
    memset(tape, 0, sizeof(tape));
    ptr = 0;

    while (*pc) {
        switch (*pc) {
            case '>':
                ptr++;
                if (ptr >= TAPE_SIZE) panic("Tape pointer overflow (Right)");
                break;
            case '<':
                ptr--;
                if (ptr < 0) panic("Tape pointer underflow (Left)");
                break;
            case '+':
                tape[ptr]++;
                break;
            case '-':
                tape[ptr]--;
                break;
            case '.':
                putchar(tape[ptr]);
                break;
            case ',':
            {
                int c = getchar();
                tape[ptr] = (c == EOF) ? 0 : (unsigned char)c;
                break;
            }
            case '[':
                if (!tape[ptr]) {
                    int loop = 1;
                    while (loop > 0) {
                        pc++;
                        if (!*pc) panic("Unmatched '['");
                        if (*pc == '[') loop++;
                        if (*pc == ']') loop--;
                    }
                }
                break;
            case ']':
                if (tape[ptr]) {
                    int loop = 1;
                    while (loop > 0) {
                        if (pc == code) panic("Unmatched ']'");
                        pc--;
                        if (*pc == '[') loop--;
                        if (*pc == ']') loop++;
                    }
                }
                break;
            default:
                /* ignore other bytes */
                break;
        }
        pc++;
    }
}

/* Helper: process an in-memory buffer that may be SPA header or spaces-encoded */
void process_buffer(unsigned char *in, size_t n) {
    /* allocate bc buffer */
    size_t bc_cap = n + 128;
    if (bc_cap < 4096) bc_cap = 4096;
    char *bc = malloc(bc_cap);
    if (!bc) panic("Alloc fail");

    if (n >= 3 && in[0] == 'S' && in[1] == 'P' && in[2] == 'A') {
        /* SPA binary format: map 1..8 -> bf ops */
        size_t out_idx = 0;
        for (long i = 3; i < (long)n; i++) {
            unsigned char op = in[i];
            char mapped = 0;
            if (op >= 1 && op <= 8) mapped = op_map[op - 1];
            if (mapped) bc[out_idx++] = mapped;
            if (out_idx + 16 >= bc_cap) {
                bc_cap *= 2;
                char *tmp = realloc(bc, bc_cap);
                if (!tmp) { free(bc); panic("Alloc fail"); }
                bc = tmp;
            }
        }
        bc[out_idx] = 0;

        /* === CRITICAL FIX ===
           Instead of dumping BF source to stdout, we must *execute* the BF program
           (the compiler) so it will read stdin (the code to compile) and emit bytes
           (compiled ELF) to stdout via '.' operations. */
        run_bf(bc);

    } else {
        /* spaces-encoded BF: parse whole input into bf string and run it */
        int parsed = parse_line((const char*)in, (int)n, bc, (int)bc_cap);
        if (parsed > 0) {
            run_bf(bc);
        }
    }
    free(bc);
}

int main(int argc, char **argv) {
    /* Windows: force stdout binary if needed */
    #ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    #endif

    if (argc > 1) {
        /* file provided as argument: read whole file (this is the compiler BF) and execute it.
           The compiler BF will read stdin (the source to compile) and write compiled bytes to stdout. */
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror("File open error"); return 1; }

        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); panic("fseek failed"); }
        long s = ftell(f);
        if (s < 0 || s > MAX_FILE_SIZE) { fclose(f); fprintf(stderr, "File too large\n"); return 1; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); panic("fseek failed"); }

        unsigned char *in = malloc((size_t)s + 1);
        if (!in) { fclose(f); panic("Alloc fail"); }
        size_t n = fread(in, 1, (size_t)s, f);
        fclose(f);
        in[n] = 0;

        /* IMPORTANT: do NOT consume stdin here. The BF compiler (run_bf) will read from stdin
           (which in the CI invocation is the vm.spaces piped into the process). */
        process_buffer(in, n);

        free(in);
        return 0;
    }

    /* No argv: decide interactive REPL vs non-interactive (piped) stdin */
    if (!isatty(fileno(stdin))) {
        /* Non-interactive: read all stdin into buffer and process once */
        size_t cap = 8192;
        size_t n = 0;
        unsigned char *in = malloc(cap + 1);
        if (!in) panic("Alloc fail");
        while (1) {
            size_t toread = cap - n;
            if (toread == 0) {
                cap *= 2;
                unsigned char *tmp = realloc(in, cap + 1);
                if (!tmp) { free(in); panic("Alloc fail"); }
                in = tmp;
                toread = cap - n;
            }
            size_t r = fread(in + n, 1, toread, stdin);
            if (r == 0) break;
            n += r;
            if (n > MAX_FILE_SIZE) { free(in); panic("Input too large"); }
        }
        in[n] = 0;
        process_buffer(in, n);
        free(in);
        return 0;
    }

    /* TTY interactive REPL */
    fprintf(stderr, "Spaces REPL (Safe Mode)\n");
    char line[4096];
    char bc[4096];
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) break;
        if (parse_line(line, (int)strlen(line), bc, (int)sizeof(bc)) > 0) {
            run_bf(bc);
            printf("\n");
        }
    }
    return 0;
}