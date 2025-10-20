#ifndef LIBUSB_THREADS_CPP_STL_H
#define LIBUSB_THREADS_CPP_STL_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef timerisset
struct timeval {
        long    tv_sec;         /* seconds */
        long    tv_usec;        /* and microseconds */
};
#define timerisset(tvp)         ((tvp)->tv_sec || (tvp)->tv_usec)
#endif

#ifndef timercmp
#define timercmp(tvp, uvp, cmp) \
        ((tvp)->tv_sec cmp (uvp)->tv_sec || \
         (tvp)->tv_sec == (uvp)->tv_sec && (tvp)->tv_usec cmp (uvp)->tv_usec)
#endif

#ifndef timerclear
#define timerclear(tvp)         (tvp)->tv_sec = (tvp)->tv_usec = 0
#endif

#define USBI_MUTEX_INITIALIZER	(NULL)
typedef struct cpp_stl_usbi_mutex_static* usbi_mutex_static_t;

void usbi_mutex_static_lock(usbi_mutex_static_t *mutex);
void usbi_mutex_static_unlock(usbi_mutex_static_t *mutex);

typedef struct cpp_stl_usbi_mutex* usbi_mutex_t;

void usbi_mutex_init(usbi_mutex_t *mutex);
void usbi_mutex_lock(usbi_mutex_t *mutex);
void usbi_mutex_unlock(usbi_mutex_t *mutex);
int usbi_mutex_trylock(usbi_mutex_t *mutex);
void usbi_mutex_destroy(usbi_mutex_t *mutex);

#if !defined(HAVE_STRUCT_TIMESPEC) && !defined(_TIMESPEC_DEFINED)
#define HAVE_STRUCT_TIMESPEC 1
#define _TIMESPEC_DEFINED 1
struct timespec {
	long tv_sec;
	long tv_nsec;
};
#endif /* HAVE_STRUCT_TIMESPEC || _TIMESPEC_DEFINED */

typedef struct cpp_stl_usbi_cond* usbi_cond_t;

void usbi_cond_init(usbi_cond_t *cond);
void usbi_cond_wait(usbi_cond_t *cond, usbi_mutex_t *mutex);
int usbi_cond_timedwait(usbi_cond_t *cond, usbi_mutex_t *mutex, const struct timeval *tv);
void usbi_cond_broadcast(usbi_cond_t *cond);
void usbi_cond_destroy(usbi_cond_t *cond);

typedef struct cpp_stl_usbi_tls* usbi_tls_key_t;

void usbi_tls_key_create(usbi_tls_key_t *key);
void *usbi_tls_key_get(usbi_tls_key_t key);
void usbi_tls_key_set(usbi_tls_key_t key, void *ptr);
void usbi_tls_key_delete(usbi_tls_key_t key);

size_t usbi_get_tid();

#ifdef __cplusplus
}
#endif

#endif // LIBUSB_THREADS_CPP_STL_H
