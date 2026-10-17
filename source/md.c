// Gemi3DS - maquetador de Markdown ligero
#include "md.h"
#include "gfx.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define LINE_GAP 2.0f

// signos que no deben empezar una línea en japonés/chino (。、」 etc.)
static bool cjk_no_start(const char *p) {
    u32 c = utf8_next(&p);
    return c == 0x3001 || c == 0x3002 || c == 0xFF0C || c == 0xFF0E || c == 0xFF01 || c == 0xFF1F || c == 0xFF09 ||
           c == 0x300D || c == 0x300F || c == 0x3011 || c == 0x30FC || c == 0x30FB || c == 0xFF1A || c == 0xFF1B ||
           (c >= 0x3041 && c <= 0x3049 && (c & 1)) || c == 0x3063 || c == 0x30C3 || c == 0x3083 || c == 0x3085 || c == 0x3087 ||
           c == 0x30E3 || c == 0x30E5 || c == 0x30E7 || c == 0x2026 || c == '.' || c == ',' || c == '!' || c == '?' || c == ')';
}

static int arena_add(MdLayout *L, const char *s, int n) {
    if (L->alen + n + 1 > L->acap) {
        int nc = L->acap ? L->acap * 2 : 1024;
        while (nc < L->alen + n + 1) nc *= 2;
        L->arena = realloc(L->arena, nc);
        L->acap = nc;
    }
    int off = L->alen;
    memcpy(L->arena + off, s, n);
    L->alen += n;
    L->arena[L->alen] = 0;
    return off;
}

static MdSeg *seg_new(MdLayout *L) {
    if (L->nseg >= L->cseg) {
        L->cseg = L->cseg ? L->cseg * 2 : 128;
        L->segs = realloc(L->segs, L->cseg * sizeof(MdSeg));
    }
    MdSeg *s = &L->segs[L->nseg++];
    memset(s, 0, sizeof(*s));
    return s;
}

static MdLine *line_new(MdLayout *L) {
    if (L->nline >= L->cline) {
        L->cline = L->cline ? L->cline * 2 : 64;
        L->lines = realloc(L->lines, L->cline * sizeof(MdLine));
    }
    MdLine *l = &L->lines[L->nline++];
    memset(l, 0, sizeof(*l));
    l->seg0 = L->nseg;
    l->marker_off = -1;
    return l;
}

void md_free(MdLayout *L) {
    free(L->arena); free(L->segs); free(L->lines);
    memset(L, 0, sizeof(*L));
}

// ---------------------------------------------------------------- inline
typedef struct { char *txt; int len, cap; u8 flags; } Run;
typedef struct { Run r[96]; int n; } Runs;

static void runs_push(Runs *R, u8 flags, const char *s, int n) {
    if (n <= 0) return;
    Run *last = R->n ? &R->r[R->n - 1] : NULL;
    if (!last || last->flags != flags) {
        if (R->n >= 96) { last = &R->r[95]; }
        else { last = &R->r[R->n++]; last->txt = NULL; last->len = last->cap = 0; last->flags = flags; }
    }
    if (last->len + n + 1 > last->cap) {
        last->cap = (last->len + n + 1) * 2;
        last->txt = realloc(last->txt, last->cap);
    }
    memcpy(last->txt + last->len, s, n);
    last->len += n;
    last->txt[last->len] = 0;
}

static void runs_free(Runs *R) { for (int i = 0; i < R->n; i++) free(R->r[i].txt); R->n = 0; }

static void parse_inline(Runs *R, const char *s, int n, u8 base, bool md) {
    u8 f = base;
    int i = 0, st = 0;
    if (!md) { runs_push(R, f, s, n); return; }
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n && strchr("*_`[]#\\", s[i + 1])) {
            runs_push(R, f, s + st, i - st);
            runs_push(R, f, s + i + 1, 1);
            i += 2; st = i; continue;
        }
        if (c == '`') {
            runs_push(R, f, s + st, i - st);
            f ^= MS_CODE; i++; st = i; continue;
        }
        if (f & MS_CODE) { i++; continue; }
        if (c == '*' && i + 1 < n && s[i + 1] == '*') {
            runs_push(R, f, s + st, i - st);
            f ^= MS_BOLD; i += 2; st = i; continue;
        }
        if (c == '_' && i + 1 < n && s[i + 1] == '_') {
            runs_push(R, f, s + st, i - st);
            f ^= MS_BOLD; i += 2; st = i; continue;
        }
        if (c == '*') {
            bool open_ok = i + 1 < n && s[i + 1] != ' ';
            bool close_ok = i > 0 && s[i - 1] != ' ';
            if ((!(f & MS_ITALIC) && open_ok) || ((f & MS_ITALIC) && close_ok)) {
                runs_push(R, f, s + st, i - st);
                f ^= MS_ITALIC; i++; st = i; continue;
            }
        }
        if (c == '[') {
            // [texto](url)
            int j = i + 1;
            while (j < n && s[j] != ']') j++;
            if (j + 1 < n && s[j + 1] == '(') {
                int k = j + 2;
                while (k < n && s[k] != ')') k++;
                if (k < n) {
                    runs_push(R, f, s + st, i - st);
                    runs_push(R, f | MS_LINK, s + i + 1, j - i - 1);
                    i = k + 1; st = i; continue;
                }
            }
        }
        i++;
    }
    runs_push(R, f, s + st, n - st);
}

// ---------------------------------------------------------------- wrap
static float seg_gw(u32 cp, float scale, u8 flags) {
    return glyph_w(cp, scale);
}

// envuelve las corridas en líneas a partir de x0 con ancho w
static void wrap_runs(MdLayout *L, Runs *R, float scale, float indent, float width, u8 kind, MdLine *first) {
    MdLine *ln = first;
    float x = indent;
    float lh = text_lineh(scale);
    ln->h = lh + LINE_GAP;
    ln->scale = scale; ln->indent = indent; ln->kind = kind;
    for (int r = 0; r < R->n; r++) {
        Run *run = &R->r[r];
        const char *p = run->txt, *e = run->txt + run->len;
        while (p < e) {
            // palabra = hasta el próximo espacio (incluido)
            const char *w0 = p, *q = p;
            float ww = 0;
            bool code = run->flags & MS_CODE || kind == LK_CODE;
            while (q < e) {
                const char *nq = q;
                u32 cp = utf8_next(&nq);
                ww += seg_gw(cp, scale, run->flags);
                q = nq;
                if (cp == ' ' && !(kind == LK_CODE)) break;
                if (cp >= 0x2E80 && !code && !(q < e && cjk_no_start(q))) break;   // chino/japonés: corte tras cada carácter
                if (code && kind == LK_CODE && ww > width) break;
            }
            if (x + ww > width && x > indent + 0.5f) {
                // nueva línea
                ln->nseg = L->nseg - ln->seg0;
                MdLine *nl = line_new(L);
                nl->h = lh + LINE_GAP; nl->scale = scale; nl->indent = indent; nl->kind = kind;
                ln = nl; x = indent;
                if (*w0 == ' ') { w0++; ww -= glyph_w(' ', scale); p = w0; if (p >= q) continue; }
            }
            if (x + ww > width && x <= indent + 0.5f) {
                // palabra demasiado larga: cortar por caracteres
                q = w0; ww = 0;
                while (q < e) {
                    const char *nq = q;
                    u32 cp = utf8_next(&nq);
                    float gw = seg_gw(cp, scale, run->flags);
                    if (x + ww + gw > width && q > w0) break;
                    ww += gw; q = nq;
                }
            }
            // agrega o extiende segmento
            MdSeg *last = (L->nseg > ln->seg0) ? &L->segs[L->nseg - 1] : NULL;
            if (last && last->flags == run->flags && last->off + last->len == L->alen) {
                arena_add(L, w0, (int)(q - w0));
                last->len += (int)(q - w0);
                last->w += ww;
            } else {
                MdSeg *sg = seg_new(L);
                sg->off = arena_add(L, w0, (int)(q - w0));
                sg->len = (int)(q - w0);
                sg->x = x; sg->w = ww; sg->flags = run->flags;
            }
            x += ww;
            p = q;
        }
    }
    ln->nseg = L->nseg - ln->seg0;
}

// ---------------------------------------------------------------- bloques
static bool is_hr(const char *s, int n) {
    int cnt = 0; char ch = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == ' ') continue;
        if (s[i] != '-' && s[i] != '*' && s[i] != '_') return false;
        if (ch && s[i] != ch) return false;
        ch = s[i]; cnt++;
    }
    return cnt >= 3;
}

static bool is_table_sep(const char *s, int n) {
    bool dash = false;
    for (int i = 0; i < n; i++) {
        if (s[i] == '-') dash = true;
        else if (!strchr("|: \t", s[i])) return false;
    }
    return dash;
}

// quita emojis y selectores que la fuente de la 3DS no tiene
static char *filter_text(const char *in) {
    size_t n = strlen(in);
    char *out = malloc(n + 1);
    const char *p = in;
    char *o = out;
    while (*p) {
        // etiquetas internas de memoria: no se muestran
        if (p[0] == '[' && p[1] == '[' && (!strncmp(p, "[[RECORDAR", 10) || !strncmp(p, "[[OLVIDAR", 9) ||
                                           !strncmp(p, "[[RECORDA", strlen(p) < 9 ? strlen(p) : 9))) {
            const char *e = strstr(p, "]]");
            if (!e) break;
            p = e + 2;
            continue;
        }
        const char *q = p;
        u32 cp = utf8_next(&q);
        bool skip = (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
                    cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||
                    (cp >= 0xE0020 && cp <= 0xE007F) || cp == 0x2B50 || cp == 0x2B55 || cp == 0x2705;
        if (!skip) { memcpy(o, p, q - p); o += q - p; }
        else if (o > out && o[-1] == ' ' && *q == ' ') q++;   // evitar dobles espacios
        p = q;
    }
    *o = 0;
    return out;
}

void md_layout(MdLayout *L, const char *text_in, float width, float scale, bool markdown) {
    char *text_buf = filter_text(text_in);
    const char *text = text_buf;
    L->alen = 0; L->nseg = 0; L->nline = 0; L->height = 0; L->width = width; L->maxw = 0;
    if (!L->arena) arena_add(L, "", 0);
    float y = 0;
    bool in_code = false;
    bool table_header = false;
    const char *p = text;
    float lh = text_lineh(scale);
    int code_start_line = -1;

    while (*p) {
        const char *eol = strchr(p, '\n');
        int n = eol ? (int)(eol - p) : (int)strlen(p);
        const char *s = p;
        p = eol ? eol + 1 : p + n;
        if (n > 0 && s[n - 1] == '\r') n--;

        // contar sangría
        int sp = 0;
        while (sp < n && (s[sp] == ' ' || s[sp] == '\t')) sp += 1;
        const char *t = s + sp; int tn = n - sp;

        if (markdown && tn >= 3 && !strncmp(t, "```", 3)) {
            if (!in_code) { in_code = true; y += 3; code_start_line = L->nline; }
            else {
                in_code = false;
                if (code_start_line >= 0 && code_start_line < L->nline) {
                    L->lines[code_start_line].code_first = 1;
                    L->lines[L->nline - 1].code_last = 1;
                }
                y += 5;
            }
            continue;
        }
        Runs R = {0};
        if (in_code) {
            MdLine *ln = line_new(L);
            ln->y = y;
            runs_push(&R, MS_CODE, s, n);
            if (n == 0) runs_push(&R, MS_CODE, " ", 1);
            float cs = scale * 0.92f;
            wrap_runs(L, &R, cs, 6, width - 6, LK_CODE, ln);
            for (int i = (int)(ln - L->lines); i < L->nline; i++) { L->lines[i].y = y; y += L->lines[i].h; }
            runs_free(&R);
            continue;
        }
        if (tn == 0) { y += lh * 0.45f; table_header = false; continue; }

        if (markdown && is_hr(t, tn)) {
            MdLine *ln = line_new(L);
            ln->kind = LK_HR; ln->y = y + 4; ln->h = 10; ln->scale = scale;
            y += 12;
            continue;
        }

        u8 kind = LK_TEXT;
        float sc = scale, indent = 0;
        u8 base = 0;
        char marker[16] = {0};
        float extra_before = 0;

        if (markdown && t[0] == '#') {
            int lvl = 0;
            while (lvl < tn && t[lvl] == '#') lvl++;
            if (lvl < tn && t[lvl] == ' ') {
                kind = LK_HEAD; base = MS_BOLD;
                sc = scale * (lvl == 1 ? 1.28f : lvl == 2 ? 1.16f : 1.06f);
                t += lvl + 1; tn -= lvl + 1;
                extra_before = L->nline ? 4 : 0;
            }
        } else if (markdown && tn >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            int lvl = sp / 2; if (lvl > 4) lvl = 4;
            indent = 14 + lvl * 12;
            strcpy(marker, lvl % 2 ? "–" : "•");
            t += 2; tn -= 2;
        } else if (markdown && tn >= 3 && isdigit((u8)t[0])) {
            int d = 0;
            while (d < tn && isdigit((u8)t[d])) d++;
            if (d < tn - 1 && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ' && d < 4) {
                int lvl = sp / 2; if (lvl > 4) lvl = 4;
                indent = 18 + lvl * 12;
                memcpy(marker, t, d + 1); marker[d + 1] = 0;
                t += d + 2; tn -= d + 2;
            }
        } else if (markdown && t[0] == '>') {
            kind = LK_QUOTE; indent = 10;
            t++; tn--;
            if (tn && t[0] == ' ') { t++; tn--; }
        } else if (markdown && t[0] == '|') {
            if (is_table_sep(t, tn)) { table_header = false; continue; }
            kind = LK_TABLE;
            // celdas separadas
            char buf[1024]; int bl = 0;
            int ti = 1;
            bool firstcell = true;
            while (ti < tn && bl < 1000) {
                int cs = ti;
                while (ti < tn && t[ti] != '|') ti++;
                int ce = ti;
                while (cs < ce && t[cs] == ' ') cs++;
                while (ce > cs && t[ce - 1] == ' ') ce--;
                if (ti < tn || ce > cs) {
                    if (!firstcell && bl < 990) { memcpy(buf + bl, "  ·  ", 6); bl += 6; }
                    int cl = ce - cs; if (bl + cl > 1000) cl = 1000 - bl;
                    memcpy(buf + bl, t + cs, cl); bl += cl;
                    firstcell = false;
                }
                ti++;
            }
            buf[bl] = 0;
            bool hdr = !table_header && (L->nline == 0 || L->lines[L->nline - 1].kind != LK_TABLE);
            table_header = true;
            MdLine *ln = line_new(L);
            parse_inline(&R, buf, bl, hdr ? MS_BOLD : 0, true);
            wrap_runs(L, &R, scale * 0.95f, 4, width, LK_TABLE, ln);
            for (int i = (int)(ln - L->lines); i < L->nline; i++) { L->lines[i].y = y; y += L->lines[i].h; }
            runs_free(&R);
            continue;
        }

        y += extra_before;
        MdLine *ln = line_new(L);
        if (marker[0]) {
            ln->marker_off = arena_add(L, marker, (int)strlen(marker));
            ln->marker_len = (int)strlen(marker);
            ln->marker_x = indent - 4 - text_width(marker, sc);
        }
        parse_inline(&R, t, tn, base, markdown);
        int first = (int)(ln - L->lines);
        wrap_runs(L, &R, sc, indent, width, kind, ln);
        for (int i = first; i < L->nline; i++) { L->lines[i].y = y; y += L->lines[i].h; }
        if (kind == LK_HEAD) y += 2;
        runs_free(&R);
    }
    if (in_code && code_start_line >= 0 && code_start_line < L->nline) {
        L->lines[code_start_line].code_first = 1;
        L->lines[L->nline - 1].code_last = 1;
    }
    L->height = y;
    free(text_buf);
    for (int i = 0; i < L->nline; i++) {
        MdLine *l = &L->lines[i];
        for (int j = 0; j < l->nseg; j++) {
            MdSeg *s = &L->segs[l->seg0 + j];
            if (s->x + s->w > L->maxw) L->maxw = s->x + s->w;
        }
    }
}

void md_draw(const MdLayout *L, float x, float y, float clip_top, float clip_bot, u32 col, float alpha) {
    char tmp[512];
    u32 c_text = col_alpha(col, alpha);
    u32 c_code = col_alpha(T.code_text, alpha);
    u32 c_link = col_alpha(T.link, alpha);
    u32 c_codebg = col_alpha(T.code_bg, alpha);
    u32 c_dim = col_alpha(T.text2, alpha);
    for (int i = 0; i < L->nline; i++) {
        const MdLine *l = &L->lines[i];
        float ly = y + l->y;
        if (ly + l->h < clip_top - 4 || ly > clip_bot + 4) continue;
        if (l->kind == LK_CODE) {
            float top = ly - (l->code_first ? 3 : 0);
            float h = l->h + (l->code_first ? 3 : 0) + (l->code_last ? 3 : 0);
            draw_rect(x, top, L->width, h, c_codebg);
            draw_rect(x, top, 2, h, col_alpha(T.accent, alpha * 0.7f));
        } else if (l->kind == LK_QUOTE) {
            draw_rect(x + 2, ly, 3, l->h, col_alpha(T.quote_bar, alpha));
        } else if (l->kind == LK_HR) {
            draw_rect(x, ly + 3, L->width, 1, col_alpha(T.divider, alpha));
            continue;
        } else if (l->kind == LK_TABLE) {
            draw_rect(x, ly, L->width, l->h, col_alpha(T.surface, alpha * 0.8f));
            draw_rect(x, ly + l->h - 1, L->width, 1, col_alpha(T.divider, alpha));
        }
        if (l->marker_off >= 0) {
            int n = l->marker_len < 15 ? l->marker_len : 15;
            memcpy(tmp, L->arena + l->marker_off, n); tmp[n] = 0;
            text_draw(tmp, x + l->marker_x, ly, l->scale, c_dim);
        }
        for (int j = 0; j < l->nseg; j++) {
            const MdSeg *s = &L->segs[l->seg0 + j];
            int n = s->len < 511 ? s->len : 511;
            memcpy(tmp, L->arena + s->off, n); tmp[n] = 0;
            u32 c = c_text;
            if (l->kind == LK_QUOTE) c = c_dim;
            if (s->flags & MS_LINK) c = c_link;
            if (s->flags & MS_CODE) {
                c = c_code;
                if (l->kind != LK_CODE) draw_rrect(x + s->x - 1, ly + 1, s->w + 2, l->h - LINE_GAP - 1, 3, c_codebg);
            }
            if (s->flags & MS_ITALIC && !(s->flags & MS_LINK)) c = col_lerp(c, T.accent, 0.25f);
            text_draw(tmp, x + s->x, ly, l->scale, c);
            if (s->flags & MS_BOLD) text_draw(tmp, x + s->x + 0.55f, ly, l->scale, c);
            if (s->flags & MS_LINK) draw_rect(x + s->x, ly + l->h - LINE_GAP - 2, s->w, 1, col_alpha(c_link, 0.6f));
        }
    }
}

void md_strip(const char *in, char *out, int outsz) {
    int o = 0;
    const char *p = in;
    bool line_start = true;
    while (*p && o < outsz - 1) {
        if (line_start) {
            while (*p == '#') p++;
            if (*p == ' ' && p > in && p[-1] == '#') p++;
            if ((*p == '-' || *p == '*') && p[1] == ' ') { out[o++] = '-'; out[o++] = ' '; p += 2; if (o >= outsz - 1) break; }
            if (*p == '>' ) { p++; if (*p == ' ') p++; }
            if (!strncmp(p, "```", 3)) { while (*p && *p != '\n') p++; continue; }
        }
        line_start = false;
        if ((*p == '*' && p[1] == '*') || (*p == '_' && p[1] == '_')) { p += 2; continue; }
        if (*p == '`') { p++; continue; }
        if (*p == '*' && p[1] != ' ') { p++; continue; }
        if (*p == '\n') line_start = true;
        out[o++] = *p++;
    }
    out[o] = 0;
}
