/* rvio.c: the C library's stdio for the DOS builds, on DOS's own file calls (rvio.h says what is there and why).
 * Files are a few at once (RV_FILES: the log, a script, a data file, the page's want list), each a DOS handle with a
 * little read-ahead for fgetc and fgets.  Text output goes through one formatter (fmt), into a string or, a line at a
 * time, into a file. */
#include <dos.h>
#include <i86.h>
#include <string.h>
#include "rvio.h"

#ifndef RV_HOST

#define RV_FILES 6
#define AHEAD 48                        /* a file's read-ahead, bytes (fgetc, fgets) */

struct RvFile {
    int h;                              /* the DOS handle; -1 a free slot */
    unsigned char text;                 /* no 'b': '\n' written as CR LF, CR dropped as it is read */
    unsigned char pos, len;             /* the read-ahead's next byte, and how many it holds */
    int back;                           /* ungetc's character, EOF none */
    unsigned char buf[AHEAD];
};
static RvFile g_file[RV_FILES] = { { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 } };
static RvFile g_out = { 1, 1, 0, 0, EOF };   /* (printf's: DOS's standard output) */

/* --- DOS --- */

long rv_lseek(int fd, long off, int whence)
{
    union REGS r;
    r.h.ah = 0x42;
    r.h.al = (unsigned char)whence;
    r.x.bx = (unsigned)fd;
    r.x.cx = (unsigned)((unsigned long)off >> 16);
    r.x.dx = (unsigned)off;
    intdos(&r, &r);
    if (r.x.cflag)
        return -1L;
    return (long)((unsigned long)r.x.dx << 16 | r.x.ax);
}

int rv_open(const char *path)
{
    int h;
    return _dos_open(path, 0, &h) ? -1 : h;
}

int rv_read(int fd, void __far *buf, unsigned n)
{
    unsigned got;
    return _dos_read(fd, buf, n, &got) ? -1 : (int)got;
}

void rv_close(int fd)
{
    _dos_close(fd);
}

int rv_remove(const char *name)
{
    union REGS r;
    r.h.ah = 0x41;
    r.x.dx = (unsigned)name;            /* (DS:DX: the name is in DGROUP) */
    intdos(&r, &r);
    return r.x.cflag ? -1 : 0;
}

/* --- files --- */

RvFile *rv_fopen(const char *name, const char *mode)
{
    RvFile *f = 0;
    int i, h;
    for (i = 0; i < RV_FILES && !f; i++)
        if (g_file[i].h < 0)
            f = &g_file[i];
    if (!f)
        return 0;
    if (mode[0] == 'r') {
        if (_dos_open(name, 0, &h))
            return 0;
    } else if (mode[0] == 'a') {
        if (_dos_open(name, 1, &h) == 0)
            rv_lseek(h, 0L, SEEK_END);
        else if (_dos_creat(name, _A_NORMAL, &h))
            return 0;
    } else if (_dos_creat(name, _A_NORMAL, &h))
        return 0;
    f->h = h;
    f->text = (unsigned char)(strchr(mode, 'b') == 0);
    f->pos = f->len = 0;
    f->back = EOF;
    return f;
}

int rv_fileno(RvFile *f)
{
    f->pos = f->len = 0;
    f->back = EOF;
    return f->h;
}

int rv_fclose(RvFile *f)
{
    if (!f || f->h < 0)
        return EOF;
    _dos_close(f->h);
    f->h = -1;
    return 0;
}

size_t rv_fread(void __far *buf, size_t size, size_t n, RvFile *f)
{
    unsigned char __far *p = (unsigned char __far *)buf;
    unsigned want = size * n, got = 0, more;
    if (!size || !want)
        return 0;
    if (f->back != EOF) {
        p[got++] = (unsigned char)f->back;
        f->back = EOF;
    }
    while (got < want && f->pos < f->len)
        p[got++] = f->buf[f->pos++];
    if (got < want && !_dos_read(f->h, p + got, want - got, &more))
        got += more;
    return got / size;
}

static void write_raw(RvFile *f, const unsigned char __far *p, unsigned n)
{
    unsigned put;
    if (n)
        _dos_write(f->h, p, n, &put);
}

/* n bytes out, CR LF for each '\n' in text mode */
static void write_text(RvFile *f, const unsigned char __far *p, unsigned n)
{
    static const unsigned char __far s_crlf[2] = { '\r', '\n' };
    unsigned i, from = 0;
    if (!f->text) {
        write_raw(f, p, n);
        return;
    }
    for (i = 0; i < n; i++)
        if (p[i] == '\n') {
            write_raw(f, p + from, i - from);
            write_raw(f, s_crlf, 2);
            from = i + 1;
        }
    write_raw(f, p + from, n - from);
}

size_t rv_fwrite(const void __far *buf, size_t size, size_t n, RvFile *f)
{
    if (!size)
        return 0;
    write_text(f, (const unsigned char __far *)buf, size * n);
    return n;
}

int rv_fputc(int c, RvFile *f)
{
    unsigned char b = (unsigned char)c;
    write_text(f, (const unsigned char __far *)&b, 1);
    return c;
}

int rv_fseek(RvFile *f, long off, int whence)
{
    if (whence == SEEK_CUR)             /* (from where the reader is, not where the read-ahead got to) */
        off -= (long)(f->len - f->pos) + (f->back != EOF);
    f->pos = f->len = 0;
    f->back = EOF;
    return rv_lseek(f->h, off, whence) < 0 ? -1 : 0;
}

long rv_ftell(RvFile *f)
{
    long at = rv_lseek(f->h, 0L, SEEK_CUR);
    return at < 0 ? -1L : at - (long)(f->len - f->pos) - (f->back != EOF);
}

int rv_fgetc(RvFile *f)
{
    unsigned got;
    int c;
    if (f->back != EOF) {
        c = f->back;
        f->back = EOF;
        return c;
    }
    for (;;) {
        if (f->pos == f->len) {
            f->pos = f->len = 0;
            if (_dos_read(f->h, (unsigned char __far *)f->buf, AHEAD, &got) || !got)
                return EOF;
            f->len = (unsigned char)got;
        }
        c = f->buf[f->pos++];
        if (!(f->text && c == '\r'))
            return c;
    }
}

int rv_ungetc(int c, RvFile *f)
{
    f->back = c;
    return c;
}

char *rv_fgets(char *s, int n, RvFile *f)
{
    int i = 0, c = 0;
    while (i < n - 1 && (c = rv_fgetc(f)) != EOF) {
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    s[i] = 0;
    return i ? s : 0;
}

/* --- the formatter --- */

typedef struct {
    char *out;                          /* into a string, or */
    RvFile *f;                          /* a file, a line's worth at a time */
    unsigned n, total;
    char buf[96];
} Sink;

static void emit(Sink *k, char c)
{
    k->total++;
    if (!k->f) {
        *k->out++ = c;
        return;
    }
    k->buf[k->n++] = c;
    if (k->n == sizeof k->buf) {
        write_text(k->f, (const unsigned char __far *)k->buf, k->n);
        k->n = 0;
    }
}

static void pad(Sink *k, int n, char c)
{
    while (n-- > 0)
        emit(k, c);
}

static void fmt(Sink *k, const char __far *p, va_list ap)
{
    for (; *p; p++) {
        int minus = 0, zero = 0, plus = 0, lng = 0, far_ = 0, width = 0, prec = -1;
        if (*p != '%') {
            emit(k, *p);
            continue;
        }
        for (p++;; p++)
            if (*p == '-')
                minus = 1;
            else if (*p == '0')
                zero = 1;
            else if (*p == '+')
                plus = 1;
            else if (*p != ' ' && *p != '#')
                break;
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');
        if (*p == '.')
            for (prec = 0, p++; *p >= '0' && *p <= '9'; p++)
                prec = prec * 10 + (*p - '0');
        for (;; p++)
            if (*p == 'l')
                lng = 1;
            else if (*p == 'F')
                far_ = 1;
            else if (*p != 'h' && *p != 'N')
                break;
        switch (*p) {
        case 'd': case 'u': case 'x': case 'X': {
            unsigned long v;
            unsigned base = *p == 'd' || *p == 'u' ? 10 : 16;
            char digits[12], sign = 0;
            int n = 0;
            if (*p == 'd') {
                long sv = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
                v = sv < 0 ? 0UL - (unsigned long)sv : (unsigned long)sv;
                sign = sv < 0 ? '-' : plus ? '+' : 0;
            } else
                v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned);
            do {
                unsigned d = (unsigned)(v % base);
                digits[n++] = (char)(d < 10 ? '0' + d : (*p == 'x' ? 'a' : 'A') + d - 10);
                v /= base;
            } while (v);
            width -= n + (sign != 0);
            if (!minus && !zero)
                pad(k, width, ' ');
            if (sign)
                emit(k, sign);
            if (!minus && zero)
                pad(k, width, '0');
            while (n)
                emit(k, digits[--n]);
            if (minus)
                pad(k, width, ' ');
            break;
        }
        case 's': {
            const char __far *s = far_ ? va_arg(ap, const char __far *) : (const char __far *)va_arg(ap, const char *);
            int n = 0;
            if (!s)
                s = "(null)";
            while (s[n] && (prec < 0 || n < prec))
                n++;
            if (!minus)
                pad(k, width - n, ' ');
            for (width -= n; n; n--)
                emit(k, *s++);
            if (minus)
                pad(k, width, ' ');
            break;
        }
        case 'c':
            emit(k, (char)va_arg(ap, int));
            break;
        case 0:
            return;
        default:                        /* (%% and anything unknown: as it is) */
            emit(k, *p);
        }
    }
}

int rv_vsprintf(char *out, const char __far *f, va_list ap)
{
    Sink k;
    k.out = out;
    k.f = 0;
    k.n = k.total = 0;
    fmt(&k, f, ap);
    *k.out = 0;
    return (int)k.total;
}

int rv_sprintf(char *out, const char __far *f, ...)
{
    va_list ap;
    int n;
    va_start(ap, f);
    n = rv_vsprintf(out, f, ap);
    va_end(ap);
    return n;
}

int rv_vfprintf(RvFile *file, const char __far *f, va_list ap)
{
    Sink k;
    k.out = 0;
    k.f = file;
    k.n = k.total = 0;
    fmt(&k, f, ap);
    write_text(file, (const unsigned char __far *)k.buf, k.n);
    return (int)k.total;
}

int rv_fprintf(RvFile *file, const char __far *f, ...)
{
    va_list ap;
    int n;
    va_start(ap, f);
    n = rv_vfprintf(file, f, ap);
    va_end(ap);
    return n;
}

int rv_printf(const char __far *f, ...)
{
    va_list ap;
    int n;
    va_start(ap, f);
    n = rv_vfprintf(&g_out, f, ap);
    va_end(ap);
    return n;
}

#endif
