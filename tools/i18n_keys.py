# Extrae las claves de traducción (_("…") / tr("…")) tras el preprocesador
import re, subprocess, sys, os, glob, json
src = os.path.join(os.path.dirname(__file__), '..', 'source')
dkp = os.environ.get('DEVKITPRO', '/opt/devkitpro')
inc = ['-I' + src, '-I' + os.path.join(src, '..', 'lib'), '-I' + dkp + '/libctru/include', '-I' + dkp + '/portlibs/3ds/include',
       '-D__3DS__', '-DCJSON_NESTING_LIMIT=256']
keys = []
LIT = r'"(?:[^"\\]|\\.)*"'
for f in sorted(glob.glob(src + '/*.c')):
    out = subprocess.run(['arm-none-eabi-gcc', '-E', '-P'] + inc + [f], capture_output=True, text=True).stdout
    for m in re.finditer(r'\btr\s*\(\s*((?:' + LIT + r'\s*)+)\)', out):
        parts = re.findall(LIT, m.group(1))
        s = ''.join(p[1:-1] for p in parts)
        s = bytes(s, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')
        if s not in keys: keys.append(s)
# claves dinámicas usadas sin literal directo
for extra in ['Sistema', 'Nuevo chat', 'Cancelar', 'Aceptar', 'Ayúdame a escribir ', 'Explícame de forma sencilla ',
              'Planifica un viaje de 3 días a ', 'Dame ideas para ', 'Ayúdame a escribir', 'Explícame un tema',
              'Planifica un viaje', 'Dame ideas', 'Chat']:
    if extra not in keys: keys.append(extra)
json.dump(keys, sys.stdout, ensure_ascii=False, indent=0)
