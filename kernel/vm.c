#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * 内核的页表。
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld 将其设置为内核代码的结束地址。

extern char trampoline[];  // trampoline.S 文件

/*
 * 为内核创建一个直接映射（direct-map）的页表。
 */
void kvminit() {
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // UART 寄存器
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio 的 MMIO 磁盘接口
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT（Core-Local Interruptor，本地中断控制器）
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC（Platform-Level Interrupt Controller，平台中断控制器）
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // 将内核文本段映射为可执行且只读。
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // 映射内核数据段以及我们将使用的物理内存。
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // 将 trampoline 映射到内核的最高虚拟地址，供陷阱进入/退出使用。
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// 将硬件的页表寄存器切换到内核页表，并刷新/启用分页相关缓存。
void kvminithart() {
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// 返回页表 pagetable 中对应虚拟地址 va 的 PTE 的地址。
// 如果 alloc != 0，则在需要时创建页表页。
//
// RISC-V 的 Sv39 机制有三级页表页（3-level page table）。一个页表页包含 512 个 64 位的 PTE。
// 一个 64 位虚拟地址分为五个字段：
//   39..63 -- 必须为 0。
//   30..38 -- level-2 索引的 9 位。
//   21..29 -- level-1 索引的 9 位。
//   12..20 -- level-0 索引的 9 位。
//    0..11 -- 页内偏移的 12 位。
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA) panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// 查找一个虚拟地址，返回对应的物理地址；如果没有映射则返回 0。
// 仅可用于查找用户页。
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA) return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0) return 0;
  if ((*pte & PTE_V) == 0) return 0;
  if ((*pte & PTE_U) == 0) return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// 向内核页表添加一条映射。
// 仅在引导阶段使用。
// 不会刷新 TLB 或启用分页（只在页表构建阶段使用）。
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) {
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0) panic("kvmmap");
}

// 将内核虚拟地址转换为物理地址。仅用于栈上的地址。
// 假定 va 已按页对齐。
uint64 kvmpa(uint64 va) {
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernel_pagetable, va, 0);
  if (pte == 0) panic("kvmpa");
  if ((*pte & PTE_V) == 0) panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// 为从 va 开始的虚拟地址创建 PTE，使其映射到从 pa 开始的物理地址。
// va 和 size 不一定是页对齐的。
// 成功返回 0；如果 walk() 无法分配所需的页表页则返回 -1。
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0) return -1;
    if (*pte & PTE_V) panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last) break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// 从 va 开始移除 npages 个映射。va 必须按页对齐。映射必须存在。
// 可选地释放对应的物理内存（do_free 为真时释放）。
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0) panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V) panic("uvmunmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// 创建一个空的用户页表。
// 如果内存不足则返回 0。
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0) return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// 将用户初始化代码（initcode）加载到 pagetable 的地址 0，供第一个进程使用。
// sz 必须小于一页。
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  char *mem;

  if (sz >= PGSIZE) panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// 为进程从 oldsz 增长到 newsz 分配 PTE 和物理内存，newsz 不必为页对齐。
// 成功返回新的大小；错误返回 0。
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  char *mem;
  uint64 a;

  if (newsz < oldsz) return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// 解除用户页，将进程大小从 oldsz 缩减到 newsz。
// oldsz 和 newsz 不需要页对齐，newsz 也可以大于 oldsz。
// oldsz 可以大于实际的进程大小。返回新的进程大小。
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// 递归释放页表页。所有叶映射必须已被移除。
void freewalk(pagetable_t pagetable) {
  // 一个页表页中有 2^9 = 512 个 PTE。
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // 这个 PTE 指向下一级页表。
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// 释放用户内存页，然后释放页表页。
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// 给定父进程的页表，将其内存复制到子进程的页表中。
// 复制页表结构和物理内存内容。
// 成功返回 0，失败返回 -1。失败时会释放已分配的页面。
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0) panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0) goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// 将某个 PTE 标记为对用户不可访问。
// 在 exec 中用于用户栈的守护页（guard page）。
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

// 从内核向用户复制数据。
// 在给定页表中，将 src 的 len 字节复制到目标虚拟地址 dstva。
// 成功返回 0，失败返回 -1。
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// 从用户向内核复制数据。
// 在给定页表中，将 srcva 指向的 len 字节复制到 dst。
// 成功返回 0，失败返回 -1。
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (srcva - va0);
    if (n > len) n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// 从用户空间复制一个以 '\0' 结尾的字符串到内核。
// 在给定页表中，从 srcva 指向的虚拟地址拷贝字节到 dst，直到遇到 '\0' 或达到 max 为止。
// 成功返回 0，失败返回 -1。
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max) n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}
