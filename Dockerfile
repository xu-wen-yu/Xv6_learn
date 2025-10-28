FROM archlinux:latest

# Allow proxy to be passed in during build for environments with restricted egress.
# These are no-ops if build-args are not provided.
ARG http_proxy
ARG https_proxy
ARG no_proxy
ENV http_proxy=${http_proxy} \
    https_proxy=${https_proxy} \
    no_proxy=${no_proxy}

RUN pacman-key --init

# Use fast mirrors to improve reliability in restricted networks
RUN printf '%s\n' \
    'Server = https://mirrors.tuna.tsinghua.edu.cn/archlinux/$repo/os/$arch' \
    'Server = https://mirrors.ustc.edu.cn/archlinux/$repo/os/$arch' \
    'Server = https://mirrors.aliyun.com/archlinux/$repo/os/$arch' \
    > /etc/pacman.d/mirrorlist

# Speed up downloads
RUN sed -i 's/^#\?ParallelDownloads.*/ParallelDownloads = 10/' /etc/pacman.conf

RUN pacman -Syyu --noconfirm

RUN pacman -S --noconfirm gcc make perl wget git python

RUN pacman -S --noconfirm riscv64-elf-gdb riscv64-elf-gcc riscv64-elf-binutils riscv64-elf-newlib qemu-system-riscv

# Download gdb dashboard (ignore failure if GitHub not reachable)
RUN wget -T 30 -P ~ https://github.com/cyrus-and/gdb-dashboard/raw/master/.gdbinit || true
RUN pacman -S --noconfirm python-pygments
