"""TinyNet - a tiny recurrent network library in numpy, and the Flubsound model file.

Layers (all causal, one frame at a time at inference):
  Dense   y = act (W x + b)                       W: (out, in)
  Conv1D  y_t = act (W [x_{t-k+1} .. x_t] + b)     W: (out, k * in), oldest frame first
  GRU     z = sigm (Wz x + bxz + Uz h + bhz), r = sigm (Wr x + bxr + Ur h + bhr)
          n = tanh (Wn x + bxn + r * (Un h + bhn)),  h' = (1 - z) * n + z * h
          W: (3H, in), U: (3H, H), rows in the order z, r, n  (the "reset after" GRU)
A layer reads the concatenation of earlier tensors (tensor 0 = the model
input, tensor i = the output of layer i - 1), so skip connections work.

The binary format (little-endian) is read by core/src/neural/TinyNet.cpp:
  header (64 bytes): "FLUBTNET", formatVersion, headerBytes, payloadBytes,
  payloadCrc32 (zlib.crc32), modelVersion, featureSet, sampleRate, frameSize,
  numFeatures, numLayers, numOutputs, parameterCount, flags, reserved
  payload: per layer type, activation, outSize, kernel, numInputs, input ids,
  weightFormat (0 float32, 1 int8 with one float32 scale per row), then the
  matrices (int8 rows padded to 4 bytes) and the float32 biases; then the
  output tensor ids; then the name (length + UTF-8, padded to 4 bytes).
"""
import struct
import zlib

import numpy as np

ACT_LINEAR, ACT_RELU, ACT_TANH, ACT_SIGMOID = 0, 1, 2, 3
LAYER_DENSE, LAYER_CONV1D, LAYER_GRU = 1, 2, 3
FORMAT_VERSION = 1
HEADER_BYTES = 64
MAGIC = b"FLUBTNET"


def act_fwd(a, z):
    if a == ACT_LINEAR:
        return z
    if a == ACT_RELU:
        return np.maximum(z, 0.0)
    if a == ACT_TANH:
        return np.tanh(z)
    if a == ACT_SIGMOID:
        return 1.0 / (1.0 + np.exp(-z))
    raise ValueError(a)


def act_grad(a, y):
    if a == ACT_LINEAR:
        return np.ones_like(y)
    if a == ACT_RELU:
        return (y > 0).astype(y.dtype)
    if a == ACT_TANH:
        return 1.0 - y * y
    if a == ACT_SIGMOID:
        return y * (1.0 - y)
    raise ValueError(a)


def sigmoid(z):
    return 1.0 / (1.0 + np.exp(-z))


class Layer:
    def __init__(self, kind, inputs, in_dim, out_dim, act=ACT_LINEAR, kernel=1, rng=None, dtype=np.float32):
        self.kind, self.inputs, self.in_dim, self.out_dim = kind, list(inputs), in_dim, out_dim
        self.act, self.kernel = act, kernel
        self.p = {}
        rng = rng or np.random.default_rng(0)

        def glorot(rows, cols):
            lim = np.sqrt(6.0 / (rows + cols))
            return rng.uniform(-lim, lim, (rows, cols)).astype(dtype)

        if kind == LAYER_DENSE:
            self.p["W"] = glorot(out_dim, in_dim)
            self.p["b"] = np.zeros(out_dim, dtype)
        elif kind == LAYER_CONV1D:
            self.p["W"] = glorot(out_dim, kernel * in_dim)
            self.p["b"] = np.zeros(out_dim, dtype)
        elif kind == LAYER_GRU:
            h = out_dim
            self.p["W"] = np.concatenate([glorot(h, in_dim) for _ in range(3)]).astype(dtype)
            u = []
            for _ in range(3):
                q, _ = np.linalg.qr(rng.standard_normal((h, h)))
                u.append(q)
            self.p["U"] = np.concatenate(u).astype(dtype)
            self.p["bx"] = np.zeros(3 * h, dtype)
            self.p["bh"] = np.zeros(3 * h, dtype)
        else:
            raise ValueError(kind)

    # ---- sequence forward / backward (B, T, features) -------------------------
    def forward(self, x):
        if self.kind == LAYER_DENSE:
            y = act_fwd(self.act, x @ self.p["W"].T + self.p["b"])
            self.cache = (x, y)
            return y
        if self.kind == LAYER_CONV1D:
            s = self._stack(x)
            y = act_fwd(self.act, s @ self.p["W"].T + self.p["b"])
            self.cache = (s, y)
            return y
        return self._gru_forward(x)

    def backward(self, dy):
        g = {}
        if self.kind in (LAYER_DENSE, LAYER_CONV1D):
            xin, y = self.cache
            dz = dy * act_grad(self.act, y)
            flat_z = dz.reshape(-1, dz.shape[-1])
            g["W"] = flat_z.T @ xin.reshape(-1, xin.shape[-1])
            g["b"] = flat_z.sum(axis=0)
            dx = dz @ self.p["W"]
            if self.kind == LAYER_CONV1D:
                dx = self._unstack(dx)
            return dx, g
        return self._gru_backward(dy)

    def _stack(self, x):
        b, t, f = x.shape
        k = self.kernel
        pad = np.concatenate([np.zeros((b, k - 1, f), x.dtype), x], axis=1)
        return np.concatenate([pad[:, j:j + t] for j in range(k)], axis=2)

    def _unstack(self, ds):
        b, t, _ = ds.shape
        f, k = self.in_dim, self.kernel
        dpad = np.zeros((b, t + k - 1, f), ds.dtype)
        for j in range(k):
            dpad[:, j:j + t] += ds[:, :, j * f:(j + 1) * f]
        return dpad[:, k - 1:]

    def _gru_forward(self, x):
        b, t, _ = x.shape
        h_dim = self.out_dim
        W, U, bx, bh = self.p["W"], self.p["U"], self.p["bx"], self.p["bh"]
        gx = x @ W.T + bx
        h = np.zeros((b, h_dim), x.dtype)
        hs = np.empty((b, t, h_dim), x.dtype)
        zs, rs, ns, ghn, hprev = (np.empty((b, t, h_dim), x.dtype) for _ in range(5))
        for i in range(t):
            gh = h @ U.T + bh
            z = sigmoid(gx[:, i, :h_dim] + gh[:, :h_dim])
            r = sigmoid(gx[:, i, h_dim:2 * h_dim] + gh[:, h_dim:2 * h_dim])
            n = np.tanh(gx[:, i, 2 * h_dim:] + r * gh[:, 2 * h_dim:])
            hprev[:, i] = h
            h = (1.0 - z) * n + z * h
            zs[:, i], rs[:, i], ns[:, i], ghn[:, i], hs[:, i] = z, r, n, gh[:, 2 * h_dim:], h
        self.cache = (x, zs, rs, ns, ghn, hprev)
        return hs

    def _gru_backward(self, dhs):
        x, zs, rs, ns, ghn, hprev = self.cache
        b, t, h_dim = dhs.shape
        U = self.p["U"]
        dgx = np.empty((b, t, 3 * h_dim), x.dtype)
        dU = np.zeros_like(U)
        dbh = np.zeros(3 * h_dim, x.dtype)
        dh_next = np.zeros((b, h_dim), x.dtype)
        for i in range(t - 1, -1, -1):
            dh = dhs[:, i] + dh_next
            z, r, n = zs[:, i], rs[:, i], ns[:, i]
            dz = dh * (hprev[:, i] - n)
            dn = dh * (1.0 - z)
            dan = dn * (1.0 - n * n)
            dar = dan * ghn[:, i] * r * (1.0 - r)
            daz = dz * z * (1.0 - z)
            dgh = np.concatenate([daz, dar, dan * r], axis=1)
            dgx[:, i] = np.concatenate([daz, dar, dan], axis=1)
            dU += dgh.T @ hprev[:, i]
            dbh += dgh.sum(axis=0)
            dh_next = dh * z + dgh @ U
        flat = dgx.reshape(-1, 3 * h_dim)
        g = {"W": flat.T @ x.reshape(-1, x.shape[-1]), "U": dU, "bx": flat.sum(axis=0), "bh": dbh}
        return dgx @ self.p["W"], g

    def param_count(self):
        return int(sum(v.size for v in self.p.values()))


class Net:
    """A layer graph: tensor 0 is the input, tensor i + 1 is layer i's output."""

    def __init__(self, num_inputs, specs, outputs, seed=0, dtype=np.float32):
        rng = np.random.default_rng(seed)
        self.num_inputs = num_inputs
        self.sizes = [num_inputs]
        self.layers = []
        for spec in specs:
            kind, inputs, out_dim = spec["kind"], spec["inputs"], spec["out"]
            in_dim = sum(self.sizes[i] for i in inputs)
            layer = Layer(kind, inputs, in_dim, out_dim, spec.get("act", ACT_LINEAR), spec.get("kernel", 1), rng, dtype)
            self.layers.append(layer)
            self.sizes.append(out_dim)
        self.outputs = list(outputs)

    def forward(self, x):
        tensors = [x]
        for layer in self.layers:
            xin = tensors[layer.inputs[0]] if len(layer.inputs) == 1 else np.concatenate([tensors[i] for i in layer.inputs], axis=-1)
            tensors.append(layer.forward(xin))
        self.tensors = tensors
        return [tensors[i] for i in self.outputs]

    def backward(self, d_outputs):
        grads = [None] * len(self.tensors)
        for tid, d in zip(self.outputs, d_outputs):
            grads[tid] = d if grads[tid] is None else grads[tid] + d
        pgrads = [None] * len(self.layers)
        for li in range(len(self.layers) - 1, -1, -1):
            layer = self.layers[li]
            dy = grads[li + 1]
            if dy is None:
                dy = np.zeros_like(self.tensors[li + 1])
            dx, pgrads[li] = layer.backward(dy)
            off = 0
            for i in layer.inputs:
                part = dx[..., off:off + self.sizes[i]]
                off += self.sizes[i]
                if i == 0:
                    continue
                grads[i] = part if grads[i] is None else grads[i] + part
        return pgrads

    def params(self):
        return [(li, k, layer.p[k]) for li, layer in enumerate(self.layers) for k in layer.p]

    def param_count(self):
        return sum(layer.param_count() for layer in self.layers)

    def state(self):
        return {f"{li}.{k}": v.copy() for li, k, v in self.params()}

    def load_state(self, st):
        for li, k, _ in self.params():
            self.layers[li].p[k] = st[f"{li}.{k}"].astype(self.layers[li].p[k].dtype).copy()


class Adam:
    def __init__(self, net, lr=1e-3, b1=0.9, b2=0.999, eps=1e-8, clip=5.0):
        self.net, self.lr, self.b1, self.b2, self.eps, self.clip = net, lr, b1, b2, eps, clip
        self.m = {(li, k): np.zeros_like(v) for li, k, v in net.params()}
        self.v = {(li, k): np.zeros_like(v) for li, k, v in net.params()}
        self.t = 0

    def step(self, pgrads, lr=None):
        lr = self.lr if lr is None else lr
        total = np.sqrt(sum(float(np.sum(g * g)) for gl in pgrads for g in gl.values()))
        scale = min(1.0, self.clip / (total + 1e-12))
        self.t += 1
        for li, gl in enumerate(pgrads):
            for k, g in gl.items():
                g = g * scale
                m = self.m[(li, k)] = self.b1 * self.m[(li, k)] + (1 - self.b1) * g
                v = self.v[(li, k)] = self.b2 * self.v[(li, k)] + (1 - self.b2) * g * g
                mh = m / (1 - self.b1 ** self.t)
                vh = v / (1 - self.b2 ** self.t)
                self.net.layers[li].p[k] -= (lr * mh / (np.sqrt(vh) + self.eps)).astype(self.net.layers[li].p[k].dtype)
        return total


# ---- quantisation ------------------------------------------------------------------
def quantise_rows(m):
    """int8 per-row: scale = max|row| / 127 (1 for an all-zero row)."""
    m = np.asarray(m, dtype=np.float64)
    amax = np.max(np.abs(m), axis=1)
    scale = np.where(amax > 0, amax / 127.0, 1.0).astype(np.float32)
    q = np.clip(np.round(m / scale[:, None].astype(np.float64)), -127, 127).astype(np.int8)
    return scale, q


def dequantise_rows(scale, q):
    return q.astype(np.float32) * scale[:, None]


def matrices_of(layer):
    if layer.kind == LAYER_GRU:
        return ["W", "U"], ["bx", "bh"]
    return ["W"], ["b"]


def quantised_copy(net, int8=True):
    """Weights as the C++ runtime will hold them (float32; int8 rows dequantised)."""
    st = {}
    for li, layer in enumerate(net.layers):
        mats, biases = matrices_of(layer)
        for k in mats:
            w = layer.p[k].astype(np.float32)
            st[f"{li}.{k}"] = dequantise_rows(*quantise_rows(w)) if int8 else w
        for k in biases:
            st[f"{li}.{k}"] = layer.p[k].astype(np.float32)
    return st


# ---- file I/O ----------------------------------------------------------------------
def _pad4(b):
    return b + b"\x00" * ((-len(b)) % 4)


def write_model(net, path, *, model_version, feature_set, sample_rate, frame_size, name, int8=True):
    payload = bytearray()
    params = 0
    for layer in net.layers:
        payload += struct.pack("<5I", layer.kind, layer.act, layer.out_dim, layer.kernel, len(layer.inputs))
        payload += struct.pack(f"<{len(layer.inputs)}I", *layer.inputs)
        payload += struct.pack("<I", 1 if int8 else 0)
        mats, biases = matrices_of(layer)
        for k in mats:
            w = layer.p[k].astype(np.float32)
            params += w.size
            if int8:
                scale, q = quantise_rows(w)
                payload += scale.astype("<f4").tobytes()
                payload += _pad4(q.tobytes())
            else:
                payload += w.astype("<f4").tobytes()
        for k in biases:
            payload += layer.p[k].astype("<f4").tobytes()
            params += layer.p[k].size
    payload += struct.pack(f"<{len(net.outputs)}I", *net.outputs)
    nb = name.encode("utf-8")
    payload += struct.pack("<I", len(nb)) + _pad4(nb)
    payload = bytes(payload)
    header = MAGIC + struct.pack("<14I", FORMAT_VERSION, HEADER_BYTES, len(payload), zlib.crc32(payload) & 0xFFFFFFFF,
                                 model_version, feature_set, sample_rate, frame_size, net.num_inputs,
                                 len(net.layers), len(net.outputs), params, 0, 0)
    assert len(header) == HEADER_BYTES
    data = header + payload
    with open(path, "wb") as f:
        f.write(data)
    return data


class StreamingNet:
    """Frame-by-frame inference in float32 with the C++ runtime's arithmetic order
    (only the summation order of the dot products differs). Used to export the
    reference outputs the C++ tests compare against."""

    def __init__(self, net, state):
        self.net = net
        self.w = {k: v.astype(np.float32) for k, v in state.items()}
        self.reset()

    def reset(self):
        self.hist = {}
        self.h = {}
        for li, layer in enumerate(self.net.layers):
            if layer.kind == LAYER_CONV1D:
                self.hist[li] = np.zeros((layer.kernel - 1, layer.in_dim), np.float32)
            if layer.kind == LAYER_GRU:
                self.h[li] = np.zeros(layer.out_dim, np.float32)

    def step(self, x):
        tensors = [np.asarray(x, np.float32)]
        for li, layer in enumerate(self.net.layers):
            xin = np.concatenate([tensors[i] for i in layer.inputs]).astype(np.float32)
            if layer.kind == LAYER_DENSE:
                y = act_fwd(layer.act, (self.w[f"{li}.W"].astype(np.float64) @ xin + self.w[f"{li}.b"])).astype(np.float32)
            elif layer.kind == LAYER_CONV1D:
                stacked = np.concatenate([self.hist[li].reshape(-1), xin]).astype(np.float32)
                if layer.kernel > 1:
                    self.hist[li] = np.vstack([self.hist[li][1:], xin[None, :]])
                y = act_fwd(layer.act, (self.w[f"{li}.W"].astype(np.float64) @ stacked + self.w[f"{li}.b"])).astype(np.float32)
            else:
                hd = layer.out_dim
                h = self.h[li]
                gx = self.w[f"{li}.W"].astype(np.float64) @ xin + self.w[f"{li}.bx"]
                gh = self.w[f"{li}.U"].astype(np.float64) @ h + self.w[f"{li}.bh"]
                z = sigmoid(gx[:hd] + gh[:hd])
                r = sigmoid(gx[hd:2 * hd] + gh[hd:2 * hd])
                n = np.tanh(gx[2 * hd:] + r * gh[2 * hd:])
                h = ((1.0 - z) * n + z * h).astype(np.float32)
                self.h[li] = h
                y = h
            tensors.append(y.astype(np.float32))
        return [tensors[i] for i in self.net.outputs]
