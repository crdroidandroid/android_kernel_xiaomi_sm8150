#ifndef _LINUX_SUSPICIOUS_H_
#define _LINUX_SUSPICIOUS_H_

#include <linux/fs.h>
#include <linux/mount.h>

#define getname_safe(name) (name == NULL ? ERR_PTR(-EINVAL) : getname(name))
#define putname_safe(name) (IS_ERR(name) ? NULL : putname(name))

int is_suspicious_path(const struct path* const file);
int is_suspicious_mount(struct vfsmount* const mnt, const struct path* const root);
int suspicious_path(const struct filename* const name);
#endif
