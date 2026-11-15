/* rvio.h: the C library's stdio for the DOS builds, small (rvio.c).  The user's pick from the memory plan, 2026-11-14:
 * "The C library's printf and file functions replaced with small DOS calls".  Watcom's stdio (its FILE table, its
 * buffers in the near heap, the printf and scanf engines and the POSIX-level read with its text mode) was some 11 KB of
 * RV.EXE; this is the part the game uses, on DOS's own file calls.
 *
 * The sources include this, never <stdio.h>: on DOS the usual names are macros for rvio.c's, so the code reads as it did;
 * the native builds (RV_HOST) have the real stdio here, and host/rvhost.h still sends their opens through fsguard.c.
 *
 * What is there: fopen (r, rb, w, wb, a, ab), fclose, fread, fwrite, fseek, ftell, fgetc, ungetc, fgets, fputc, fprintf,
 * vfprintf, printf, sprintf, vsprintf, remove; and for a raw handle rv_open (to read), rv_read, rv_lseek, rv_close.
 * Text mode as stdio's: a file opened without 'b' has '\n' written as CR LF and CR dropped as it is read (fgetc, fgets).
 * The formats: %d %u %x %X %s %c %%, the flags - + 0, a width, a string's precision, and the sizes l (long) and F (a far
 * string), which is every format the game has (2026-11-14). */
#ifndef RVIO_H
#define RVIO_H

#include <stdarg.h>
#include <stddef.h>

#if defined(RV_HOST)

#include <stdio.h>
/* (the raw handle's: where they are used the file has <io.h> and <fcntl.h>, which only the game's native build has) */
#define rv_open(p) open((p), O_RDONLY | O_BINARY)
#define rv_read(fd, b, n) read((fd), (b), (n))
#define rv_lseek(fd, o, w) lseek((fd), (o), (w))
#define rv_close(fd) close(fd)

#else

typedef struct RvFile RvFile;
#define FILE RvFile
#ifndef EOF
#define EOF (-1)
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

RvFile *rv_fopen(const char *name, const char *mode);
int rv_fclose(RvFile *f);
size_t rv_fread(void __far *buf, size_t size, size_t n, RvFile *f);
size_t rv_fwrite(const void __far *buf, size_t size, size_t n, RvFile *f);
int rv_fseek(RvFile *f, long off, int whence);
long rv_ftell(RvFile *f);
int rv_fgetc(RvFile *f);
int rv_ungetc(int c, RvFile *f);
char *rv_fgets(char *s, int n, RvFile *f);
int rv_fputc(int c, RvFile *f);
int rv_fprintf(RvFile *f, const char __far *fmt, ...);
int rv_vfprintf(RvFile *f, const char __far *fmt, va_list ap);
int rv_printf(const char __far *fmt, ...);
int rv_sprintf(char *out, const char __far *fmt, ...);
int rv_vsprintf(char *out, const char __far *fmt, va_list ap);
int rv_remove(const char *name);
int rv_fileno(RvFile *f);               /* its DOS handle (its read-ahead dropped: the caller reads the handle itself) */

int rv_open(const char *path);          /* to read, binary: a DOS handle, -1 none */
int rv_read(int fd, void __far *buf, unsigned n);   /* bytes read, -1 a failure */
long rv_lseek(int fd, long off, int whence);        /* the new place, -1 a failure */
void rv_close(int fd);

#define fopen rv_fopen
#define fclose rv_fclose
#define fread rv_fread
#define fwrite rv_fwrite
#define fseek rv_fseek
#define ftell rv_ftell
#define fgetc rv_fgetc
#define ungetc rv_ungetc
#define fgets rv_fgets
#define fputc rv_fputc
#define fprintf rv_fprintf
#define vfprintf rv_vfprintf
#define printf rv_printf
#define sprintf rv_sprintf
#define vsprintf rv_vsprintf
#define remove rv_remove
#define fileno rv_fileno
#define lseek rv_lseek

#endif
#endif
