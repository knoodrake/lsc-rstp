/* Compat uClibc 1.0.28 : symboles fournis par glibc mais absents de la caméra.
 * - __assert_fail  : utilisé par assert() de glibc (libnl en a besoin)
 * - __isoc23_strto*: glibc >= 2.38 redirige strtol/strtoul/... vers ces noms
 * On les implémente au-dessus des fonctions uClibc (mêmes signatures).
 */
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <locale.h>
#include <string.h>

void __assert_fail(const char *assertion, const char *file, unsigned int line,
                   const char *function)
{
    (void)assertion; (void)file; (void)line; (void)function;
    static const char msg[] = "assertion failed\n";
    write(2, msg, sizeof(msg) - 1);
    abort();
}

/* déclarations explicites (les macros glibc ont été neutralisées par -D ci-dessous) */
extern long int strtol(const char *, char **, int);
extern unsigned long int strtoul(const char *, char **, int);
extern long long int strtoll(const char *, char **, int);
extern unsigned long long int strtoull(const char *, char **, int);

long int __isoc23_strtol(const char *n, char **e, int b) { return strtol(n, e, b); }
unsigned long int __isoc23_strtoul(const char *n, char **e, int b) { return strtoul(n, e, b); }
long long int __isoc23_strtoll(const char *n, char **e, int b) { return strtoll(n, e, b); }
unsigned long long int __isoc23_strtoull(const char *n, char **e, int b) { return strtoull(n, e, b); }

extern int vsscanf(const char *, const char *, va_list);
extern int vfscanf(void *, const char *, va_list);
int __isoc23_fscanf(void *f, const char *fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt); r = vfscanf(f, fmt, ap); va_end(ap);
    return r;
}
int __isoc23_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt); r = vsscanf(s, fmt, ap); va_end(ap);
    return r;
}

/* ctype : glibc expose des tables via __ctype_*_loc(), uClibc exporte les tables
 * directement (même encodage de bits _ISbit que glibc). */
extern const unsigned short int *__ctype_b;
extern const int *__ctype_tolower;
extern const int *__ctype_toupper;

const unsigned short int **__ctype_b_loc(void) { return &__ctype_b; }
const int **__ctype_tolower_loc(void) { return &__ctype_tolower; }
const int **__ctype_toupper_loc(void) { return &__ctype_toupper; }

/* locale : le firmware uClibc est compilé sans support locale complet. */
locale_t newlocale(int category_mask, const char *locale_name, locale_t base)
{ (void)category_mask; (void)locale_name; (void)base; return (locale_t) 1; }

void freelocale(locale_t locobj) { (void)locobj; }

char *strerror_l(int errnum, locale_t locobj) { (void)locobj; return strerror(errnum); }
