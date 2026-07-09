#ifndef __KSU_H_KERNEL_COMPAT
#define __KSU_H_KERNEL_COMPAT

#include <linux/fs.h>
#include <linux/version.h>
#include <linux/task_work.h>
#include "ss/policydb.h"
#include "linux/key.h"

/*
 * Adapt to Huawei HISI kernel without affecting other kernels ,
 * Huawei Hisi Kernel EBITMAP Enable or Disable Flag ,
 * From ss/ebitmap.h
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)) &&                         \
		(LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)) ||             \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)) &&                    \
		(LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))
#ifdef HISI_SELINUX_EBITMAP_RO
#define CONFIG_IS_HW_HISI
#endif
#endif

// Checks for UH, KDP and RKP
#ifdef SAMSUNG_UH_DRIVER_EXIST
#if defined(CONFIG_UH) || defined(CONFIG_KDP) || defined(CONFIG_RKP)
#error "CONFIG_UH, CONFIG_KDP and CONFIG_RKP is enabled! Please disable or remove it before compile a kernel with KernelSU!"
#endif
#endif

extern struct file *ksu_filp_open_compat(const char *filename, int flags,
					 umode_t mode);
extern ssize_t ksu_kernel_read_compat(struct file *p, void *buf, size_t count,
				      loff_t *pos);
extern ssize_t ksu_kernel_write_compat(struct file *p, const void *buf,
				       size_t count, loff_t *pos);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) ||                           \
	defined(CONFIG_IS_HW_HISI) || defined(CONFIG_KSU_ALLOWLIST_WORKAROUND)
extern struct key *init_session_keyring;
#endif

extern long ksu_copy_from_user_nofault(void *dst, const void __user *src, size_t size);
/*
 * ksu_copy_from_user_retry
 * try nofault copy first, if it fails, try with plain
 * paramters are the same as copy_from_user
 * 0 = success
 */
static inline long ksu_copy_from_user_retry(void *to, 
		const void __user *from, unsigned long count)
{
	long ret = ksu_copy_from_user_nofault(to, from, count);
	if (likely(!ret))
		return ret;

	// we faulted! fallback to slow path
	return copy_from_user(to, from, count);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
extern void *ksu_compat_kvrealloc(const void *p, size_t oldsize, size_t newsize,
				  gfp_t flags);
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
static inline void *ksu_kvmalloc(size_t size, gfp_t flags)
{
	void *buf = kmalloc(size, flags);
	if (!buf)
		buf = vmalloc(size);
	
	return buf;
}

static inline void ksu_kvfree(const void *buf)
{
	if (is_vmalloc_addr(buf))
		vfree(buf);
	else
		kfree(buf);
}
#define kvmalloc ksu_kvmalloc
#define kvfree ksu_kvfree
#endif

// https://elixir.bootlin.com/linux/v4.14.222/source/lib/string.c#L282
static inline ssize_t __strscpy_pad(char *dest, const char *src, size_t count)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 222)
    return strscpy_pad(dest, src, count);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
    ssize_t res = strscpy(dest, src, count);
    if (res >= 0 && (size_t)res < count) {
        memset(dest + res, 0, count - res);
    }
    return res;
#else
    if (count == 0)
        return -E2BIG;

    memcpy(dest, src, count);
    dest[count - 1] = '\0';
    return strlen(dest);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#define ksu_access_ok(addr, size) access_ok(addr, size)
#else
#define ksu_access_ok(addr, size) access_ok(VERIFY_READ, addr, size)
#endif

#ifndef KSU_OPTIONAL_STRNCPY
extern long strncpy_from_user_nofault(char *dst, const void __user *unsafe_addr,
				   long count);
#endif // #ifndef KSU_OPTIONAL_STRNCPY

// Linux >= 5.7
// task_work_add (struct, struct, enum)
// Linux pre-5.7
// task_work_add (struct, struct, bool)
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
#ifndef TWA_RESUME
#define TWA_RESUME true
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION (4, 12, 0) && !defined(EPOLLIN)
#define EPOLLIN		0x00000001
#define EPOLLPRI	0x00000002
#define EPOLLOUT	0x00000004
#define EPOLLERR	0x00000008
#define EPOLLHUP	0x00000010
#define EPOLLRDNORM	0x00000040
#define EPOLLRDBAND	0x00000080
#define EPOLLWRNORM	0x00000100
#define EPOLLWRBAND	0x00000200
#define EPOLLMSG	0x00000400
#define EPOLLRDHUP	0x00002000
#endif // < 4.12 && !EPOLLIN

#if LINUX_VERSION_CODE < KERNEL_VERSION (3, 15, 0)
#define task_ppid_nr(a) (pid_t)sys_getppid()
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION (3, 17, 0)
static inline u64 ksu_ktime_get_ns(void) { return ktime_to_ns(ktime_get()); }
#define ktime_get_ns ksu_ktime_get_ns
#endif

// WARNING: no overflow safety!
#ifndef struct_size
#define struct_size(p, member, n) (sizeof(*(p)) + (n) * sizeof(*(p)->member))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION (4, 12, 0)
#ifndef ALIGN_DOWN
#define ALIGN_DOWN(x, a) __ALIGN_KERNEL((x) - ((a) - 1), (a))
#endif
#endif

#endif // #ifndef __KSU_H_KERNEL_COMPAT
