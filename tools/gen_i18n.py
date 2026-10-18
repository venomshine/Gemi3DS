# Gemi3DS - Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
# Software libre bajo la GNU GPL v3 con términos adicionales de atribución. Consulta LICENSE y NOTICE.md.
# Genera source/i18n_tab.h a partir de tools/i18n_tr.py y avisa de claves sin traducir
import os, sys, json, subprocess
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from i18n_tr import TR
def cstr(s):
    out = '"'
    for ch in s:
        o = ord(ch)
        if ch == '"': out += '\\"'
        elif ch == '\\': out += '\\\\'
        elif ch == '\n': out += '\\n'
        elif 0xE000 <= o <= 0xF8FF:
            out += ''.join('\\x%02X' % b for b in ch.encode('utf-8')) + '" "'
        else: out += ch
    return out + '"'
keys = json.loads(subprocess.run([sys.executable, os.path.join(here, 'i18n_keys.py')], capture_output=True, text=True).stdout)
miss = [k for k in keys if k not in TR]
for k in miss: print('SIN TRADUCIR:', repr(k), file=sys.stderr)
for k in TR:
    if k not in keys: print('sin uso:', repr(k), file=sys.stderr)
with open(os.path.join(here, '..', 'source', 'i18n_tab.h'), 'w') as f:
    f.write('/* Gemi3DS - Copyright (C) 2026 VENOMSHiNE - GPL-3.0 con términos adicionales (ver LICENSE y NOTICE.md) */\n// generado por tools/gen_i18n.py - no editar a mano\nstatic const TrEntry TR_TAB[] = {\n')
    for k, (en, ja) in TR.items():
        f.write('    {%s,\n     %s,\n     %s},\n' % (cstr(k), cstr(en), cstr(ja)))
    f.write('};\n')
print(len(TR), 'entradas;', len(miss), 'sin traducir')
