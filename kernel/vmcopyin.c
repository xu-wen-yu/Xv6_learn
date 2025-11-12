#include "param.h"
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"

//
// 本文件包含 copyin_new() 和 copyinstr_new()
// 它们是 vm.c 中 copyin 和 copyinstr 的替代实现。
//

static struct stats {
  int ncopyin;
  int ncopyinstr;
} stats;

int statscopyin(char *buf, int sz) {
  int n;
  n = snprintf(buf, sz, "copyin: %d\n", stats.ncopyin);
  n += snprintf(buf + n, sz, "copyinstr: %d\n", stats.ncopyinstr);
  return n;
}

// 从用户空间复制到内核空间。
// 在给定的页表中，从虚拟地址 srcva 复制 len 字节到 dst。
// 成功返回 0，出错返回 -1。
int copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  struct proc *p = myproc();

  if (srcva >= p->sz || srcva + len >= p->sz || srcva + len < srcva) return -1;
  memmove((void *)dst, (void *)srcva, len);
  stats.ncopyin++;  // XXX: 这里应加锁
  return 0;
}

// 从用户空间复制以 NUL 结尾的字符串到内核空间。
// 在给定的页表中，从虚拟地址 srcva 复制字节到 dst，
// 直到遇到 '\0'，或达到最大长度 max。
// 成功返回 0，出错返回 -1。
int copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  struct proc *p = myproc();
  char *s = (char *)srcva;

  stats.ncopyinstr++;  // XXX: 这里应加锁
  for (int i = 0; i < max && srcva + i < p->sz; i++) {
    dst[i] = s[i];
    if (s[i] == '\0') return 0;
  }
  return -1;
}
