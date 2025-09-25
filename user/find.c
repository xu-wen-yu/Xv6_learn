#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

// 递归查找函数
void find(char *path, char *target) {
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  // 打开路径
  if ((fd = open(path, 0)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  // 获取文件状态
  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  // 如果是普通文件，检查文件名是否匹配
  if (st.type == T_FILE) {
    char *name = path;
    // 提取文件名（去掉路径部分）
    for (p = path + strlen(path); p >= path && *p != '/'; p--)
      ;
    p++;
    if (p != path) {
      name = p;
    }
    // 文件名匹配时输出路径
    if (strcmp(name, target) == 0) {
      printf("%s\n", path);
    }
    close(fd);
    return;
  }

  // 如果是目录，递归处理
  if (st.type == T_DIR) {
    // 检查路径长度是否超过缓冲区大小
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
      printf("find: path too long\n");
      close(fd);
      return;
    }

    // 复制路径到缓冲区
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    // 遍历目录内容
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      // 跳过无效条目和 . .. 目录
      if (de.inum == 0 || strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) {
        continue;
      }

      // 构建子项的完整路径
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;

      // 检查这个子项的名称是否匹配目标
      if (strcmp(de.name, target) == 0) {
        printf("%s\n", buf);
      }

      // 如果子项是目录，递归查找
      if (stat(buf, &st) >= 0 && st.type == T_DIR) {
        find(buf, target);
      }
    }
  }

  close(fd);
}

int main(int argc, char *argv[]) {
  // 检查命令行参数
  if (argc != 3) {
    fprintf(2, "Usage: find <path> <name>\n");
    exit(1);
  }

  // 调用递归查找函数
  find(argv[1], argv[2]);
  exit(0);
}