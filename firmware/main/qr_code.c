/* QR Code generation, byte mode, ECC level L, versions 1..6.
 *
 * The symbol is built in the caller's buffer, which doubles as scratch space:
 * while building, each byte holds a cell state (bit 0 = dark, bit 1 = function
 * module), and it is normalised to 0/1 before returning. Function modules are
 * laid down first, then the interleaved codeword bits, then the mask selected
 * by the specification's penalty rules.
 *
 * Reference: ISO/IEC 18004 clause 8. The tables below are the ECC level L
 * entries for versions 1..6; every one of those versions uses equal-size
 * blocks, so codeword interleaving needs no group table.
 */
#include "qr_code.h"

#include <stddef.h>
#include <string.h>

#define QR_VERSION_MAX 6

#define CELL_DARK 0x01u
#define CELL_FUNC 0x02u

/* Data codewords per symbol, ECC codewords per block, block count, and the
 * resulting byte-mode capacity (which already includes mode/length overhead). */
static const uint16_t s_data_cw[QR_VERSION_MAX + 1] = {0, 19, 34, 55, 80, 108, 136};
static const uint8_t s_ecc_cw[QR_VERSION_MAX + 1] = {0, 7, 10, 15, 20, 26, 18};
static const uint8_t s_blocks[QR_VERSION_MAX + 1] = {0, 1, 1, 1, 1, 1, 2};
static const uint16_t s_capacity[QR_VERSION_MAX + 1] = {0, 17, 32, 53, 78, 106, 134};

#define QR_DATA_CW_MAX 136
#define QR_ECC_CW_MAX 26
#define QR_BLOCKS_MAX 2

/* Penalty weights from ISO/IEC 18004 table 24. */
#define PENALTY_RUN 3
#define PENALTY_BLOCK 3
#define PENALTY_FINDER 40
#define PENALTY_BALANCE 10

/* GF(256) arithmetic modulo the QR primitive polynomial x^8 + x^4 + x^3 + x^2
 * + 1 (0x11D). Russian peasant multiplication keeps the code table-free. */
static uint8_t gf_mul(uint8_t x, uint8_t y)
{
    uint8_t z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (uint8_t)((z << 1) ^ ((z >> 7) * 0x1Du));
        z ^= (uint8_t)(((y >> i) & 1u) * x);
    }
    return z;
}

/* Coefficients of the generator polynomial of the requested degree, without
 * its leading 1 (the Reed-Solomon remainder divides by this tail). */
static void rs_generator(uint8_t degree, uint8_t *gen)
{
    memset(gen, 0, degree);
    gen[degree - 1] = 1;
    uint8_t root = 1;
    for (uint8_t i = 0; i < degree; i++) {
        for (uint8_t j = 0; j < degree; j++) {
            gen[j] = gf_mul(gen[j], root);
            if (j + 1 < degree) gen[j] ^= gen[j + 1];
        }
        root = gf_mul(root, 2);
    }
}

static void rs_remainder(const uint8_t *data, int len, const uint8_t *gen,
                         int degree, uint8_t *out)
{
    memset(out, 0, (size_t)degree);
    for (int i = 0; i < len; i++) {
        uint8_t factor = (uint8_t)(data[i] ^ out[0]);
        memmove(out, out + 1, (size_t)(degree - 1));
        out[degree - 1] = 0;
        for (int j = 0; j < degree; j++) out[j] ^= gf_mul(gen[j], factor);
    }
}

static void put_bits(uint8_t *buf, int *bitpos, uint32_t value, int count)
{
    for (int i = count - 1; i >= 0; i--) {
        int pos = *bitpos;
        if ((value >> i) & 1u) buf[pos >> 3] |= (uint8_t)(1u << (7 - (pos & 7)));
        (*bitpos)++;
    }
}

/* Mode indicator, character count, payload, terminator and pad codewords. */
static void build_data(const char *text, int len, int version, uint8_t *out)
{
    int data_cw = s_data_cw[version];
    int capacity = data_cw * 8;
    memset(out, 0, (size_t)data_cw);

    int bits = 0;
    put_bits(out, &bits, 0x4u, 4);              /* byte mode */
    put_bits(out, &bits, (uint32_t)len, 8);     /* versions 1..9 use 8 bits */
    for (int i = 0; i < len; i++) put_bits(out, &bits, (uint8_t)text[i], 8);

    if (bits + 4 <= capacity) bits += 4;
    while (bits < capacity && bits % 8 != 0) bits++;

    bool flip = false;
    for (int i = bits / 8; i < data_cw; i++) {
        out[i] = flip ? 0x11u : 0xECu;
        flip = !flip;
    }
}

static void set_cell(uint8_t *m, int size, int x, int y, bool dark)
{
    if (x < 0 || y < 0 || x >= size || y >= size) return;
    m[y * size + x] = (uint8_t)(CELL_FUNC | (dark ? CELL_DARK : 0));
}

/* Finder pattern and its separator: dark where the Chebyshev distance from the
 * center is 0, 1 or 3. */
static void draw_finder(uint8_t *m, int size, int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int ax = dx < 0 ? -dx : dx;
            int ay = dy < 0 ? -dy : dy;
            int dist = ax > ay ? ax : ay;
            set_cell(m, size, cx + dx, cy + dy, dist != 2 && dist != 4);
        }
    }
}

static void draw_alignment(uint8_t *m, int size, int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int ax = dx < 0 ? -dx : dx;
            int ay = dy < 0 ? -dy : dy;
            int dist = ax > ay ? ax : ay;
            set_cell(m, size, cx + dx, cy + dy, dist != 1);
        }
    }
}

/* Format information: 5 data bits (ECC level and mask) protected by a (15,5)
 * BCH code, then XORed with the specified mask pattern. Level L is 0b01. */
static void draw_format(uint8_t *m, int size, uint8_t mask)
{
    uint32_t data = (1u << 3) | mask;
    uint32_t rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
    uint32_t bits = ((data << 10) | rem) ^ 0x5412u;

    for (int i = 0; i <= 5; i++) set_cell(m, size, 8, i, (bits >> i) & 1u);
    set_cell(m, size, 8, 7, (bits >> 6) & 1u);
    set_cell(m, size, 8, 8, (bits >> 7) & 1u);
    set_cell(m, size, 7, 8, (bits >> 8) & 1u);
    for (int i = 9; i < 15; i++) set_cell(m, size, 14 - i, 8, (bits >> i) & 1u);

    for (int i = 0; i < 8; i++) set_cell(m, size, size - 1 - i, 8, (bits >> i) & 1u);
    for (int i = 8; i < 15; i++) set_cell(m, size, 8, size - 15 + i, (bits >> i) & 1u);
    set_cell(m, size, 8, size - 8, true);   /* the always-dark module */
}

static void draw_function_patterns(uint8_t *m, int size, int version)
{
    for (int i = 0; i < size; i++) {
        set_cell(m, size, 6, i, (i & 1) == 0);
        set_cell(m, size, i, 6, (i & 1) == 0);
    }
    draw_finder(m, size, 3, 3);
    draw_finder(m, size, size - 4, 3);
    draw_finder(m, size, 3, size - 4);

    if (version >= 2) {
        int centers[2] = {6, size - 7};
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                /* Skip the three corners already covered by finder patterns. */
                if ((i == 0 && j == 0) || (i == 0 && j == 1) || (i == 1 && j == 0)) continue;
                draw_alignment(m, size, centers[j], centers[i]);
            }
        }
    }
    /* Reserve the format modules so the data and mask passes skip them. */
    draw_format(m, size, 0);
}

/* Zigzag placement from the bottom-right corner, two columns at a time. */
static void place_codewords(uint8_t *m, int size, const uint8_t *cw, int n)
{
    int bit = 0;
    int total = n * 8;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;   /* the vertical timing column is skipped */
        bool upward = ((right + 1) & 2) == 0;
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int y = upward ? size - 1 - vert : vert;
                if ((m[y * size + x] & CELL_FUNC) != 0 || bit >= total) continue;
                m[y * size + x] = (uint8_t)((cw[bit >> 3] >> (7 - (bit & 7))) & 1u);
                bit++;
            }
        }
    }
}

static bool mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return (x * y) % 2 + (x * y) % 3 == 0;
    case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
    default: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
    }
}

/* Inverting only the data modules makes this its own inverse, so a trial mask
 * can be scored and undone without a second copy of the symbol. */
static void apply_mask(uint8_t *m, int size, int mask)
{
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            uint8_t *cell = &m[y * size + x];
            if ((*cell & CELL_FUNC) == 0 && mask_bit(mask, x, y)) *cell ^= CELL_DARK;
        }
    }
}

/* Run history for the finder-like (1:1:3:1:1) penalty rule. The quiet zone
 * counts as a run of light modules on either end of a row or column. */
static void add_history(int run, int *history, int size)
{
    if (history[0] == 0) run += size;
    memmove(&history[1], &history[0], 6 * sizeof(history[0]));
    history[0] = run;
}

static int count_finder_like(const int *h)
{
    int n = h[1];
    bool core = n > 0 && h[2] == n && h[3] == n * 3 && h[4] == n && h[5] == n;
    return (core && h[0] >= n * 4 && h[6] >= n) + (core && h[6] >= n * 4 && h[0] >= n);
}

static int terminate_and_count(bool color, int run, int *history, int size)
{
    if (color) {
        add_history(run, history, size);
        run = 0;
    }
    run += size;
    add_history(run, history, size);
    return count_finder_like(history);
}

static long penalty(const uint8_t *m, int size)
{
    long result = 0;

    for (int y = 0; y < size; y++) {
        bool color = false;
        int run = 0;
        int history[7] = {0};
        for (int x = 0; x < size; x++) {
            bool dark = (m[y * size + x] & CELL_DARK) != 0;
            if (dark == color) {
                run++;
                if (run == 5) result += PENALTY_RUN;
                else if (run > 5) result++;
            } else {
                add_history(run, history, size);
                if (!color) result += (long)count_finder_like(history) * PENALTY_FINDER;
                color = dark;
                run = 1;
            }
        }
        result += (long)terminate_and_count(color, run, history, size) * PENALTY_FINDER;
    }

    for (int x = 0; x < size; x++) {
        bool color = false;
        int run = 0;
        int history[7] = {0};
        for (int y = 0; y < size; y++) {
            bool dark = (m[y * size + x] & CELL_DARK) != 0;
            if (dark == color) {
                run++;
                if (run == 5) result += PENALTY_RUN;
                else if (run > 5) result++;
            } else {
                add_history(run, history, size);
                if (!color) result += (long)count_finder_like(history) * PENALTY_FINDER;
                color = dark;
                run = 1;
            }
        }
        result += (long)terminate_and_count(color, run, history, size) * PENALTY_FINDER;
    }

    for (int y = 0; y < size - 1; y++) {
        for (int x = 0; x < size - 1; x++) {
            uint8_t c = m[y * size + x] & CELL_DARK;
            if (c == (m[y * size + x + 1] & CELL_DARK) &&
                c == (m[(y + 1) * size + x] & CELL_DARK) &&
                c == (m[(y + 1) * size + x + 1] & CELL_DARK)) {
                result += PENALTY_BLOCK;
            }
        }
    }

    int dark = 0;
    for (int i = 0; i < size * size; i++) dark += (m[i] & CELL_DARK) != 0;
    long total = (long)size * size;
    long deviation = dark * 20L - total * 10L;
    if (deviation < 0) deviation = -deviation;
    result += ((deviation + total - 1) / total - 1) * PENALTY_BALANCE;
    return result;
}

static int choose_mask(uint8_t *m, int size)
{
    int best = 0;
    long best_score = -1;
    for (int mask = 0; mask < 8; mask++) {
        apply_mask(m, size, mask);
        draw_format(m, size, (uint8_t)mask);
        long score = penalty(m, size);
        apply_mask(m, size, mask);   /* undo; format modules are rewritten next round */
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best = mask;
        }
    }
    return best;
}

int qr_encode(const char *text, unsigned char *modules)
{
    if (text == NULL || modules == NULL) return 0;
    size_t len = strlen(text);

    int version = 0;
    for (int v = 1; v <= QR_VERSION_MAX; v++) {
        if (len <= s_capacity[v]) {
            version = v;
            break;
        }
    }
    if (version == 0) return 0;

    uint8_t data[QR_DATA_CW_MAX];
    build_data(text, (int)len, version, data);

    int data_cw = s_data_cw[version];
    int ecc_cw = s_ecc_cw[version];
    int blocks = s_blocks[version];
    int per_block = data_cw / blocks;

    uint8_t gen[QR_ECC_CW_MAX];
    uint8_t ecc[QR_ECC_CW_MAX * QR_BLOCKS_MAX];
    rs_generator((uint8_t)ecc_cw, gen);
    for (int b = 0; b < blocks; b++) {
        rs_remainder(data + b * per_block, per_block, gen, ecc_cw, ecc + b * ecc_cw);
    }

    uint8_t cw[QR_DATA_CW_MAX + QR_ECC_CW_MAX * QR_BLOCKS_MAX];
    int n = 0;
    for (int i = 0; i < per_block; i++) {
        for (int b = 0; b < blocks; b++) cw[n++] = data[b * per_block + i];
    }
    for (int i = 0; i < ecc_cw; i++) {
        for (int b = 0; b < blocks; b++) cw[n++] = ecc[b * ecc_cw + i];
    }

    int size = 4 * version + 17;
    memset(modules, 0, (size_t)(size * size));
    draw_function_patterns(modules, size, version);
    place_codewords(modules, size, cw, n);
    int mask = choose_mask(modules, size);
    apply_mask(modules, size, mask);
    draw_format(modules, size, (uint8_t)mask);
    for (int i = 0; i < size * size; i++) modules[i] &= CELL_DARK;
    return size;
}
