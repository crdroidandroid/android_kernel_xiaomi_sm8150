#ifndef __KSU_H_ADB_ROOT
#define __KSU_H_ADB_ROOT
#include <asm/ptrace.h>
#include "runtime/user_arg_ptr.h"

struct filename;

long ksu_adb_root_handle_execve(struct pt_regs *regs);
long ksu_adb_root_handle_execveat(struct pt_regs *regs);
long ksu_adb_root_handle_execve_filename(struct filename *filename,
					 struct user_arg_ptr *envp);

void ksu_adb_root_init(void);

void ksu_adb_root_exit(void);

#endif
