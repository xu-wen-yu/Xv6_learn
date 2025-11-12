// 当前是哪个 hart（CPU 核）？
static inline uint64 r_mhartid() {
  uint64 x;
  asm volatile("csrr %0, mhartid" : "=r"(x));
  return x;
}

// 机器态状态寄存器 mstatus

#define MSTATUS_MPP_MASK (3L << 11)  // 之前的特权级模式
#define MSTATUS_MPP_M (3L << 11)
#define MSTATUS_MPP_S (1L << 11)
#define MSTATUS_MPP_U (0L << 11)
#define MSTATUS_MIE (1L << 3)  // 机器态中断使能

static inline uint64 r_mstatus() {
  uint64 x;
  asm volatile("csrr %0, mstatus" : "=r"(x));
  return x;
}

static inline void w_mstatus(uint64 x) { asm volatile("csrw mstatus, %0" : : "r"(x)); }

// 机器态异常程序计数器 mepc：
// 保存从异常返回时将要执行的指令地址。
static inline void w_mepc(uint64 x) { asm volatile("csrw mepc, %0" : : "r"(x)); }

// 管态（Supervisor）状态寄存器 sstatus

#define SSTATUS_SPP (1L << 8)   // 之前的模式，1=Supervisor，0=User
#define SSTATUS_SPIE (1L << 5)  // 管态“之前的中断使能”位
#define SSTATUS_UPIE (1L << 4)  // 用户态“之前的中断使能”位
#define SSTATUS_SIE (1L << 1)   // 管态中断使能
#define SSTATUS_UIE (1L << 0)   // 用户态中断使能
#define SSTATUS_SUM (1L << 18)  // 允许内核访问用户空间

static inline uint64 r_sstatus() {
  uint64 x;
  asm volatile("csrr %0, sstatus" : "=r"(x));
  return x;
}

static inline void w_sstatus(uint64 x) { asm volatile("csrw sstatus, %0" : : "r"(x)); }

// 管态中断挂起寄存器 sip
static inline uint64 r_sip() {
  uint64 x;
  asm volatile("csrr %0, sip" : "=r"(x));
  return x;
}

static inline void w_sip(uint64 x) { asm volatile("csrw sip, %0" : : "r"(x)); }

// 管态中断使能寄存器 sie
#define SIE_SEIE (1L << 9)  // 外部中断
#define SIE_STIE (1L << 5)  // 定时器中断
#define SIE_SSIE (1L << 1)  // 软件中断
static inline uint64 r_sie() {
  uint64 x;
  asm volatile("csrr %0, sie" : "=r"(x));
  return x;
}

static inline void w_sie(uint64 x) { asm volatile("csrw sie, %0" : : "r"(x)); }

// 机器态中断使能寄存器 mie
#define MIE_MEIE (1L << 11)  // 外部中断
#define MIE_MTIE (1L << 7)   // 定时器中断
#define MIE_MSIE (1L << 3)   // 软件中断
static inline uint64 r_mie() {
  uint64 x;
  asm volatile("csrr %0, mie" : "=r"(x));
  return x;
}

static inline void w_mie(uint64 x) { asm volatile("csrw mie, %0" : : "r"(x)); }

// 管态异常程序计数器 sepc：
// 保存从异常返回时将要执行的指令地址。
static inline void w_sepc(uint64 x) { asm volatile("csrw sepc, %0" : : "r"(x)); }

static inline uint64 r_sepc() {
  uint64 x;
  asm volatile("csrr %0, sepc" : "=r"(x));
  return x;
}

// 机器态异常委托寄存器 medeleg
static inline uint64 r_medeleg() {
  uint64 x;
  asm volatile("csrr %0, medeleg" : "=r"(x));
  return x;
}

static inline void w_medeleg(uint64 x) { asm volatile("csrw medeleg, %0" : : "r"(x)); }

// 机器态中断委托寄存器 mideleg
static inline uint64 r_mideleg() {
  uint64 x;
  asm volatile("csrr %0, mideleg" : "=r"(x));
  return x;
}

static inline void w_mideleg(uint64 x) { asm volatile("csrw mideleg, %0" : : "r"(x)); }

// 管态陷阱向量基地址寄存器 stvec
// 低两位为模式 bits。
static inline void w_stvec(uint64 x) { asm volatile("csrw stvec, %0" : : "r"(x)); }

static inline uint64 r_stvec() {
  uint64 x;
  asm volatile("csrr %0, stvec" : "=r"(x));
  return x;
}

// 机器态中断向量寄存器 mtvec
static inline void w_mtvec(uint64 x) { asm volatile("csrw mtvec, %0" : : "r"(x)); }

// 物理内存保护（PMP）相关寄存器
static inline void w_pmpcfg0(uint64 x) { asm volatile("csrw pmpcfg0, %0" : : "r"(x)); }

static inline void w_pmpaddr0(uint64 x) { asm volatile("csrw pmpaddr0, %0" : : "r"(x)); }

// 使用 RISC-V 的 Sv39 页表方案
#define SATP_SV39 (8L << 60)

#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))

// 管态地址转换与保护寄存器 satp；
// 保存当前页表的物理地址（以页号形式）。
static inline void w_satp(uint64 x) { asm volatile("csrw satp, %0" : : "r"(x)); }

static inline uint64 r_satp() {
  uint64 x;
  asm volatile("csrr %0, satp" : "=r"(x));
  return x;
}

// 管态临时寄存器 sscratch，供 trampoline.S 早期陷阱处理使用
static inline void w_sscratch(uint64 x) { asm volatile("csrw sscratch, %0" : : "r"(x)); }

static inline void w_mscratch(uint64 x) { asm volatile("csrw mscratch, %0" : : "r"(x)); }

// 管态陷阱原因寄存器 scause
static inline uint64 r_scause() {
  uint64 x;
  asm volatile("csrr %0, scause" : "=r"(x));
  return x;
}

// 管态陷阱附加值寄存器 stval
static inline uint64 r_stval() {
  uint64 x;
  asm volatile("csrr %0, stval" : "=r"(x));
  return x;
}

// 机器态计数器使能寄存器 mcounteren
static inline void w_mcounteren(uint64 x) { asm volatile("csrw mcounteren, %0" : : "r"(x)); }

static inline uint64 r_mcounteren() {
  uint64 x;
  asm volatile("csrr %0, mcounteren" : "=r"(x));
  return x;
}

// 机器态时间/周期计数器 time
static inline uint64 r_time() {
  uint64 x;
  asm volatile("csrr %0, time" : "=r"(x));
  return x;
}

// 使能设备中断（置 SIE）
static inline void intr_on() { w_sstatus(r_sstatus() | SSTATUS_SIE); }

// 关闭设备中断（清 SIE）
static inline void intr_off() { w_sstatus(r_sstatus() & ~SSTATUS_SIE); }

// 设备中断当前是否开启？
static inline int intr_get() {
  uint64 x = r_sstatus();
  return (x & SSTATUS_SIE) != 0;
}

static inline uint64 r_sp() {
  uint64 x;
  asm volatile("mv %0, sp" : "=r"(x));
  return x;
}

// 读写 tp（线程指针），保存本核的 hartid（核编号），即 cpus[] 的索引
static inline uint64 r_tp() {
  uint64 x;
  asm volatile("mv %0, tp" : "=r"(x));
  return x;
}

static inline void w_tp(uint64 x) { asm volatile("mv tp, %0" : : "r"(x)); }

static inline uint64 r_ra() {
  uint64 x;
  asm volatile("mv %0, ra" : "=r"(x));
  return x;
}

// 刷新 TLB
static inline void sfence_vma() {
  // 参数 zero, zero 表示刷新所有 TLB 项
  asm volatile("sfence.vma zero, zero");
}

#define PGSIZE 4096  // 定义内存页大小为4096字节（4KB）
#define PGSHIFT 12   // 页内偏移的位数。因为2^12 = 4096，所以地址的低12位表示页内偏移

#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

// 页表项标志位定义（用于控制页面访问权限）  
#define PTE_V (1L << 0)  // valid - 有效位：1表示页表项有效，0表示无效（访问会触发页错误）
#define PTE_R (1L << 1)  // read - 可读位：控制页面是否可读
#define PTE_W (1L << 2)  // write - 可写位：控制页面是否可写
#define PTE_X (1L << 3)  // execute - 可执行位：控制页面是否可执行
#define PTE_U (1L << 4)  // user - 用户可访问位：1表示用户模式可访问，0表示仅内核可访问

// 将物理地址转换为页表项格式
// 操作步骤：1.右移12位得到物理页号 2.左移10位为标志位腾出空间
// 在RISC-V Sv39中，页表项的[63:10]存储物理页号，[9:0]存储标志位
#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)

// 从页表项提取物理地址
// 操作步骤：1.右移10位清除标志位 2.左移12位得到物理页基地址
// 注意：得到的是物理页的起始地址，要得到完整地址需加上页内偏移
#define PTE2PA(pte) (((pte) >> 10) << 12)

#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// 从虚拟地址中提取三级页表各自的 9 位索引。
#define PXMASK 0x1FF  // 9 位
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va) ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

// 超过可能的最高虚拟地址一位。
// MAXVA 实际上比 Sv39 允许的最大值少 1 位，
// 这样可避免当最高位为 1 时对虚拟地址进行符号扩展。
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))

typedef uint64 pte_t;
typedef uint64 *pagetable_t;  // 每个页表页包含 512 个 PTE
