# MDEC decoder: functional specification

This is the design input, together with psx-spx, for the clean-room rewrite
of the MDEC numeric core (`runtime/src/mdec_core.c`). It says what the core
must compute: the arithmetic, the order of operations and the edge cases. It
contains no code from any implementation and does not describe how one is
organised.

## Ground rules for the implementer

- Read psx-spx's chapter "Macroblock Decoder (MDEC)"
  (<https://psx-spx.consoledev.net/macroblockdecodermdec/>) and this file.
- Do not open the derived material in the history: `runtime/src/mdec.c`
  before the core split, the removed `runtime/src/mdec_core_legacy.c` and
  `tools/mdec_pin/`, and `accuracy/axis5_mdec.md` before its 2026-10
  rewrite. Do not open Beetle PSX / Mednafen / DuckStation sources either, or
  `mdec_ref/` in a build tree (the reference decoder that `mdec_replay_ab`
  extracts).
- The reference decoder is an oracle: run `mdec_replay_ab` and read its
  report, never its source (section 8).
- The output must be byte-identical to the current decoder's (section 8).

## Origin tags

Every rule carries its origin:

- **[psx-spx]**: documented there as stated.
- **[psx-spx, approximate]**: psx-spx documents the operation but marks its
  precision as uncertain ("or so?", "unknown"). The exact rule given here is
  what the output requires.
- **[Mednafen]**: hardware behaviour first published by the Mednafen
  project's research on the MDEC. These are facts about the hardware, given
  here as arithmetic. No code, structure or naming comes from there.
- **[project]**: decided by this project: interfaces, wire formats, and how
  the device model behaves at the edges.

## 1. Where the core sits

`runtime/src/mdec.c`, the device, stays: command parsing, FIFOs, DMA, the
status register, the snapshot, the statistics and the 4/8-bit output. It
calls three functions that the core provides, declared in
`runtime/include/mdec_core.h` **[project]**:

```c
/* Command 3: the 64 halfwords as received -> the table the decoder keeps
 * (and the snapshot stores; section 3). */
void mdec_core_set_scale(int16_t table[64], const uint16_t raw[64]);

/* One block from in[*pos .. end). Returns 0, consuming nothing, when
 * *pos >= end. Otherwise returns 1 with out[8*y + x] the decoded samples
 * (-128..127), *pos just past the last word read and *qscale the quantiser
 * scale field of the block's first word (section 4). quant: the luma or
 * chroma table, 64 bytes in scan order as uploaded by command 2. */
int mdec_core_decode_block(int16_t out[64], const uint16_t *in,
                           uint32_t *pos, uint32_t end,
                           const uint8_t quant[64], const int16_t table[64],
                           uint32_t *qscale);

/* One 16x16 colour macroblock from its six decoded blocks: 768 bytes at out
 * for depth 2 (24-bit), 512 for depth 3 (15-bit) (section 5). */
void mdec_core_color_macroblock(uint8_t *out, const int16_t cr[64],
                                const int16_t cb[64], const int16_t y[4][64],
                                unsigned depth, int is_signed, int bit15);
```

SIMD is optional. Any vector path must give exactly the scalar results.

## 2. Notation

- Arithmetic is on mathematical integers. Every intermediate below fits in a
  signed 32-bit integer.
- `floor(a / b)`: division rounding toward minus infinity. When b is a power
  of two this is an arithmetic right shift.
- `sgn(v)`: -1, 0 or +1.
- `sat(v, lo, hi)`: v limited to [lo, hi].
- `wrap9(v)`: v reduced modulo 512 into -256..255, i.e. its low 9 bits read
  as a signed number.
- `clip8(v) = sat(wrap9(v), -128, 127)`: the wrap comes first, so 300
  becomes -212 and then -128.
- `sext10(w)`: the low 10 bits of w read as a signed number (-512..511).
- Bits are numbered from 0 (least significant).
- A block is an 8x8 array indexed [row][column]. Row-major storage puts
  [r][c] at index 8r + c.

## 3. Scale table (command 3)

3.1 The command's 64 halfwords raw[0..63], read as signed 16-bit numbers,
form an 8x8 matrix in row-major order: raw[8r + c] is row r, column c. Row r
is the r-th cosine basis function (r is the frequency) and column c is the
sample position. The standard table is built from s0..s7 in the pattern
psx-spx shows, but any values are accepted. **[psx-spx]**

3.2 Only the upper 13 bits of each entry are used:
`S[r][c] = floor(raw[8r + c] / 8)`, so -4096 <= S <= 4095. **[psx-spx]**

3.3 Stored layout **[project]**: `mdec_core_set_scale` writes
`table[8c + r] = S[r][c]`, the transpose of the upload order, as int16. This
is the snapshot's wire format (the 64 int16 that follow the two quant
tables). It must not change, so that existing savestates load and netplay
digests stay equal. The core may keep any private form in addition, as long
as what it decodes with is the S held in this table.

## 4. Block decode (`mdec_core_decode_block`)

### 4.1 Walking the stream

a. If *pos >= end, return 0 and consume nothing. **[project]**

b. Words equal to 0xFE00 before a block are padding and are skipped while
   more input remains. **[psx-spx]** If the input runs out while skipping,
   the last 0xFE00 read becomes the block's first word: qscale 63, value
   -512. **[project]**

c. The first word gives `qscale` = bits 15..10 (0..63) and the DC value
   `sext10` of bits 9..0, at scan index k = 0. Report qscale through
   *qscale. **[psx-spx]**

d. Then, while input remains and k < 63, read a word:
   - If it is 0xFE00 (end of block), stop. The word is consumed. **[psx-spx]**
   - Otherwise set k = k + (bits 15..10) + 1. If k >= 64, stop: the word is
     consumed and nothing is stored. Otherwise its value `sext10(bits 9..0)`
     is the coefficient at scan index k. **[psx-spx]**

   Once index 63 has been stored, the walk stops without reading another
   word. An end code that follows is left for the next block, where 4.1b
   skips it. **[project]**

e. Coefficients that are not given are 0. *pos ends just past the last word
   read.

### 4.2 Dequantisation

psx-spx: the DC value uses quant[0] without qscale; an AC value uses
qscale x quant[k] and is divided by 8; qscale 0 means x2 without the table;
results saturate to the -0x400..+0x3FF band. **[psx-spx, approximate]**

The exact rule works in sixteenths of psx-spx's units. For the value x given
at scan index k: **[Mednafen]**

- DC (k = 0): q = quant[0]. If q != 0, `v = 16*x*q - 8*sgn(x)`. If q = 0,
  `v = 32*x`.
- AC (k >= 1): q = qscale * quant[k]. If q != 0,
  `v = 16*floor(x*q / 8) - 8*sgn(x)`. If q = 0, `v = 32*x`.
- Then `v = sat(v, -16384, 16383)`.

The "q = 0" case is decided by the product: a zero quant entry counts as
well as a zero qscale. The DC value never uses qscale.

### 4.3 Placement

v goes to the natural position p = zagzig[k], where zagzig is the inverse of
psx-spx's zigzag table (zagzig[zigzag[i]] = i). Writing p = 8r + c, row r is
the vertical frequency and column c the horizontal one:
C[r][c] = v. **[psx-spx]** zagzig begins 0, 1, 8, 16, 9, 2, 3, 10, 17, 24.

The same placement applies when q = 0. **[Mednafen]** (psx-spx describes
q_scale = 0 as storing without the zig-zag; see section 7.)

### 4.4 Inverse DCT

Two separable passes with the table S of section 3, the vertical pass first
(psx-spx's real_idct_core order). **[psx-spx]**

The exact rounding and limits are below. psx-spx gives `(sum+0FFFh)/2000h`
per pass with the table divided by 8, "or so?". **[Mednafen]**

    R(s) = floor((s + 16384) / 32768)

    pass 1:  T[y][h] = R( sum over u = 0..7 of S[u][y] * C[u][h] )
    pass 2:  P[y][x] = clip8( R( sum over h = 0..7 of T[y][h] * S[h][x] ) )

    out[8y + x] = P[y][x]     (row y from the top, column x from the left)

For any table and coefficients, |T| <= 16384, so keeping T in 16 bits never
changes a value. The final clip8 (a 9-bit wrap, then saturation) applies to
every block, luma and chroma alike. psx-spx documents that operation for
luma in monochrome output ("AND 1FFh", then MinMax); its use on every block
is **[Mednafen]**.

## 5. Colour macroblock (`mdec_core_color_macroblock`)

### 5.1 Layout

The six blocks arrive as Cr, Cb, Y0, Y1, Y2, Y3. Cr and Cb are decoded with
the chroma quant table, the Y blocks with the luma one. Y0 is the top-left
quarter of the 16x16 macroblock, Y1 top-right, Y2 bottom-left and Y3
bottom-right. **[psx-spx]**

Pixel (x, y), with x and y in 0..15, takes:

- `Y = Yb[8*(y mod 8) + (x mod 8)]`, where b = 2*(y >= 8) + (x >= 8);
- Cr and Cb at index `(x div 2) + 8*(y div 2)` of their blocks. **[psx-spx]**

Pixels are emitted in row-major order: row 0 left to right, then row 1, and
so on.

### 5.2 YCbCr to RGB

psx-spx: `R = Y + 1.402*Cr`, `G = Y - 0.3437*Cb - 0.7143*Cr`,
`B = Y + 1.772*Cb`, each saturated to -128..127, "exact fixed point
resolution unknown", "probably also some 9bit limit".
**[psx-spx, approximate]**

The exact rule: **[Mednafen]**

    R = clip8( Y + floor((359*Cr + 128) / 256) )
    G = clip8( Y + floor((32*floor(-88*Cb / 32) + 8*floor(-183*Cr / 8) + 128) / 256) )
    B = clip8( Y + floor((454*Cb + 128) / 256) )

359, 88, 183 and 454 are psx-spx's coefficients times 256, rounded to the
nearest integer. Three things are **[Mednafen]**: the 1/256 resolution, the
+128 rounding, and the green terms each truncated to a multiple of 32 (Cb)
and of 8 (Cr) before they are summed. Y + chroma can leave the 9-bit range
(127 + 178 = 305); clip8 then wraps it before saturating.

### 5.3 24-bit output (depth 2)

Three bytes per pixel in the order R, G, B (psx-spx's 24-bit BGR value,
stored little-endian). Each byte is `v + 128` for unsigned output, or v as a
two's-complement byte for signed output; equivalently
`(v + 128) XOR (signed ? 0x80 : 0)`. **[psx-spx]**

### 5.4 15-bit output (depth 3)

For each channel take `c = v + 128` (0..255) and reduce it to five bits:
`c5 = min(31, floor((c + 4) / 8))` **[Mednafen]**. Then pack
`w = R5 | G5 << 5 | B5 << 10` (the GPU's 15-bit layout). **[psx-spx]**

For signed output, XOR w with 0x4210: the top bit of each 5-bit channel
flips, the same signed/unsigned relation psx-spx gives for the other depths.
**[psx-spx, applied per channel]** If the command's bit15 flag (bit 25) is
set, XOR w with 0x8000. **[psx-spx]** Store w as two bytes, low byte first.

## 6. Kept by the device (context, not part of the core)

**[project]**, as in the current `mdec.c`:

- **Command 2**: 64 luma bytes, then 64 chroma bytes when bit 0 is set;
  otherwise the chroma table becomes a copy of the luma one.
- **Colour decode** (depth 2 or 3): macroblocks are decoded while input
  remains. A macroblock whose blocks run out of input is discarded.
- **Monochrome decode** (depth 0 or 1): each block that
  `mdec_core_decode_block` returns becomes 64 bytes in row-major order,
  `Y + 128` or Y as a signed byte. Depth 0 produces the same bytes as depth
  1 (section 7).
- **Counters**: a block counter in the snapshot goes up once per block
  returned. `mdec_mean_qscale_x16` is the rounded mean, times 16, of the
  qscale of every block in the last colour decode.

## 7. Differences from psx-spx deliberately preserved

The rewrite must keep these, because the output has to stay identical. Fix
them later, one change each, with the pinned fingerprints updated knowingly:

- q = 0 still uses the zig-zag placement (4.3).
- Depth 0 (4-bit) outputs one byte per pixel, the same as depth 1. Going by
  its name, a 4-bit depth packs two pixels per byte.
- The status register's current-block field stays 4.
- A final run of 0xFE00 at the end of the input becomes a block (4.1b).

## 8. Proving it

Build the framework tests with a reference, i.e. any commit whose decoder is
the current one (for example the one that adds this file):

```
cmake -S recompiler -B build/fwtest -G Ninja -DBUILD_TESTING=ON \
      -DMDEC_REF_COMMIT=<commit>
cmake --build build/fwtest --target mdec_replay mdec_replay_ab \
      mdec_capture_test mdec_busy_drain_test
```

Done means all of the following hold:

1. `mdec_replay --synthetic 1 6000 --expect d3c6fd9fc4d8bf4f` passes
   (registered as `mdec_replay_synthetic_test`).
2. `mdec_replay_ab --synthetic S 20000` reports "reference: identical" for
   several seeds S (1, 7, 12345, ...).
3. A capture of a real movie (`PSX_MDEC_CAPTURE`; game data, never commit
   it) replays with `mdec_replay --corpus FILE --check-live` and 0
   mismatches. `--check-live` compares against the hashes the old decoder
   recorded live.
4. `mdec_replay_corpus_test`, `mdec_capture_test`, `mdec_busy_drain_test`
   and `mdec_snapshot_activity_test` still pass, and the snapshot format is
   unchanged.

When something differs, `mdec_replay_ab` stops at the first differing event.
It prints the macroblock and pixel (or the snapshot field) with the expected
and actual values and the RLE of the blocks involved. `--show N` prints that
event's whole input.
