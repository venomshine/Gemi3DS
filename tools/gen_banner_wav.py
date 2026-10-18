# Gemi3DS - Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
# Software libre bajo la GNU GPL v3 con términos adicionales de atribución. Consulta LICENSE y NOTICE.md.
# Genera cia/banner.wav: el sonido del banner en el menú HOME (un "destello" de campanitas)
# Estéreo, PCM 16 bits a 32728 Hz (la frecuencia del DSP de la 3DS, sin remuestreo), menos de 3 s.
# Uso: python3 tools/gen_banner_wav.py
import math, os, random, struct, wave

SR = 32728
DUR = 2.2
N = int(SR * DUR)
random.seed(3)

L = [0.0] * N
R = [0.0] * N

def bell(t0, freq, amp, pan, decay=1.6, bright=1.0):
    """campana FM (portadora 1:1, moduladora 1:3.5): suena a cristal, no a tono puro"""
    i0 = int(t0 * SR)
    gl, gr = math.cos(pan * math.pi / 2), math.sin(pan * math.pi / 2)
    for i in range(i0, N):
        t = (i - i0) / SR
        env = (1 - math.exp(-t * 900)) * math.exp(-t * (3.2 / decay))
        if env < 1e-4 and t > 0.05: break
        idx = bright * 2.2 * math.exp(-t * 6)          # el brillo cae antes que el volumen
        mod = math.sin(2 * math.pi * freq * 3.5 * t)
        s = math.sin(2 * math.pi * freq * t + idx * mod)
        s += 0.18 * math.sin(2 * math.pi * freq * 2.76 * t) * math.exp(-t * 9)   # parcial inarmónico
        v = s * env * amp
        L[i] += v * gl; R[i] += v * gr

def sparkle(t0, n, spread, amp):
    """polvo de estrellas: notas muy agudas y cortas repartidas en estéreo"""
    scale = [2093.0, 2349.3, 2637.0, 3136.0, 3520.0, 4186.0]   # do6 re6 mi6 sol6 la6 do7
    for k in range(n):
        t = t0 + k * spread / n + random.uniform(0, 0.02)
        bell(t, random.choice(scale), amp * random.uniform(0.5, 1.0), random.uniform(0.1, 0.9), decay=0.35, bright=0.6)

# acorde ascendente sol5 · re6 · sol6 (quinta + octava: abierto y luminoso) y destellos encima
bell(0.00, 783.99, 0.34, 0.40, decay=1.9)
bell(0.09, 1174.66, 0.30, 0.60, decay=1.7)
bell(0.18, 1567.98, 0.26, 0.50, decay=1.6)
bell(0.18, 392.00, 0.16, 0.50, decay=2.2, bright=0.5)   # base suave para que no suene "chillón"
sparkle(0.24, 9, 0.55, 0.10)

# reverberación ligera (dos ecos cruzados entre canales)
for d, g in ((int(0.061 * SR), 0.22), (int(0.113 * SR), 0.14)):
    for i in range(N - 1, d - 1, -1):
        L[i] += R[i - d] * g
        R[i] += L[i - d] * g

# fundido final y normalización a -2 dBFS
fade = int(0.25 * SR)
for i in range(N - fade, N):
    f = (N - i) / fade
    L[i] *= f; R[i] *= f
peak = max(max(abs(x) for x in L), max(abs(x) for x in R))
gain = 10 ** (-2 / 20) * 32767 / peak

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'cia', 'banner.wav')
with wave.open(out, 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes(b''.join(struct.pack('<hh', int(l * gain), int(r * gain)) for l, r in zip(L, R)))
print('escrito', os.path.normpath(out), '%.2f s' % DUR)
