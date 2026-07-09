#ifndef __KSU_H_SULOG_EVENT
#define __KSU_H_SULOG_EVENT

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
#include <linux/compiler_types.h>
#endif
#include <linux/gfp.h>
#include <linux/types.h>
#include "uapi/sulog.h" // IWYU pragma: keep

struct ksu_event_queue;
struct ksu_sulog_pending_event;

int ksu_sulog_events_init(void);
void ksu_sulog_events_exit(void);

struct ksu_sulog_pending_event *ksu_sulog_capture(__u16 event_type, const char *bprm_argv, size_t bprm_argv_len, gfp_t gfp);

void ksu_sulog_emit_pending(struct ksu_sulog_pending_event *pending, int retval, gfp_t gfp);

int ksu_sulog_emit_grant_root(int retval, __u32 uid, __u32 euid, gfp_t gfp);
int ksu_sulog_emit(__u16 event_type, const char *bprm_argv, size_t bprm_argv_len, gfp_t gfp);
static void ksu_sulog_emit_bprm(const char *filename);

struct ksu_event_queue *ksu_sulog_get_queue(void);

#endif