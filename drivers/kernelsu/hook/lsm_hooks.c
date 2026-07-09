#include <linux/lsm_hooks.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <linux/binfmts.h>
#include <linux/err.h>
#include <linux/atomic.h>

#include "klog.h" // IWYU pragma: keep
#include "runtime/ksud_boot.h"
#include "compat/kernel_compat.h"
#include "setuid_hook.h"
#include "manager/throne_tracker.h"

#ifndef KSU_KPROBES_HOOK

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) ||                           \
	defined(CONFIG_IS_HW_HISI) || defined(CONFIG_KSU_ALLOWLIST_WORKAROUND)
struct key *init_session_keyring = NULL;

static int ksu_key_permission(key_ref_t key_ref, const struct cred *cred,
			      unsigned perm)
{
	if (init_session_keyring != NULL) {
		return 0;
	}
	if (strcmp(current->comm, "init")) {
		// we are only interested in `init` process
		return 0;
	}
	init_session_keyring = cred->session_keyring;
	pr_info("kernel_compat: got init_session_keyring\n");
	return 0;
}
#endif

extern int ksu_hide_setprocattr(const char *name, void *value, size_t size);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
static int (*selinux_setprocattr_fn)(const char *name, void *value, size_t size) __read_mostly = NULL;
static __nocfi int ksu_setprocattr_wrapper(const char *name, void *value, size_t size)
{
	ksu_hide_setprocattr(name, value, size);
	if (likely(selinux_setprocattr_fn))
		return selinux_setprocattr_fn(name, value, size);
	return 0;
}
#define ksu_security_add_hooks security_add_hooks
#else
static int (*selinux_setprocattr_fn)(struct task_struct *p, char *name, void *value, size_t size) __read_mostly = NULL;
static __nocfi int ksu_setprocattr_wrapper(struct task_struct *p, char *name, void *value, size_t size)
{
	ksu_hide_setprocattr(name, value, size);
	if (likely(selinux_setprocattr_fn))
		return selinux_setprocattr_fn(p, name, value, size);

	return 0;
}
#define ksu_security_add_hooks(a, b, c) security_add_hooks(a, b)
#endif

/**
 *  security_setprocattr is a weird LSM on 5.4 and up, and this is normally backported
 *  down to 4.14 and 4.19. somehow this LSM is a one-shot. only the first to register
 *  is called.
 *
 *  however this is not an issue for us on 3.x as we are hijacking selinux_ops on it
 *
 */
#define SETPROCATTR_HOOK_NAME "ksu_setprocattr"
static struct security_hook_list ksu_hooks_setprocattr[] __ro_after_init = {
	LSM_HOOK_INIT(setprocattr, ksu_setprocattr_wrapper),
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int ksu_inode_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
			    struct inode *new_dir, struct dentry *new_dentry)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int ksu_inode_rename(struct user_namespace *mnt_userns, struct inode *old_dir, struct dentry *old_dentry,
			    struct inode *new_dir, struct dentry *new_dentry)
#else
static int ksu_inode_rename(struct inode *old_dir, struct dentry *old_dentry,
			    struct inode *new_dir, struct dentry *new_dentry)
#endif
{
	// skip kernel threads
	if (!current->mm) {
		return 0;
	}

	// skip non system uid
	if (current_uid().val != 1000) {
		return 0;
	}

	if (!old_dentry || !new_dentry) {
		return 0;
	}

	// Use d_name.name instead of the dangerous d_iname 
	// which can cause OOPS when the dentry is in an inconsistent state during rename
	if (strcmp(new_dentry->d_name.name, "packages.list")) {
		return 0;
	}

	char path[128];
	char *buf = dentry_path_raw(new_dentry, path, sizeof(path));
	if (IS_ERR(buf)) {
		pr_err("dentry_path_raw failed.\n");
		return 0;
	}

	if (!strstr(buf, "/system/packages.list")) {
		return 0;
	}

	// Do not track anything until the system has fully booted.
	// Parsing files during early boot from an LSM hook can causes VFS deadlocks
	if (!ksu_boot_completed) {
		return 0;
	}

	pr_debug("renameat: %s -> %s, new path: %s\n", old_dentry->d_name.name,
		new_dentry->d_name.name, buf);

	// Thread-safe execution using atomic operations to prevent race conditions
	// if system_server threads execute this hook concurrently.
	static atomic_t first_time = ATOMIC_INIT(1);

	// atomic_xchg swaps the value to 0 and returns the old value.
	// If the old value was 1, we are the first thread to reach here.
	if (atomic_xchg(&first_time, 0) == 1) {
		track_throne(true);
	} else {
		track_throne(false);
	}

	return 0;
}

static int ksu_task_fix_setuid(struct cred *new, const struct cred *old,
			       int flags)
{
	kuid_t new_uid = new->uid;
	kuid_t new_euid = new->euid;

	return ksu_handle_setresuid((uid_t)new_uid.val, (uid_t)new_euid.val,
				    (uid_t)new_uid.val);
}

#ifndef DEVPTS_SUPER_MAGIC
#define DEVPTS_SUPER_MAGIC	0x1cd1
#endif

//extern int __ksu_handle_devpts(struct inode *inode); // sucompat.c

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
int ksu_inode_permission(struct mnt_idmap *idmap, struct inode *inode, int mask)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
int ksu_inode_permission(struct user_namespace *mnt_userns, struct inode *inode, int mask)
#else
int ksu_inode_permission(struct inode *inode, int mask)
#endif
{
	if (unlikely(inode && inode->i_sb && inode->i_sb->s_magic == DEVPTS_SUPER_MAGIC)) {
		//__ksu_handle_devpts(inode);
	}
	return 0;
}

static struct security_hook_list ksu_hooks[] = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) ||                           \
	defined(CONFIG_IS_HW_HISI) || defined(CONFIG_KSU_ALLOWLIST_WORKAROUND)
	LSM_HOOK_INIT(key_permission, ksu_key_permission),
#endif
	LSM_HOOK_INIT(inode_permission, ksu_inode_permission),
	LSM_HOOK_INIT(inode_rename, ksu_inode_rename),
	LSM_HOOK_INIT(task_fix_setuid, ksu_task_fix_setuid)
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
static const struct lsm_id ksu_lsmid = {
	.name = "ksu",
	.id = 912,
};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 17, 0) || defined(KSU_COMPAT_SECURITY_DELETE_HOOKS_HLIST)
static void ksu_hlist_del_safe(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;

	if (!pprev)
		return;

	// this is here so we don't get lost
	/**
	 *	original state
	 * n			ptr	*ptr
	 * H	hlist_head	0x1000	0xA000
	 *
	 * A	node->next	0xA000	0xB000
	 *	node->pprev	0xA008	0x1000
	 *
	 * B	node->next	0xB000	0xC000
	 *	node->pprev	0xB008	0xA000
	 *
	 * C	node->next	0xC000	0xFFFF
	 *	node->pprev	0xC008	0xB000
	 *
	 */

	// on hlist, pprev is the address of the 'next' pointer in the previous element
	// so what we do is:
	// 	write the value 0xC000 (next) into address 0xA000 (A->next)
	// 	write the value 0xA000 (pprev) into address 0xC008 (C->pprev)

	/**
	 * 	after this routine
	 *
	 * H	hlist_head	0x1000	0xA000
	 *
	 * A	node->next	0xA000	0xC000  <-- now points to C
	 *	node->pprev	0xA008	0x1000
	 *
	 * B	node->next	0xB000	0xC000  <-- orphaned
	 *	node->pprev	0xB008	0xA000  <-- orphaned
	 *
	 * C	node->next	0xC000	0xFFFF
	 *	node->pprev	0xC008	0xA000  <-- now points to A's next
	 *
	 */

	// NOTE: pprev is **
	uintptr_t addr = (uintptr_t)pprev;
	uintptr_t base = addr & PAGE_MASK;
	uintptr_t offset = addr & ~PAGE_MASK;

	struct page *page = phys_to_page(__pa(base));
	if (!page)
		return;

	// vmap pprev
	void *writable_addr = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
	if (!writable_addr)
		return;

	uintptr_t target_slot = (uintptr_t)((uintptr_t)writable_addr + offset);

	preempt_disable();
	local_irq_disable();

	WRITE_ONCE(*(struct hlist_node **)target_slot, next);

	local_irq_enable();
	preempt_enable();

	vunmap(writable_addr);

	smp_mb();

	if (!next)
		return;

	// NOTE: pprev is **, taking ref, it becomes ***
	addr = (uintptr_t)&next->pprev;
	base = addr & PAGE_MASK;
	offset = addr & ~PAGE_MASK;

	page = phys_to_page(__pa(base));
	if (!page)
		return;

	writable_addr = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
	if (!writable_addr)
		return;

	target_slot = (uintptr_t)((uintptr_t)writable_addr + offset);

	preempt_disable();
	local_irq_disable();

	// use our pprev as the new pprev for the next in chain
	WRITE_ONCE(*(struct hlist_node ***)target_slot, pprev);

	local_irq_enable();
	preempt_enable();

	vunmap(writable_addr);

	smp_mb();
}

static void ksu_dethrone_selinux_setprocattr()
{
	struct hlist_head *head = ksu_hooks_setprocattr[0].head; 
	struct security_hook_list *pos;
	struct hlist_node *tmp;

	if (!head)
		return;

	hlist_for_each_entry_safe(pos, tmp, head, list) {

		// grab selinux_setprocattr fn ptr
		if (!strcmp(pos->lsm, "selinux")) {
			selinux_setprocattr_fn = pos->hook.setprocattr;
			pr_info("ksu_setprocattr: selinux_setprocattr found at 0x%lx \n", (uintptr_t)selinux_setprocattr_fn);
		}

		// remove everything else that aint us.
		// NOTE: on some kernels BPF_LSM is enabled and it will also register setprocattr
		// so this has to be done!
		if (!!strcmp(pos->lsm, SETPROCATTR_HOOK_NAME)) {
			pr_info("ksu_setprocattr: delete setprocattr LSM: %s\n", pos->lsm);
			ksu_hlist_del_safe(&pos->list);
		}
	}
}

#else

static void ksu_list_del_safe(struct list_head *entry)
{
	struct list_head *next = entry->next;
	struct list_head *prev = entry->prev;

	// on a linked list we have to patch both the before us and the next to us
	if (!prev)
		return;

	// smash prev->next, basically we write 'next' into 'prev->next'
	unsigned long addr_p = (unsigned long)&prev->next;
	unsigned long base_p = addr_p & PAGE_MASK;
	unsigned long offset_p = addr_p & ~PAGE_MASK;

	struct page *page_p = phys_to_page(__pa(base_p));
	if (!page_p)
		return;

	void *w_page = vmap(&page_p, 1, VM_MAP, PAGE_KERNEL);
	if (!w_page)
		return;

	struct list_head **target = (void *)((unsigned long)w_page + offset_p);
	
	preempt_disable();
	local_irq_disable();

	WRITE_ONCE(*target, next);

	local_irq_enable();
	preempt_enable();

	vunmap(w_page);
	
	smp_mb();

	if (!next)
		return;

	// smash next->prev, basically we need to write 'prev' into 'next->prev'
	unsigned long addr_n = (unsigned long)&next->prev;
	unsigned long base_n = addr_n & PAGE_MASK;
	unsigned long offset_n = addr_n & ~PAGE_MASK;

	struct page *page_n = phys_to_page(__pa(base_n));
	if (!page_n)
		return;

	w_page = vmap(&page_n, 1, VM_MAP, PAGE_KERNEL);
	if (!w_page)
		return;
	
	target = (void *)((unsigned long)w_page + offset_n);

	preempt_disable();
	local_irq_disable();

	WRITE_ONCE(*target, prev);

	local_irq_enable();
	preempt_enable();

	vunmap(w_page);

	smp_mb();

}

static void ksu_dethrone_selinux_setprocattr()
{
	struct list_head *head = ksu_hooks_setprocattr[0].head;
	struct security_hook_list *pos, *tmp;

	if (!head)
		return;

	if (list_empty(head))
		return;

	list_for_each_entry_safe(pos, tmp, head, list) {
		// dont unhook ourself!
		if (pos->hook.setprocattr == ksu_setprocattr_wrapper)
			continue;

		// this is likely selinux_setprocattr, we save its address
		if (!selinux_setprocattr_fn && pos->hook.setprocattr) {
			selinux_setprocattr_fn = pos->hook.setprocattr;
			pr_info("ksu_setprocattr: found first setprocattr at 0x%lx\n", (uintptr_t)selinux_setprocattr_fn);
		}

		// just delete evrything
		pr_info("ksu_setprocattr: delete setprocattr LSM at 0x%lx\n", (uintptr_t)pos->hook.setprocattr);
		ksu_list_del_safe(&pos->list);
	}
}
#endif

void __init ksu_lsm_hook_init(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	ksu_security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), &ksu_lsmid);
#else
	ksu_security_add_hooks(ksu_hooks, ARRAY_SIZE(ksu_hooks), "ksu");
#endif
	pr_info("LSM hooks initialized.\n");
	ksu_security_add_hooks(ksu_hooks_setprocattr, ARRAY_SIZE(ksu_hooks_setprocattr), SETPROCATTR_HOOK_NAME);
	ksu_dethrone_selinux_setprocattr();
	pr_info("setprocattr (SELinux Hide) hooks initialized.\n");
}
#else
void __init ksu_lsm_hook_init(void)
{
	return;
}
#endif
