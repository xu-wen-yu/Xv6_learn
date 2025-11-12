#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void wakeup1(struct proc *chan);
static void freeproc(struct proc *p);

extern char etext[];  // kernel.ld 将其设置为内核代码结束位置。

extern char trampoline[];  // trampoline.S
extern pagetable_t kernel_pagetable;

// 在引导时初始化进程表。
void procinit(void) {
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");

    // 为进程的内核栈分配一页。
    // 将其映射到高地址处，后面跟随一个无效的“保护页”。
    char *pa = kalloc();
    if (pa == 0) panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    p->kstack = va;
    p->kstack_pa = (uint64)pa;//此处使用内核栈的物理地址（Physical Address），用于建立映射
  }
  kvminithart();//此处不必考虑p->k_pagetable的映射，因为此时还没有进程
}

// 必须在关中断情况下调用，
// 以避免与进程被迁移到其他 CPU 的竞争。
int cpuid() {
  int id = r_tp();
  return id;
}

// 返回当前 CPU 的 cpu 结构。
// 必须在关中断情况下调用。
struct cpu *mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// 返回当前进程的 struct proc*，若无则返回 0。
struct proc *myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid() {
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// 在进程表中查找一个 UNUSED 的进程槽。
// 找到后初始化其在内核中运行所需的状态，并在持有 p->lock 的情况下返回。
// 若没有空闲进程，或内存分配失败，则返回 0。
static struct proc *allocproc(void) {
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();

  // 为 trapframe 分配一页。
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    release(&p->lock);
    return 0;
  }

  // 创建一张空的用户页表。
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 调用函数创建每进程独立的内核页表
  p->k_pagetable = (pagetable_t)kalloc();
  kvmindependentinit(p->k_pagetable);

  // 在进程 p 的独立内核页表 p->k_pagetable 中，将虚拟地址 p->kstack 映射到物理地址 p->kstack_pa
  mappages(p->k_pagetable, p->kstack, PGSIZE, p->kstack_pa, PTE_W | PTE_R);  // 注意虚拟地址为p->kstack = va，使用mappages而不是kvmmap，因为kvmmap只适用于全局内核页表

  sync_pagetable(p->pagetable, p->k_pagetable); // 建立用户-内核映射
  // 设置新的上下文，从 forkret 开始执行；
  // forkret 最终会返回到用户态。
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// 释放一个进程结构及其附属数据（包括用户页）。
// 必须持有 p->lock。
static void freeproc(struct proc *p) {
  if (p->trapframe) kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable) proc_freepagetable(p->pagetable, p->sz);
  if (p->k_pagetable) kvmfree(p->k_pagetable);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// 为给定进程创建用户页表，
// 其中不包含用户内存，但包含 trampoline 相关映射。
pagetable_t proc_pagetable(struct proc *p) {
  pagetable_t pagetable;

  // 一张空页表。
  pagetable = uvmcreate();
  if (pagetable == 0) return 0;

  // 将 trampoline 代码（用于系统调用返回路径）
  // 映射到最高的用户虚拟地址处。
  // 仅内核在进出用户态时使用，因此不设置 PTE_U。
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // 为 trampoline.S 将 trapframe 映射到 TRAMPOLINE 之下。
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe), PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// 释放进程的页表，并释放其引用的物理内存。
void proc_freepagetable(pagetable_t pagetable, uint64 sz) {
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// 一个会调用 exec("/init") 的用户程序
// 可用 od -t xC initcode 查看字节
uchar initcode[] = {0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02, 0x97, 0x05, 0x00, 0x00, 0x93,
                    0x85, 0x35, 0x02, 0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00, 0x93, 0x08,
                    0x20, 0x00, 0x73, 0x00, 0x00, 0x00, 0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e,
                    0x69, 0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// 创建第一个用户进程。
void userinit(void) {
  struct proc *p;

  p = allocproc();
  initproc = p;

  // 分配一页用户内存，并拷贝 init 的指令与数据。
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // 为首次从内核返回到用户态做准备。
  p->trapframe->epc = 0;      // 用户程序计数器
  p->trapframe->sp = PGSIZE;  // 用户栈指针

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  sync_pagetable(p->pagetable, p->k_pagetable); // 同步用户-内核映射

  release(&p->lock);
}

// 将用户内存增长或缩小 n 字节。
// 成功返回 0，否则返回 -1。
int growproc(int n) {
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  sync_pagetable(p->pagetable, p->k_pagetable); // 同步用户-内核映射
  return 0;
}

// 通过复制父进程创建一个新进程。
// 设置子进程的内核栈，使其看起来像从 fork() 系统调用返回。
int fork(void) {
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // 分配进程结构。
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // 将父进程的用户内存复制到子进程。
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  sync_pagetable(np->pagetable, np->k_pagetable);// 同步用户-内核映射,不能放在freeproc前，否则报错panic: kerneltrap，因为if里执行的是error后的操作

  np->parent = p;

  // 复制已保存的用户寄存器。
  *(np->trapframe) = *(p->trapframe);

  // 使 fork 在子进程中返回 0。
  np->trapframe->a0 = 0;

  // 增加所有已打开文件描述符的引用计数。
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i]) np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}

// 将进程 p 遗留的子进程交给 init（托孤）。
// 调用者必须持有 p->lock。
void reparent(struct proc *p) {
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    // 这里在未持有 pp->lock 的情况下使用了 pp->parent。
    // 若先获取锁，可能导致死锁：当 pp 或其子进程也在 exit()
    // 并准备去获取 p 的锁时，会互相等待。
    if (pp->parent == p) {
      // 在检查与加锁之间，pp->parent 不会变化，
      // 因为只有父进程会修改它，而此处我们就是父进程。
      acquire(&pp->lock);
      pp->parent = initproc;
      // 理论上此处应该唤醒 init，但这需要持有 initproc->lock，
      // 而我们当前正持有 init 的一个子进程（pp）的锁，会导致死锁。
      // 因此 exit() 总是在获取任何锁之前就唤醒 init。
      release(&pp->lock);
    }
  }
}

// 退出当前进程。本函数不返回。
// 退出的进程会保持在僵尸状态，直到其父进程调用 wait()。
void exit(int status) {
  struct proc *p = myproc();

  if (p == initproc) panic("init exiting");

  // 关闭所有已打开的文件。
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  // 可能会把子进程托孤给 init。但一旦我们已经获取了其他进程锁，
  // 就无法再获取 init 的锁来精确唤醒它。因此无论是否必要，都唤醒 init。
  // 即便 init 错过这次唤醒也无妨。
  acquire(&initproc->lock);
  wakeup1(initproc);
  release(&initproc->lock);

  // 复制一份 p->parent，以确保解锁的是同一个父进程。
  // 这样即使我们在等待父进程锁期间被改为由 init 作为父亲，也不会解错锁。
  // 可能与正在退出的父进程发生竞态，但最多导致一次无害的虚假唤醒；
  // proc 结构不会被重用为其他类型的数据。
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);

  // 唤醒在 wait() 中的父进程需要持有父进程的锁。
  // 遵循“先父后子”的加锁顺序，因此先锁父进程。
  acquire(&original_parent->lock);

  acquire(&p->lock);

  // 将所有子进程托孤给 init。
  reparent(p);

  // 父进程可能正睡在 wait() 中。
  wakeup1(original_parent);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&original_parent->lock);

  // 跳转到调度器，不再返回。
  sched();
  panic("zombie exit");
}

// 等待任一子进程退出并返回其 pid。
// 如果当前进程没有子进程，则返回 -1。
int wait(uint64 addr) {
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  // 全程持有 p->lock，避免遗漏子进程 exit() 的唤醒。
  acquire(&p->lock);

  for (;;) {
    // 扫描进程表，寻找已退出的子进程。
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++) {
      // 这里在未持有 np->lock 的情况下访问 np->parent。
      // 若先获取 np->lock 可能导致死锁，
      // 因为 np 可能是 p 的祖先，而我们已持有 p->lock。
      if (np->parent == p) {
        // 在检查与加锁之间，np->parent 不会变化，
        // 因为只有父进程会修改它，而此处我们就是父进程。
        acquire(&np->lock);
        havekids = 1;
        if (np->state == ZOMBIE) {
          // 找到一个。
          pid = np->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate, sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&p->lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&p->lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // 没有子进程或被杀死，则无须等待。
    if (!havekids || p->killed) {
      release(&p->lock);
      return -1;
    }

    // 等待某个子进程退出。
    sleep(p, &p->lock);  // DOC: wait-sleep
  }
}

// 每 CPU 的进程调度器。
// 每个 CPU 在自身初始化后都会调用 scheduler()。
// 调度器不返回；其循环执行：
//  - 选择一个进程运行；
//  - swtch 切换去运行该进程；
//  - 该进程最终通过 swtch 把控制权交还给调度器。
void scheduler(void) {
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // 允许设备中断，避免死锁。
    intr_on();

    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // 切换到选中的进程。该进程负责在切回调度器前
        // 释放其锁并在适当时机重新获取。
        w_satp(MAKE_SATP(p->k_pagetable));
        sfence_vma();
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // 该进程暂时运行完毕。
        // 在切回之前它应该已经修改过 p->state。
        c->proc = 0;

        // 恢复全局内核页表，保证调度器运行在共享内核地址空间中。
        w_satp(MAKE_SATP(kernel_pagetable));
        sfence_vma();// 不加这两行所有测试正确?而且不能使用p->pagetable

        found = 1;
      }
      release(&p->lock);
    }
#if !defined(LAB_FS)
    if (found == 0) {
      intr_on();
      asm volatile("wfi");
    }
#else
    ;
#endif
  }
}

// 切换到调度器。调用时必须仅持有 p->lock，
// 且已修改 proc->state。保存并恢复 intena，
// 因为 intena 属于当前内核线程而非 CPU。
// 理想情况应为 proc->intena 和 proc->noff，
// 但在极少数持锁且无进程的场景中会破坏该假设。
void sched(void) {
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock)) panic("sched p->lock");
  if (mycpu()->noff != 1) panic("sched locks");
  if (p->state == RUNNING) panic("sched running");
  if (intr_get()) panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// 主动让出 CPU 一个调度轮次。
void yield(void) {
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// 子进程在被调度器首次调度运行时，
// 会通过 swtch 跳转到 forkret。
void forkret(void) {
  static int first = 1;

  // 仍然持有来自 scheduler 的 p->lock。
  release(&myproc()->lock);

  if (first) {
    // 文件系统初始化必须在一个常规进程上下文中执行
    // （例如会调用 sleep），因此不能在 main() 中执行。
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// 原子地释放锁并在 chan 上睡眠。
// 被唤醒时会重新获取该锁。
void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();

  // 必须先获取 p->lock 才能修改 p->state 并调用 sched。
  // 一旦持有 p->lock，我们就不会错过任何唤醒
  // （wakeup 会获取 p->lock），
  // 因此可以放心释放传入的 lk。
  if (lk != &p->lock) {  // DOC: sleeplock0
    acquire(&p->lock);   // DOC: sleeplock1
    release(lk);
  }

  // 进入睡眠。
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // 清理现场。
  p->chan = 0;

  // 重新获取原始的锁。
  if (lk != &p->lock) {
    release(&p->lock);
    acquire(lk);
  }
}

// 唤醒所有在 chan 上睡眠的进程。
// 调用时不能持有任何 p->lock。
void wakeup(void *chan) {
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
    }
    release(&p->lock);
  }
}

// 如果进程 p 在 wait() 中睡眠，则唤醒它；由 exit() 使用。
// 调用者必须持有 p->lock。
static void wakeup1(struct proc *p) {
  if (!holding(&p->lock)) panic("wakeup1");
  if (p->chan == p && p->state == SLEEPING) {
    p->state = RUNNABLE;
  }
}

// 杀死给定 pid 的进程。
// 被杀进程只有在尝试返回用户态时才会真正退出（见 trap.c 的 usertrap()）。
int kill(int pid) {
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // 从 sleep() 中唤醒该进程。
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// 依据 usr_dst，拷贝到用户地址或内核地址。
// 成功返回 0，出错返回 -1。
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len) {
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// 依据 usr_src，从用户地址或内核地址拷贝。
// 成功返回 0，出错返回 -1。
int either_copyin(void *dst, int user_src, uint64 src, uint64 len) {
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// 在控制台打印进程列表（调试用）。
// 当用户在控制台输入 ^P 时运行。
// 不加锁，以避免进一步卡死系统。
void procdump(void) {
  static char *states[] = {
      [UNUSED] "unused", [SLEEPING] "sleep ", [RUNNABLE] "runble", [RUNNING] "run   ", [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED) continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
