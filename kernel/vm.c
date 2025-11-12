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

extern char etext[];  // kernel.ld 将其设置为内核代码结束位置。

extern char trampoline[];  // 来自 trampoline.S

/*
 * 为内核创建一张直接映射（direct-map）的页表。
 */
void kvminit() {
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // UART 寄存器
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio MMIO 磁盘接口
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // 将内核文本段映射为“可执行+只读”。
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // 映射内核数据段以及我们将使用的物理内存。
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // 将用作陷入/返回跳板的 trampoline 映射到
  // 内核最高的虚拟地址处。
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

void kvmindependentinit(pagetable_t independent_kernel_pagetable) {
  memset(independent_kernel_pagetable, 0, PGSIZE);
  // 设备 MMIO
  mappages(independent_kernel_pagetable, UART0, PGSIZE, UART0, PTE_R | PTE_W);
  mappages(independent_kernel_pagetable, VIRTIO0, PGSIZE, VIRTIO0, PTE_R | PTE_W);
  mappages(independent_kernel_pagetable, PLIC, 0x400000, PLIC, PTE_R | PTE_W);
  // 内核文本段（RX）与数据段（RW）
  mappages(independent_kernel_pagetable, KERNBASE, (uint64)etext - KERNBASE, KERNBASE, PTE_R | PTE_X);
  mappages(independent_kernel_pagetable, (uint64)etext, PHYSTOP - (uint64)etext, (uint64)etext, PTE_R | PTE_W);
  // trampoline
  mappages(independent_kernel_pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X);
}

void sync_pagetable(pagetable_t src, pagetable_t dst) {//dst是内核，src是用户
  // 一个页表页包含512个PTE（2^9 = 512）
  // 遍历当前页表页中的所有PTE表项
  for (int i = 0; i < 512; i++) {
    pte_t pte = src[i];
    /* 检查当前PTE是否指向下一级页表（非叶子节点）：
       - PTE_V: 页表项有效
       - (PTE_R|PTE_W|PTE_X) == 0: 不具有R/W/X权限，说明不是叶子映射
       满足这两个条件说明该PTE指向的是下一级页表页 */
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // 这个PTE指向下一级页表
      uint64 child = PTE2PA(pte);  // 从PTE中提取子页表的物理地址
      pagetable_t child_src = (pagetable_t)child;
      pagetable_t child_dst;
      if ((dst[i] & PTE_V) == 0) {// 内核子页表页不存在
        child_dst = (pagetable_t)kalloc();
        if (child_dst == 0) panic("sync_pagetable: kalloc");//内存分配失败
        memset(child_dst, 0, PGSIZE);
        dst[i] = PA2PTE((uint64)child_dst) | PTE_V;// 在内核页表中创建新的子页表页映射
      } else {
        if ((dst[i] & (PTE_R | PTE_W | PTE_X)) != 0) panic("sync_pagetable: dst leaf");
        child_dst = (pagetable_t)PTE2PA(dst[i]);// 获取内核子页表页地址
      }
      sync_pagetable(child_src, child_dst);  // 递归同步子页表
    } else if (pte & PTE_V) {        // 如果PTE有效且具有R/W/X权限（叶子节点）
      // 直接从目的用户页表复制该PTE到源内核页表
      dst[i] = pte & ~PTE_U;
      // 设置PTE_U，只有内核才能访问的页
    } else {
      // 源侧无映射，保留内核已有的内核态映射
      continue;  // 不能直接dst[i]=0，因为可能覆盖内核已有映射
    }
    // 如果PTE无效（PTE_V=0），则直接跳过
  }
}

// 将硬件页表寄存器切换为内核页表，并开启分页。
void kvminithart() {
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// 返回页表 pagetable 中与虚拟地址 va 对应的 PTE 页表地址。
// 如果 alloc!=0，则按需创建缺失的页表页。
//
// RISC-V Sv39 分页方案包含三级页表页。
// 一张页表页包含 512 个 64 位 PTE。
// 64 位虚拟地址被分成五个字段：
//   39..63 -- 必须为 0。
//   30..38 -- 9 位二级索引（level-2）。
//   21..29 -- 9 位一级索引（level-1）。
//   12..20 -- 9 位零级索引（level-0）。
//    0..11 -- 页内 12 位字节偏移。
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

// 查找虚拟地址，返回物理地址；若未映射则返回 0。
// 只能用于查询用户页。
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
// 不会刷新 TLB（缓存映射关系），也不会启用分页。
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) {
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0) panic("kvmmap");
}

// 将内核虚拟地址转换为物理地址。
// 仅对栈上的地址需要该功能。
// 假设 va 已按页对齐。
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

// 为从 va 开始的一段虚拟地址区间创建 PTE，使其映射到
// 从 pa 开始的一段物理地址。va 与 size 可能未按页对齐。
// 成功返回 0；若 walk() 无法分配必须的页表页则返回 -1。
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

// 从 va 开始移除 npages 个页的映射。va 必须按页对齐。
// 这些映射必须已存在。
// 可选地释放相应的物理内存。
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

// 创建一张空的用户页表。
// 若内存不足则返回 0。
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0) return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// 将用户态的 initcode 加载到页表的地址 0，
// 用于系统中的第一个进程。
// sz 必须小于一页。
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  char *mem;

  if (sz >= PGSIZE) panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// 分配 PTE 和物理内存，将进程大小从 oldsz 扩展到 newsz。
// 两者不必按页对齐。成功返回新大小，否则返回 0。
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

// 释放用户页，使进程大小从 oldsz 收缩到 newsz。
// oldsz 与 newsz 不必按页对齐，newsz 也不必小于 oldsz。
// oldsz 可以大于实际进程大小。返回新的进程大小。
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// 递归释放页表页
// 注意：所有叶子映射（指向实际物理页的映射）必须在此前已被移除
void freewalk(pagetable_t pagetable) {
  // 一个页表页包含512个PTE（2^9 = 512）
  // 遍历当前页表页中的所有PTE表项
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];  // 获取第i条PTE
    /* 检查当前PTE是否指向下一级页表（非叶子节点）：
       - PTE_V: 页表项有效
       - (PTE_R|PTE_W|PTE_X) == 0: 不具有R/W/X权限，说明不是叶子映射
       满足这两个条件说明该PTE指向的是下一级页表页 */
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // 这个PTE指向下一级页表
      uint64 child = PTE2PA(pte);    // 从PTE中提取子页表的物理地址
      freewalk((pagetable_t)child);  // 递归释放子页表
      pagetable[i] = 0;              // 将当前PTE清零，标记为空闲
    } else if (pte & PTE_V) {        // 如果PTE有效且具有R/W/X权限（叶子节点）
      /* 发现仍然存在的叶子映射，报错！
         因为在调用freewalk之前，应该先通过uvmunmap等函数解除所有叶子映射 */
      panic("freewalk: leaf");
    }
    // 如果PTE无效（PTE_V=0），则直接跳过
  }
  // 当前页表页的所有PTE已处理完毕，释放该页表页占用的物理内存
  kfree((void *)pagetable);
}

// 先释放用户内存页，
// 再释放页表页。
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

void kvmfree(pagetable_t pagetable) {
  // 一个页表页包含512个PTE（2^9 = 512）
  // 遍历当前页表页中的所有PTE表项
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];  // 获取第i条PTE
    /* 检查当前PTE是否指向下一级页表（非叶子节点）：
       - PTE_V: 页表项有效
       - (PTE_R|PTE_W|PTE_X) == 0: 不具有R/W/X权限，说明不是叶子映射
       满足这两个条件说明该PTE指向的是下一级页表页 */
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // 这个PTE指向下一级页表
      uint64 child = PTE2PA(pte);   // 从PTE中提取子页表的物理地址
      kvmfree((pagetable_t)child);  // 递归释放子页表
      pagetable[i] = 0;             // 将当前PTE清零，标记为空闲
    } else if (pte & PTE_V) {       // 如果PTE有效且具有R/W/X权限（叶子节点）
      // 对于内核页表，叶子映射指向共享的物理页（内核代码、数据、设备等）
      // 这些物理页不应被释放，只需清除映射即可
      pagetable[i] = 0;
    }
    // 如果PTE无效（PTE_V=0），则直接跳过
  }
  // 当前页表页的所有PTE已处理完毕，释放页表页本身占用的物理内存
  kfree((void *)pagetable);//不进行kfree会fork失败，因为子进程的页表页没有被释放，这些不是物理页，而是页表页
}

// 给定父进程的页表，将其内存复制到子进程的页表中。
// 既复制页表也复制物理内存。
// 成功返回 0，失败返回 -1。
// 失败时释放已分配的页面。
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

// 将某个 PTE 标记为用户不可访问。
// 由 exec 用于用户栈的保护页。
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

// 从内核拷贝到用户。
// 将长度为 len 的数据从 src 拷贝到给定页表中的用户虚拟地址 dstva。
// 成功返回 0，出错返回 -1。
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

// 从用户拷贝到内核。
// 将长度为 len 的数据从给定页表中的用户虚拟地址 srcva 拷贝到 dst。
// 成功返回 0，出错返回 -1。
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  w_sstatus(r_sstatus() | SSTATUS_SUM);
  return copyin_new(pagetable, dst, srcva, len);
  w_sstatus(r_sstatus() & ~SSTATUS_SUM);
}

// 从用户拷贝一个以空字符结尾的字符串到内核。
// 从给定页表中的用户虚拟地址 srcva 拷贝到 dst，
// 直到遇到 '\0' 或达到最大长度 max。
// 成功返回 0，出错返回 -1。
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  w_sstatus(r_sstatus() | SSTATUS_SUM);
  return copyinstr_new(pagetable, dst, srcva, max);
  w_sstatus(r_sstatus() & ~SSTATUS_SUM);//这里不设置前后这两条测试也能过？
}

// 检查当前是否使用全局内核页表（kpgtbl）
int test_pagetable() {
  uint64 satp = r_satp();
  uint64 gsatp = MAKE_SATP(kernel_pagetable);
  printf("test_pagetable: %d\n", satp != gsatp);
  return satp != gsatp;
}

void vmprint(pagetable_t pgtbl) {
  printf("page table %p\n", pgtbl);
  for (int i = 0; i < 512; i++) {
    pte_t pte = pgtbl[i];  // 获取第i条PTE
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      uint64 child1 = PTE2PA(pte);
      printf("||idx: %d: pa: %p, flags: %s%s%s%s\n", i,(void *)((uint64)child1 << PXSHIFT(1)),
             (pte & PTE_R) ? "r" : "-", (pte & PTE_W) ? "w" : "-", (pte & PTE_X) ? "x" : "-",
             (pte & PTE_U) ? "u" : "-");
      for (int j = 0; j < 512; j++) {
        pte_t child_pte = ((pagetable_t)child1)[j];
        if ((child_pte & PTE_V) && (child_pte & (PTE_R | PTE_W | PTE_X)) == 0) {
          uint64 child2 = PTE2PA(child_pte);
          printf("||  ||idx: %d: pa: %p, flags: %s%s%s%s\n", j, (void *)((uint64)child2 << PXSHIFT(2)),
                 (child_pte & PTE_R) ? "r" : "-", (child_pte & PTE_W) ? "w" : "-", (child_pte & PTE_X) ? "x" : "-",
                 (child_pte & PTE_U) ? "u" : "-");
          for (int k = 0; k < 512; k++) {
            pte_t child_pte2 = ((pagetable_t)child2)[k];
            if ((child_pte2 & PTE_V)) {
              printf("||  ||  ||idx: %d: va: %p -> pa: %p, flags: %s%s%s%s\n", k,
                     ((uint64)i << PXSHIFT(2)) | ((uint64)j << PXSHIFT(1)) | ((uint64)k << PXSHIFT(0)),
                     (void *)PTE2PA(child_pte2), (child_pte2 & PTE_R) ? "r" : "-", (child_pte2 & PTE_W) ? "w" : "-",
                     (child_pte2 & PTE_X) ? "x" : "-", (child_pte2 & PTE_U) ? "u" : "-");
            }
          }  // 使用三级索引组合：根层循环索引为 i，第二层为 j，第三层为 k,组合一个虚拟地址 VA（Sv39）：VA = (vpn2 <<
             // PXSHIFT(2)) | (vpn1 << PXSHIFT(1)) | (vpn0 << PXSHIFT(0)) | offset（页内偏移）其中 offset ∈ [0,
             // 1<<PGSHIFT)。打印页表时用 offset=0。
        }
      }
    }
  }
}