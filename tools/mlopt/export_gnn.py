#!/usr/bin/env python3
# Flatten gnn_genius.pt to a little-endian fp32 blob (header MLGN + dims, then
# tensors in `order` below). The C reader in src/ir/ml_gnn.c must match this order.
import os
import struct
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "gnn_genius.pt")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "gnn_genius.bin")
    ck = torch.load(src, map_location="cpu")
    sd = ck["model"]
    d = ck["cfg"]["d_model"]
    layers = ck["cfg"]["layers"]
    nclass = ck["cfg"]["n_classes"]

    order = ["kind_emb.weight", "op_emb.weight", "feat_lin.weight", "feat_lin.bias"]
    for li in range(layers):
        for t in range(8):
            order += [f"msg.{li}.{t}.weight", f"msg.{li}.{t}.bias"]
        order += [f"selfw.{li}.weight", f"selfw.{li}.bias",
                  f"norm.{li}.weight", f"norm.{li}.bias"]
    order += ["head.0.weight", "head.0.bias", "head.2.weight", "head.2.bias"]

    with open(dst, "wb") as f:
        f.write(b"MLGN")
        f.write(struct.pack("<iiii", 1, d, layers, nclass))
        n = 0
        for k in order:
            t = sd[k].contiguous().to(torch.float32).cpu().numpy().ravel()
            f.write(t.tobytes())
            n += t.size
    print(f"wrote {dst}: d={d} layers={layers} nclass={nclass} floats={n} "
          f"({os.path.getsize(dst)} bytes)")

if __name__ == "__main__":
    main()
