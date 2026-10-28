#!/usr/bin/env python3
"""
Generate a tiny stories-class checkpoint and tokenizer for MyOS Phase 3.
Size: ~400 KB, fast to load and execute under QEMU TCG.
"""

import struct
import math
import sys
import os

def generate_checkpoint(out_ckpt="tools/stories.bin", out_tok="tools/tokenizer.bin"):
    dim = 64
    hidden_dim = 128
    n_layers = 2
    n_heads = 2
    n_kv_heads = 2
    vocab_size = 256
    seq_len = 128

    config = struct.pack("<7i", dim, hidden_dim, n_layers, n_heads, n_kv_heads, vocab_size, seq_len)

    # Deterministic pseudo-random float generator
    seed = 42
    def next_f32():
        nonlocal seed
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        return (seed / 0x7FFFFFFF - 0.5) * 0.1

    weights = bytearray()

    # token_embedding_table: (vocab_size, dim)
    for v in range(vocab_size):
        for d in range(dim):
            # Embeddings biased so tokens predict sequential letters or basic patterns
            val = next_f32() + (0.1 if (v + 1) % vocab_size == (d % vocab_size) else 0.0)
            weights.extend(struct.pack("<f", val))

    # rms_att_weight: (n_layers, dim)
    for _ in range(n_layers * dim):
        weights.extend(struct.pack("<f", 1.0))

    # wq, wk, wv, wo: n_layers * 4 * dim * dim
    for _ in range(n_layers * 4 * dim * dim):
        weights.extend(struct.pack("<f", next_f32()))

    # rms_ffn_weight: (n_layers, dim)
    for _ in range(n_layers * dim):
        weights.extend(struct.pack("<f", 1.0))

    # w1, w2, w3: n_layers * 3 * dim * hidden_dim
    for _ in range(n_layers * 3 * dim * hidden_dim):
        weights.extend(struct.pack("<f", next_f32()))

    # rms_final_weight: (dim,)
    for _ in range(dim):
        weights.extend(struct.pack("<f", 1.0))

    with open(out_ckpt, "wb") as f:
        f.write(config)
        f.write(weights)

    # Tokenizer: vocab_size tokens, each 1 byte
    with open(out_tok, "wb") as f:
        for i in range(vocab_size):
            char_byte = bytes([i])
            f.write(struct.pack("<B", 1))
            f.write(char_byte)

    print(f"Generated {out_ckpt} ({len(config) + len(weights)} bytes)")
    print(f"Generated {out_tok} ({vocab_size * 2} bytes)")

if __name__ == "__main__":
    ckpt = sys.argv[1] if len(sys.argv) > 1 else "tools/stories.bin"
    tok = sys.argv[2] if len(sys.argv) > 2 else "tools/tokenizer.bin"
    generate_checkpoint(ckpt, tok)

