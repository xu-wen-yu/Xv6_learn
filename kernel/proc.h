// 内核上下文切换时保存的寄存器。
struct context {
  uint64 ra;
  uint64 sp;

  // 被调用者保存（callee-saved）
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// 每个 CPU 的状态。
struct cpu {
  struct proc *proc;       // 该 CPU 上正在运行的进程，或为空。
  struct context context;  // 调用 swtch() 切入调度器。
  int noff;                // push_off() 的嵌套深度。
  int intena;              // 在 push_off() 之前中断是否启用？
};

extern struct cpu cpus[NCPU];

// trampoline.S 中陷阱处理代码所需的每进程数据。
// 位于用户页表中紧邻 trampoline 页下方的独立一页中；
// 在内核页表中不会被特殊映射。
// sscratch 寄存器指向此处。
// trampoline.S 中的 uservec 将用户寄存器保存在 trapframe 中，
// 然后用 trapframe 中的 kernel_sp、kernel_hartid、kernel_satp
// 初始化寄存器，并跳转到 kernel_trap。
// trampoline.S 中的 usertrapret() 和 userret 负责设置
// trapframe 的 kernel_* 字段，从 trapframe 恢复用户寄存器，
// 切换到用户页表，并进入用户态。
// trapframe 包含 s0-s11 等被调用者保存的用户寄存器，
// 因为通过 usertrapret() 返回用户态不会经过整个内核调用栈。
struct trapframe {
  /*   0 */ uint64 kernel_satp;    // 内核页表
  /*   8 */ uint64 kernel_sp;      // 进程内核栈的栈顶
  /*  16 */ uint64 kernel_trap;    // usertrap()
  /*  24 */ uint64 epc;            // 保存的用户程序计数器
  /*  32 */ uint64 kernel_hartid;  // 保存的内核 tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate { UNUSED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// 每个进程的状态
struct proc {
  struct spinlock lock;

  // 使用以下字段时必须持有 p->lock：
  enum procstate state;  // 进程状态
  struct proc *parent;   // 父进程
  void *chan;            // 若非零，表示在此通道上睡眠
  int killed;            // 若非零，表示已被终止
  int xstate;            // 退出状态，将返回给父进程的 wait
  int pid;               // 进程 ID

  // 以下字段为进程私有，使用时无需持有 p->lock。
  uint64 kstack;                // 内核栈的虚拟地址
  uint64 sz;                    // 进程内存大小（字节）
  pagetable_t pagetable;        // 用户页表
  struct trapframe *trapframe;  // trampoline.S 使用的数据页
  struct context context;       // 调用 swtch() 以运行该进程
  struct file *ofile[NOFILE];   // 打开的文件
  struct inode *cwd;            // 当前工作目录
  char name[16];                // 进程名（用于调试）

  pagetable_t k_pagetable;
  uint64 kstack_pa;
};
