// parser.h — hand-written ITCH 5.0 decode (Rung 1, spec §5)
//
// YOU write this. Notes from the spec:
//   - Framing: [u16 BE length N][N-byte payload]; 11-byte common header
//     (type@0, locate@1 u16, tracking@3, ts@5 48-bit ns). Same as Rung 0.
//   - Big-endian decode via __builtin_bswap16/32/64.
//   - Handle A, F, E, C, X, D, U. S/R parsed, book-ignored.
//   - U = D(old ref) + A(new ref); side & locate INHERITED from original order.
//   - Prices as raw ints end-to-end (no float) — required for the bit-exact diff.
//   - Offsets already validated in Rung 0 (oracle/reconstruct.cpp) — reuse them.

#pragma once
