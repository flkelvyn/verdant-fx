#!/usr/bin/env python3
"""
Verdant FX - protótipo de referência do algoritmo de nitidez adaptativa.

Espelha, linha a linha, shaders/vegetation_sharpen.hlsl. Serve para validar o
ALGORITMO em imagens sintéticas (halo, ruído, gradientes, ganho em vegetação).
NÃO substitui testes visuais no ETS2: nenhuma captura do jogo foi usada.

Uso: python3 tests/prototype_sharpen.py
"""
import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722])


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def shift(img, dy, dx):
    """Amostra img[y+dy, x+dx] com clamp nas bordas (equivale ao Load clampado)."""
    h, w = img.shape[:2]
    ys = np.clip(np.arange(h) + dy, 0, h - 1)
    xs = np.clip(np.arange(w) + dx, 0, w - 1)
    return img[ys][:, xs]


def sharpen(img, intensity=0.45, halo=0.60, edge=0.50, noise=0.40,
            veg_focus=0.65, taps=9, srgb_view=False):
    img = img.astype(np.float64)
    c = np.clip(img, 0, 1) ** (1 / 2.2) if srgb_view else img.copy()

    def L(a):
        return a @ LUMA

    cc = c
    n, s = shift(c, -1, 0), shift(c, 1, 0)
    w, e = shift(c, 0, -1), shift(c, 0, 1)
    ln, ls, lw, le, lc = L(n), L(s), L(w), L(e), L(cc)

    if taps >= 9:
        nw, ne = shift(c, -1, -1), shift(c, -1, 1)
        sw, se = shift(c, 1, -1), shift(c, 1, 1)
        lnw, lne, lsw, lse = L(nw), L(ne), L(sw), L(se)
        blur = (4 * lc + 2 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 16.0
        mn = np.minimum.reduce([lc, ln, ls, lw, le, lnw, lne, lsw, lse])
        mx = np.maximum.reduce([lc, ln, ls, lw, le, lnw, lne, lsw, lse])
    else:
        blur = (2 * lc + ln + ls + lw + le) / 6.0
        mn = np.minimum.reduce([lc, ln, ls, lw, le])
        mx = np.maximum.reduce([lc, ln, ls, lw, le])

    rng = mx - mn
    detail = lc - blur

    # controle de ruído (coring suave)
    nf = noise * 0.006
    detail = np.sign(detail) * np.maximum(np.abs(detail) - nf, 0.0)

    # preservação de bordas fortes
    edge_att = 1.0 - edge * smoothstep(0.10, 0.35, rng)

    # proteção de sombras e realces
    tone = smoothstep(0.02, 0.10, lc) * (1.0 - smoothstep(0.90, 0.99, lc))

    # heurística de vegetação: dominância do verde
    gdom = cc[..., 1] - np.maximum(cc[..., 0], cc[..., 2])
    veg = smoothstep(0.01, 0.08, gdom)
    weight = (1.0 - veg_focus) + veg_focus * veg

    amount = intensity * 1.5
    delta = detail * amount * weight * edge_att * tone

    # supressão de halo: limita overshoot ao intervalo local
    slack = (1.0 - halo) * rng * 0.5
    target = np.clip(lc + delta, mn - slack, mx + slack)
    delta = target - lc

    out = np.clip(cc + delta[..., None], 0.0, 1.0)
    if srgb_view:
        out = out ** 2.2
    return out


# ---------------------------------------------------------------- utilidades
def gauss_blur(img, sigma):
    r = int(3 * sigma) + 1
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = img.astype(np.float64)
    for axis in (0, 1):
        pad = [(0, 0)] * out.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="edge")
        acc = np.zeros_like(out)
        for i, kv in enumerate(k):
            sl = [slice(None)] * out.ndim
            sl[axis] = slice(i, i + out.shape[axis])
            acc += kv * p[tuple(sl)]
        out = acc
    return out


def rgb(gray):
    return np.repeat(gray[..., None], 3, axis=2)


def local_std(gray, k=7):
    m = gauss_blur(gray, k / 3)
    m2 = gauss_blur(gray * gray, k / 3)
    return np.sqrt(np.maximum(m2 - m * m, 0)).mean()


# --------------------------------------------------------------------- testes
def test_step_edge_overshoot(sigma=0.4, **kw):
    h, w = 64, 128
    x = np.zeros((h, w))
    x[:, w // 2:] = 0.8
    x[:, : w // 2] = 0.2
    img = rgb(gauss_blur(x, sigma))
    out = sharpen(img, **kw)
    over = max(out.max() - 0.8, 0.2 - out.min(), 0.0)
    return over


def test_noise_amplification():
    rng = np.random.default_rng(1)
    base = np.full((128, 128), 0.5)
    img = rgb(np.clip(base + rng.normal(0, 0.01, base.shape), 0, 1))
    out = sharpen(img)
    return out[..., 0].std() / img[..., 0].std()


def test_gradient_stability():
    ramp = np.tile(np.linspace(0.3, 0.7, 256), (64, 1))
    q = np.round(ramp * 255) / 255.0  # quantização 8 bits (banding do céu)
    img = rgb(q)
    out = sharpen(img)
    return np.abs(out - img).max() * 255.0  # desvio máximo em níveis de 8 bits


def foliage_texture(seed=3):
    rng = np.random.default_rng(seed)
    t = gauss_blur(rng.random((192, 192)), 1.2)  # alta frequência levemente borrada
    t = (t - t.min()) / (t.max() - t.min())
    return t


def test_vegetation_gain():
    t = foliage_texture()
    green = np.stack([0.10 + 0.10 * t, 0.25 + 0.30 * t, 0.06 + 0.06 * t], axis=2)
    gray = rgb(0.25 + 0.30 * t)
    gain_green = local_std(sharpen(green)[..., 1], 7) / local_std(green[..., 1], 7)
    gain_gray = local_std(sharpen(gray)[..., 1], 7) / local_std(gray[..., 1], 7)
    return gain_green, gain_gray


def test_range_and_nan():
    rng = np.random.default_rng(7)
    img = rng.random((96, 96, 3))
    for kw in (dict(), dict(taps=5), dict(srgb_view=True), dict(intensity=1.0, halo=0.0)):
        out = sharpen(img, **kw)
        if not np.isfinite(out).all() or out.min() < 0 or out.max() > 1:
            return False
    return True


def test_hue_preservation():
    t = foliage_texture(5)
    green = np.stack([0.10 + 0.10 * t, 0.25 + 0.30 * t, 0.06 + 0.06 * t], axis=2)
    out = sharpen(green)
    d = np.abs((out[..., 1] - out[..., 0]) - (green[..., 1] - green[..., 0]))
    return d.max()


if __name__ == "__main__":
    print("Verdant FX — validação algorítmica (imagens sintéticas, NÃO é teste no jogo)")
    o = test_step_edge_overshoot()
    o_max = test_step_edge_overshoot(intensity=1.0)
    o_nohalo = test_step_edge_overshoot(intensity=1.0, halo=0.0, edge=0.0, noise=0.0)
    print(f"[1] overshoot em borda (halo) — padrão: {o:.4f} | intensidade 1.0: {o_max:.4f} | "
          f"sem proteções (referência): {o_nohalo:.4f}  (limite adotado p/ intensidade 1.0: 0.050)")
    a = test_noise_amplification()
    print(f"[2] amplificação de ruído (std saída/entrada): {a:.3f}  (limite adotado: 1.50)")
    g = test_gradient_stability()
    print(f"[3] desvio máx. em gradiente 8 bits: {g:.2f} níveis  (limite adotado: 2.0)")
    gv, gn = test_vegetation_gain()
    print(f"[4] ganho de contraste local — verde: {gv:.3f} | cinza: {gn:.3f}  (esperado: verde > cinza > 1.0)")
    ok = test_range_and_nan()
    print(f"[5] saída finita e em [0,1] em todas as variantes: {ok}")
    hp = test_hue_preservation()
    print(f"[6] variação máx. de (G-R) após sharpen: {hp:.4f}  (preservação de matiz; limite 0.010)")

    fails = []
    if o_max > 0.05: fails.append("halo")
    if a > 1.5: fails.append("ruído")
    if g > 2.0: fails.append("gradiente")
    if not (gv > gn > 1.0): fails.append("ganho veg/cinza")
    if not ok: fails.append("faixa/NaN")
    if hp > 0.010: fails.append("matiz")
    print("RESULTADO:", "OK" if not fails else "FALHAS: " + ", ".join(fails))
    raise SystemExit(1 if fails else 0)
