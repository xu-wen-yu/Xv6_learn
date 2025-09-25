#include "kernel/types.h"
#include "user.h"

int main(int argc, char *argv[])
{
    int ping_pipe[2];  // 父进程到子进程的管道
    int pong_pipe[2];  // 子进程到父进程的管道
    int parent_pid;    // 父进程ID
    int child_pid;     // 子进程ID

    // 创建管道
    pipe(ping_pipe);
    pipe(pong_pipe);


    // 创建子进程
    if (fork() == 0) {
        child_pid = getpid();
        // 子进程
        close(ping_pipe[1]);  // 关闭父写端
        close(pong_pipe[0]);  // 关闭子读端

        // 从父进程读取数据（等待父进程写入）
        read(ping_pipe[0], &parent_pid, sizeof(parent_pid));
        printf("%d: received ping from pid %d\n", child_pid, parent_pid);

        // 向父进程发送数据
        write(pong_pipe[1], &child_pid, sizeof(child_pid));

        close(ping_pipe[0]);
        close(pong_pipe[1]);
        exit(0);
    } else {
        // 父进程
        parent_pid = getpid();// 获取父进程ID
        close(ping_pipe[0]);  // 关闭父读端
        close(pong_pipe[1]);  // 关闭子写端

        // 向子进程发送数据（先写入，启动通信流程）
        write(ping_pipe[1], &parent_pid, sizeof(parent_pid));

        // 从子进程读取数据（等待子进程响应）
        read(pong_pipe[0], &child_pid, sizeof(child_pid));
        printf("%d: received pong from pid %d\n", parent_pid, child_pid);

        close(ping_pipe[1]);
        close(pong_pipe[0]);
        wait(0);  // 等待子进程结束
        exit(0);
    }
}