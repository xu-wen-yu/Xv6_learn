#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);

int exec(char *path, char **argv) {
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG + 1], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);

  // 检查 ELF 头
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) goto bad;
  if (elf.magic != ELF_MAGIC) goto bad;

  if ((pagetable = proc_pagetable(p)) == 0) goto bad;

  // 将程序加载到内存中。
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) goto bad;
    if (ph.type != ELF_PROG_LOAD) continue;
    if (ph.memsz < ph.filesz) goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr) goto bad;
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0) goto bad;
    sz = sz1;
    if (ph.vaddr % PGSIZE != 0) goto bad;
    if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // 在下一个页边界分配两页。
  // 使用第二页作为用户栈。
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if ((sz1 = uvmalloc(pagetable, sz, sz + 2 * PGSIZE)) == 0) goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - 2 * PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // 推入参数字符串，并在 ustack 中准备其余栈内容。
  for (argc = 0; argv[argc]; argc++) {
    if (argc >= MAXARG) goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;  // riscv sp must be 16-byte aligned
    if (sp < stackbase) goto bad;
    if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // 推入 argv[] 指针数组。
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase) goto bad;
  if (copyout(pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0) goto bad;

  // 传递给用户态 main(argc, argv) 的参数
  // argc 通过系统调用返回值传回，
  // 返回值放在寄存器 a0。
  p->trapframe->a1 = sp;

  // 保存程序名用于调试。
  for (last = s = path; *s; s++)
    if (*s == '/') last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // 提交新的用户镜像。
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // 初始程序计数器 = main
  p->trapframe->sp = sp;          // 初始栈指针
  sync_pagetable(p->pagetable, p->k_pagetable);//先同步，确保新页表映射在内核页表里可用；再释放旧页表，避免空窗期。
  proc_freepagetable(oldpagetable, oldsz);

  if(p->pid == 1)
    vmprint(p->pagetable);
  return argc;  // 该返回值最终进入 a0，作为 main(argc, argv) 的第一个参数

bad:
  if (pagetable) proc_freepagetable(pagetable, sz);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// 将程序段加载到页表 pagetable 的虚拟地址 va。
// va 必须按页对齐
// 且从 va 到 va+sz 的页面必须已经建立映射。
// 成功返回 0，失败返回 -1。
static int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz) {
  uint i, n;
  uint64 pa;

  if ((va % PGSIZE) != 0) panic("loadseg: va must be page aligned");  // va 必须按页对齐

  for (i = 0; i < sz; i += PGSIZE) {
    pa = walkaddr(pagetable, va + i);
    if (pa == 0) panic("loadseg: address should exist");
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, 0, (uint64)pa, offset + i, n) != n) return -1;
  }

  return 0;
}
