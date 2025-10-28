// 物理内存布局
//
// qemu 使用 -machine virt 按照下列方式映射（基于 qemu 的 hw/riscv/virt.c）：
//
// 00001000 -- 启动 ROM，由 qemu 提供
// 02000000 -- CLINT（本地中断控制器）
// 0C000000 -- PLIC（可编程中断控制器）
// 10000000 -- uart0
// 10001000 -- virtio 磁盘
// 80000000 -- 启动 ROM 在 machine 模式下跳转到此处
//             - kernel 在此处加载内核
// 80000000 之后为未使用的 RAM。
//
// 内核对物理内存的使用如下：
// 80000000 -- entry.S，然后是内核的代码（text）和数据
// end -- 内核页面分配区的起始地址
// PHYSTOP -- 内核使用的物理内存结束地址
//
// qemu 将 UART 寄存器映射到物理内存的此处。
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio 的内存映射 I/O 接口
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// 本地中断控制器，包含定时器。
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT + 0xBFF8) // 自启动以来的时钟周期计数。

// qemu 将可编程中断控制器（PLIC）映射到此处。
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// 内核期望在物理地址 0x80000000 到 PHYSTOP 之间
// 有可供内核和用户页使用的 RAM。
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)

// 将 trampoline 页面映射到最高地址，
// 在用户空间和内核空间中都相同。
#define TRAMPOLINE (MAXVA - PGSIZE)

// 将内核栈映射在 trampoline 之下，
// 每个内核栈两侧都有无效的保护页（guard pages）。
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// 用户内存布局。
// 从地址 0 开始：
//   程序代码（text）
//   原始数据和 bss 段
//   固定大小的栈
//   可扩展的堆
//   ...
//   TRAPFRAME（p->trapframe，由 trampoline 使用）
//   TRAMPOLINE（与内核中映射的同一页）
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
