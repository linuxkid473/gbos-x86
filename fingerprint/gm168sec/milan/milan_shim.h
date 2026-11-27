/* milan_shim.h - minimal glib replacement for the Milan matcher.
 *
 * Installed as glib.h in the shim include dir so the vendored Milan
 * sources (#include <glib.h> in 4 headers) build against Bionic, which
 * has no glib. Only the surface Milan actually uses is provided; see
 * milan-audit.md for the full audit (file/line references).
 *
 * Provided: integer typedefs, TRUE/FALSE, GBytes (+ref/unref/new/take/
 * get_data/get_size/equal), minimal GError (+new_literal/set_error_literal/
 * clear_error, G_DEFINE_QUARK), GPtrArray (+new_with_free_func/add/unref/
 * index), GDestroyNotify, malloc family (g_malloc/free/new0/new/memdup2/
 * try_malloc/try_malloc0/try_new0/malloc0), g_return_*, g_autofree,
 * g_autoptr (+G_DEFINE_AUTOPTR_CLEANUP_FUNC), g_clear_pointer,
 * g_steal_pointer, g_bytes_equal, g_strcmp0, GUINT32_TO_LE.
 *
 * Deliberately NOT provided: GVariant (only used by print.c's
 * build_data/parse_data, the libfprint FpPrint envelope - deleted from
 * the vendored print.c, see milan-audit.md), GList/GObject/GTask/GMutex/
 * GByteArray (unused), g_compute_checksum_for_data / g_strlcpy (only in
 * debug-hooks.c, excluded from the build - GOODIX53X5_DEBUG off).
 */
#ifndef GM_MILAN_SHIM_H
#define GM_MILAN_SHIM_H

/* strdup() and friends under -std=c11. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <endian.h>

#define G_STRUCT_OFFSET(s, f) offsetof(s, f)
#define G_ALIGNOF(t) _Alignof(t)
#define G_STATIC_ASSERT(expr) _Static_assert(expr, #expr)
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define G_MAXSIZE SIZE_MAX
#define G_MAXUINT UINT_MAX
#define G_MAXINT INT_MAX
#define G_MININT INT_MIN
#define G_MAXUINT32 UINT32_MAX
#define G_MAXINT32 INT32_MAX

typedef int8_t gint8;
typedef uint8_t guint8;
typedef int16_t gint16;
typedef uint16_t guint16;
typedef int32_t gint32;
typedef uint32_t guint32;
typedef int64_t gint64;
typedef uint64_t guint64;
typedef short gshort;
typedef unsigned short gushort;
typedef int gint;
typedef unsigned int guint;
typedef long glong;
typedef unsigned long gulong;
typedef float gfloat;
typedef double gdouble;
typedef char gchar;
typedef unsigned char guchar;
typedef int gboolean;
typedef void *gpointer;
typedef const void *gconstpointer;
typedef size_t gsize;
typedef ssize_t gssize;
typedef intptr_t gintptr;
typedef uintptr_t guintptr;
typedef uint32_t GQuark;
typedef void (*GDestroyNotify)(void *);

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define GUINT32_TO_LE(v) htole32(v)

/* ---- GBytes: refcounted immutable byte buffer ---- */
typedef struct {
	uint8_t *data;
	size_t size;
	int ref;
} GBytes;

static inline GBytes *g_bytes_new(const void *data, size_t size)
{
	GBytes *b = (GBytes *)malloc(sizeof(*b));
	if (!b)
		return NULL;
	b->data = (uint8_t *)malloc(size ? size : 1);
	if (!b->data) {
		free(b);
		return NULL;
	}
	if (size)
		memcpy(b->data, data, size);
	b->size = size;
	b->ref = 1;
	return b;
}
static inline GBytes *g_bytes_new_take(void *data, size_t size)
{
	GBytes *b = (GBytes *)malloc(sizeof(*b));
	if (!b) {
		free(data);
		return NULL;
	}
	b->data = (uint8_t *)data;
	b->size = size;
	b->ref = 1;
	return b;
}
static inline GBytes *g_bytes_ref(GBytes *b)
{
	if (b)
		b->ref++;
	return b;
}
static inline void g_bytes_unref(GBytes *b)
{
	if (b && --b->ref == 0) {
		free(b->data);
		free(b);
	}
}
static inline const void *g_bytes_get_data(GBytes *b, size_t *size)
{
	if (size)
		*size = b ? b->size : 0;
	return b ? b->data : NULL;
}
static inline size_t g_bytes_get_size(GBytes *b)
{
	return b ? b->size : 0;
}
static inline gboolean g_bytes_equal(const GBytes *a, const GBytes *b)
{
	if (a == b)
		return TRUE;
	if (!a || !b || a->size != b->size)
		return FALSE;
	return memcmp(a->data, b->data, a->size) == 0 ? TRUE : FALSE;
}
static inline void gm_autoptr_cleanup_GBytes(void *p)
{
	g_bytes_unref(*(GBytes **)p);
}

/* ---- GError: domain/code/message ---- */
typedef struct {
	GQuark domain;
	gint code;
	gchar *message;
} GError;

static inline GError *g_error_new_literal(GQuark domain, gint code, const gchar *message)
{
	GError *e = (GError *)malloc(sizeof(*e));
	if (!e)
		return NULL;
	e->domain = domain;
	e->code = code;
	e->message = message ? strdup(message) : NULL;
	return e;
}
static inline void g_set_error_literal(GError **err, GQuark domain, gint code, const gchar *message)
{
	if (!err)
		return;
	if (*err) {
		free((*err)->message);
		free(*err);
	}
	*err = g_error_new_literal(domain, code, message);
}
static inline void g_clear_error(GError **err)
{
	if (err && *err) {
		free((*err)->message);
		free(*err);
		*err = NULL;
	}
}
static inline void g_error_free(GError *e)
{
	if (e) {
		free(e->message);
		free(e);
	}
}
static inline void gm_autoptr_cleanup_GError(void *p)
{
	g_error_free(*(GError **)p);
}
/* glib's G_DEFINE_QUARK(name, func) defines func##_quark(). */
#define G_DEFINE_QUARK(name, func) GQuark func##_quark(void) { return 1; }
#define G_N_ELEMENTS(arr) (sizeof(arr) / sizeof((arr)[0]))

/* ---- GPtrArray ---- */
typedef struct {
	void **pdata;
	size_t len;
	size_t cap;
	GDestroyNotify free_func;
} GPtrArray;

static inline GPtrArray *g_ptr_array_new_with_free_func(GDestroyNotify free_func)
{
	GPtrArray *a = (GPtrArray *)calloc(1, sizeof(*a));
	if (a)
		a->free_func = free_func;
	return a;
}
static inline void g_ptr_array_add(GPtrArray *a, void *data)
{
	if (a->len == a->cap) {
		size_t ncap = a->cap ? a->cap * 2 : 8;
		void **np = (void **)realloc(a->pdata, ncap * sizeof(void *));
		if (!np)
			return;
		a->pdata = np;
		a->cap = ncap;
	}
	a->pdata[a->len++] = data;
}
static inline void g_ptr_array_unref(GPtrArray *a)
{
	if (!a)
		return;
	if (a->free_func)
		for (size_t i = 0; i < a->len; i++)
			if (a->pdata[i])
				a->free_func(a->pdata[i]);
	free(a->pdata);
	free(a);
}
#define g_ptr_array_index(a, i) ((a)->pdata[(i)])
static inline void gm_autoptr_cleanup_GPtrArray(void *p)
{
	g_ptr_array_unref(*(GPtrArray **)p);
}

/* ---- allocation ---- */
/* g_free is a real function in glib (it is passed as a GDestroyNotify
 * to g_clear_pointer), so it must not be a macro here. */
static inline void g_free(void *p)
{
	free(p);
}
#define g_malloc(n) malloc(n)
#define g_malloc0(n) calloc(1, (n))
#define g_try_malloc(n) malloc(n)
#define g_try_malloc0(n) calloc(1, (n))
#define g_new(T, n) ((T *)malloc(sizeof(T) * (size_t)(n)))
#define g_new0(T, n) ((T *)calloc((size_t)(n), sizeof(T)))
#define g_try_new0(T, n) ((T *)calloc((size_t)(n), sizeof(T)))
static inline void *g_memdup2(const void *mem, size_t size)
{
	void *p = malloc(size ? size : 1);
	if (p && size)
		memcpy(p, mem, size);
	return p;
}

/* ---- g_return_* ---- */
#define g_return_if_fail(expr) do { if (!(expr)) return; } while (0)
#define g_return_val_if_fail(expr, val) do { if (!(expr)) return (val); } while (0)

/* ---- cleanup attributes (Clang __attribute__((cleanup))) ---- */
static inline void gm_shim_autofree(void *p)
{
	free(*(void **)p);
}
#define g_autofree __attribute__((cleanup(gm_shim_autofree)))
/* glib: g_autoptr(T) is __attribute__((cleanup(f))) T * (pointer type). */
#define g_autoptr(Type) __attribute__((cleanup(gm_autoptr_cleanup_##Type))) Type *
#define G_DEFINE_AUTOPTR_CLEANUP_FUNC(Type, func) \
	static inline void gm_autoptr_cleanup_##Type(void *p) { func(*(Type **)p); }

/* ---- misc ---- */
#define g_clear_pointer(pp, destroy) do { \
		void **__pp = (void **)(pp); \
		if (*__pp) { \
			void *__p = *__pp; \
			*__pp = NULL; \
			((GDestroyNotify)(destroy))(__p); \
		} \
	} while (0)
#define g_steal_pointer(pp) ({ \
		void **__pp = (void **)(pp); \
		void *__p = *__pp; \
		*__pp = NULL; \
		__p; \
	})
static inline int g_strcmp0(const char *a, const char *b)
{
	if (!a)
		return b ? -1 : 0;
	if (!b)
		return 1;
	return strcmp(a, b);
}

#endif /* GM_MILAN_SHIM_H */
