#!/usr/bin/env python3
"""Flatten a `gnn_oracle` checkpoint to the little-endian fp32 blob the compiler
loads. The C reader in src/ir/ml_gnn.c must match the order emitted here.

    python export_oracle.py _bakeoff/oracle_C.pt oracle_C.bin
    python export_oracle.py _bakeoff/oracle_C.pt --manifest      # order only

The shipped `gnn_genius.bin` uses the v1 `MLGN` format, whose header carries only
(version, d, layers, nclass) because everything else was fixed at compile time.
The oracle variants differ in feature count, edge count, and whether they even
have a recurrent core, so v2 puts all of that in the header: a loader can then
reject a blob it does not understand instead of silently misreading it.

    magic   "MLGO"
    i32     version = 2
    i32     d_model
    i32     layers          (fixed-depth block count; 1 when PONDER)
    i32     n_classes
    i32     nfeat           9 or 45
    i32     nedge           8 or 12
    i32     flags           bit0 OBS, bit1 PTR, bit2 PONDER, bit3 AUX
    i32     max_steps       ACT cap (0 when not PONDER)
    f32[]   tensors, in the order `tensor_order` returns

Nothing about this file changes what the compiler loads today. `gnn_genius.bin`
stays as it is until a variant is ported to C, validated against the golden
vectors, and deliberately swapped in.
"""
import argparse
import os
import struct
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MAGIC = b"MLGO"
VERSION = 2

FLAG_OBS = 1
FLAG_PTR = 2
FLAG_PONDER = 4
FLAG_AUX = 8


def tensor_order(cfg):
    """The exact emission order. Every entry must exist in the state dict; a
    KeyError here means the architecture changed without the exporter."""
    layers = cfg["layers"]
    nedge = 12 if cfg.get("use_obs", True) else 8
    order = ["kind_emb.weight", "op_emb.weight", "feat_lin.weight", "feat_lin.bias"]

    if cfg.get("use_ponder"):
        # One shared block, applied recurrently, plus the GRU update, the
        # LayerNorm, and the halting projection.
        for t in range(nedge):
            order += [f"block.msg.{t}.weight", f"block.msg.{t}.bias"]
        order += ["block.selfw.weight", "block.selfw.bias",
                  "upd.weight_ih", "upd.weight_hh", "upd.bias_ih", "upd.bias_hh",
                  "norm.weight", "norm.bias",
                  "halt.weight", "halt.bias"]
    else:
        for li in range(layers):
            for t in range(nedge):
                order += [f"blocks.{li}.msg.{t}.weight", f"blocks.{li}.msg.{t}.bias"]
            order += [f"blocks.{li}.selfw.weight", f"blocks.{li}.selfw.bias",
                      f"norms.{li}.weight", f"norms.{li}.bias"]

    order += ["head.0.weight", "head.0.bias", "head.2.weight", "head.2.bias"]
    if cfg.get("use_ptr"):
        order += ["ptr_q.weight", "ptr_q.bias", "ptr_k.weight", "ptr_k.bias",
                  "ptr_none"]
    if cfg.get("aux"):
        order += ["aux_live.weight", "aux_live.bias",
                  "aux_risk.weight", "aux_risk.bias"]
    return order


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?", default=None)
    ap.add_argument("--manifest", action="store_true",
                    help="print the tensor order and shapes, write nothing")
    args = ap.parse_args()

    ck = torch.load(args.src, map_location="cpu", weights_only=False)
    sd = ck["model"]
    cfg = dict(ck["cfg"])
    d = cfg["d_model"]
    layers = 1 if cfg.get("use_ponder") else cfg["layers"]
    nclass = cfg["n_classes"]
    nfeat = 45 if cfg.get("use_obs", True) else 9
    nedge = 12 if cfg.get("use_obs", True) else 8
    flags = ((FLAG_OBS if cfg.get("use_obs") else 0) |
             (FLAG_PTR if cfg.get("use_ptr") else 0) |
             (FLAG_PONDER if cfg.get("use_ponder") else 0) |
             (FLAG_AUX if cfg.get("aux") else 0))
    max_steps = cfg.get("max_steps", 0) if cfg.get("use_ponder") else 0

    order = tensor_order(cfg)
    missing = [k for k in order if k not in sd]
    if missing:
        print(f"ERROR: checkpoint is missing {len(missing)} expected tensors, "
              f"first few: {missing[:5]}", file=sys.stderr)
        return 1
    extra = [k for k in sd if k not in order]
    if extra:
        print(f"WARNING: {len(extra)} tensors in the checkpoint are NOT "
              f"exported: {extra[:8]}", file=sys.stderr)

    if args.manifest:
        print(f"variant={ck.get('variant')} d={d} layers={layers} "
              f"nclass={nclass} nfeat={nfeat} nedge={nedge} flags={flags} "
              f"max_steps={max_steps}")
        tot = 0
        for k in order:
            n = sd[k].numel()
            tot += n
            print(f"  {k:32s} {tuple(sd[k].shape)!s:>18s} {n:>10d}")
        print(f"  {'TOTAL':32s} {'':>18s} {tot:>10d} floats "
              f"({tot*4/1e6:.1f} MB)")
        return 0

    dst = args.dst or os.path.splitext(args.src)[0] + ".bin"
    with open(dst, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<iiiiiiii", VERSION, d, layers, nclass, nfeat,
                            nedge, flags, max_steps))
        n = 0
        for k in order:
            t = sd[k].contiguous().to(torch.float32).cpu().numpy().ravel()
            f.write(t.tobytes())
            n += t.size
    print(f"wrote {dst}: variant={ck.get('variant')} d={d} layers={layers} "
          f"nclass={nclass} nfeat={nfeat} nedge={nedge} flags={flags} "
          f"floats={n} ({os.path.getsize(dst)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
