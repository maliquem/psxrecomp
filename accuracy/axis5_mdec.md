# Axis 5 — MDEC (Motion Decoder / FMV) accuracy status

Scope: `runtime/src/mdec.c` (the device) and `runtime/src/mdec_core.c` (the
numeric core), measured against psx-spx "Macroblock Decoder (MDEC)". The
numeric rules, each tagged with its origin, are in
`docs/MDEC_DECODER_SPEC.md`.

This file used to quote another emulator's MDEC source line by line. That
analysis was retired when the core was rewritten clean-room (2026-10).

---

## 1. What the implementation does

- **Device** (`mdec.c`):
  - parses commands (`cmd >> 29`: 1 decode, 2 quant tables, 3 scale table)
    and latches the output flags from command bits 25-28 (bit15, signed,
    depth);
  - builds the status register;
  - runs DMA channels 0/1;
  - keeps the snapshot (savestates, netplay digests), the FMV-activity stamp,
    the qscale statistic and the `PSX_MDEC_CAPTURE` recorder.
- **One-shot decode**: a DECODE command's whole input is buffered, decoded at
  once and then streamed to DMA. Status bit 29 (Command Busy) stays set while
  more than one 32-word FIFO load of output is unread, because libpress
  `DecDCTin` polls it before sending the next frame.
- **Numeric core** (`mdec_core.c`): the scale table, the stream walk,
  dequantisation, zig-zag placement, the two-pass inverse DCT, YCbCr to RGB
  and 24/15-bit packing, exactly as the spec says. The 4/8-bit output stays in
  the device.

## 2. Resolved

The FMV that used to look slightly off (contrast, ringing, greens off-hue,
block banding) came from approximate dequantisation, IDCT rounding and colour
conversion. The decoder now follows the exact rules of spec sections 4-5:

- dequantisation precision and its sign bias;
- per-pass IDCT rounding and the 9-bit limit;
- the 1/256 colour conversion with the truncated green terms;
- 15-bit rounding.

Its output is pinned against the previous decoder's (section 4). It has not
been checked against real hardware here.

## 3. Open gaps

- **G1 [MED] 4-bit output.** Depth 0 outputs one byte per pixel, like depth
  1. Going by its name, a 4-bit depth packs two pixels per byte. This affects
  MDEC texture decompression only, not FMV. (Spec section 7.)
- **G2 [MED] No FIFO / streaming model.** With a one-shot decode:
  - the remaining-words field (status bits 0-15) collapses as soon as the
    input is buffered;
  - the DMA request bits (27/28) follow single-word readiness rather than
    32-word FIFO blocks;
  - FIFO full/empty timing differs from hardware.
  A game that paces the MDEC by polling those bits, instead of by DMA block
  counts, could see different timing.
- **G3 [LOW] Current block** (status bits 16-18) stays at 4. psx-spx lists
  the block being decoded (Y1-Y4, Cr, Cb). This only means something with a
  streaming decode (G2).
- **G4 [LOW] q = 0 placement.** The decoder keeps the zig-zag placement when
  the quantiser product is 0. psx-spx describes that mode as storing without
  the zig-zag. Unverified on hardware. (Spec 4.3 and section 7.)
- **G5 [LOW] Trailing padding.** A run of 0xFE00 at the very end of a
  command's input becomes one more block (qscale 63, DC -512). In
  monochrome mode it is emitted. Movies are unaffected, because the colour
  path discards the incomplete macroblock.

## 4. Validation

- `mdec_replay_synthetic_test` pins the decoder's output fingerprint over
  6000 generated events covering every depth, flag and edge case above.
- `mdec_replay_ab` (configure with `-DMDEC_REF_COMMIT=<commit>`) runs two
  decoders side by side and reports the first difference: macroblock, pixel
  or snapshot field, plus the RLE involved.
- `PSX_MDEC_CAPTURE=<file>` records a game's MDEC commands.
  `mdec_replay --corpus <file> --check-live` proves that a decoder
  reproduces what was decoded live. Captures are game data: never commit
  them.
- A deliberate output change (fixing G1, G4 or G5) updates the pinned
  fingerprint in the same commit and says so.
