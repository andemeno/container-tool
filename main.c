#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/stat.h>

static char child_stack[1024 * 1024];
static int pipefds[2];

#define cmd_max_len 33

struct Args {
    // имя хоста
    const char* hostname;
    // путь к корневой файловой системе
    const char* rootfs_path;
    // процесс с аргументами, который нужно запустить в контейнере
    const char* cmd[cmd_max_len + 1];
};

// функция дочернего процесса
int childMain(void* arg) {
    assert(arg != NULL);

    // ждём сообщения от родителя, что можно начинать работу
    close(pipefds[1]);
    char buf;
    if(read(pipefds[0], &buf, 1) != 1) {
        perror("child: read from pipefds");
        return 1;
    }
    close(pipefds[0]);

    const struct Args* args = (const struct Args*)arg;

    // задаём имя хоста
    if(args->hostname) {
        if(sethostname(args->hostname, strlen(args->hostname)) == -1) {
            perror("sethostname");
            return EXIT_FAILURE;
        }
    }

    // изолируем точки монтирования от родительского процесса
    if(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        perror("mount MS_PRIVATE");
        return EXIT_FAILURE;
    }

    // монтируем arg.rootfs_path сам в себя, чтобы он стал точкой монтирования
    if(mount(args->rootfs_path, args->rootfs_path, NULL, MS_BIND | MS_REC, NULL) == -1) {
        perror("mount MS_BIND");
        return EXIT_FAILURE;
    }

    char mountpoint[64];
    const int dir_mode = 0755;

    // создаем каталоги для монтирования служебных ФС и монтируем их
    // NOTE: procfs необходимо монтировать до смены корневого каталога (pivot_root)
    sprintf(mountpoint, "%s/proc", args->rootfs_path);
    mkdir(mountpoint, dir_mode);
    if(mount("proc", mountpoint, "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) == -1)
        perror("mount procfs");

    // NOTE: dev монтируем как tmpfs
    sprintf(mountpoint, "%s/dev", args->rootfs_path);
    mkdir(mountpoint, dir_mode);
    if(mount("tmpfs", mountpoint, "tmpfs", MS_NOSUID | MS_NOEXEC, NULL) == -1)
        perror("mount dev");

    // создаём каталог, в который поместим старый корень
    sprintf(mountpoint, "%s/oldroot", args->rootfs_path);
    mkdir(mountpoint, dir_mode);

    // делаем rootfs_path текущим
    if(chdir(args->rootfs_path) == -1) {
        perror("chdir rootfs_path");
        return EXIT_FAILURE;
    }

    // делаем текущий каталог корневым, а тот что до этого был корневым примонтируем в ./oldroot
    if(syscall(SYS_pivot_root, ".", "oldroot") == -1) {
        perror("pivot_root");
        return EXIT_FAILURE;
    }

    // отмонтируем старый корень, но каталог удалять не будем
    // по наличию oldroot в выводе утилиты ls можно будет понять, что это файловая система дочернего процесса
    if(umount2("oldroot", MNT_DETACH) == -1) {
        perror("umount oldroot");
    }

    // запускаем целевую команду
    execv(args->cmd[0], (char* const*)args->cmd);

    // в случае успеха execv, сюда не попадём
    perror("execv");
    return EXIT_FAILURE;
}

int setupIDmapping(pid_t pid) {
    // запрещаем дочернему процессу создавать группы
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
    int fd = open(path, O_WRONLY);
    if(fd != -1) {
        if(write(fd, "deny", 4) == -1)
            perror("write deny");
        close(fd);
    }

    // uid_map
    snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
    fd = open(path, O_WRONLY);
    if(fd == -1) {
        perror("uid_map");
        return -1;
    }
    // чтобы процесс внутри namespace видел себя запущенным из-под root (UID 0)
    dprintf(fd, "0 %d 1\n", getuid());
    close(fd);

    // gid_map по аналогии с uid_map
    snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
    fd = open(path, O_WRONLY);
    if(fd == -1) {
        perror("gid_map");
        return -1;
    }
    dprintf(fd, "0 %d 1\n", getgid());
    close(fd);

    return 0;
}

int parseMainArgs(int argc, char* argv[], struct Args* child_args, int* show_parent_info) {
    assert(child_args != NULL);
    assert(show_parent_args != NULL);

    // аргументы командной строки
    static struct option long_opts[] = {
        {"rootfs", required_argument, 0,'r'},
        {"host", required_argument, 0,'n'},
        {"parent", no_argument, 0,'p'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    child_args->hostname = NULL;
    child_args->rootfs_path = NULL;
    child_args->cmd[0] = NULL;
    *show_parent_info = 0;

    int opt;
    while((opt = getopt_long(argc, argv, "r:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'r':
                child_args->rootfs_path = optarg;
                break;
            case 'n':
                child_args->hostname = optarg;
                break;
            case 'p':
                *show_parent_info = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s --rootfs=PATH [--hostname=NAME] -- PROCESS [PROCESS ARGUMENTS]\n"
                                "Performs isolated PROCESS launch via namespaces (PID, mount, user, UTS, IPC, network)\n"
                                "-r, --rootfs=PATH\tpath to the root file system\n"
                                "-n, --host=NAME\tname of the host into isolated namespace\n"
                                "-p, --parent\t\tshow parent process info\n"
                                "-h, --help\t\tthis message\n", argv[0]);
                return -1;
        }
    }

    if(child_args->rootfs_path == NULL) {
        printf("Path to the root file system not specified\n");
        return -1;
    }

    if(optind < argc) {
        if(argc - optind > cmd_max_len) {
            printf("Too much PROCESS arguments: %d (%d allowed)\n", argc - optind - 1, cmd_max_len - 1);
            return -1;
        }

        for(int i = optind; i < argc; i++)
            child_args->cmd[i - optind] = argv[i];
        child_args->cmd[argc - optind] = NULL;
    }

    if(child_args->cmd[0] == NULL) {
        printf("Process to launch not specified\n");
        return -1;
    }

    return 0;
}

int pathExists(const char* path) {
    DIR* dir = opendir(path);
    if (dir) {
        closedir(dir);
        return 0;
    }
    return -1;
}

void showParentInfo() {
    if(system("echo parent id: ") == -1)
        perror("system echo");
    if(system("id") == -1)
        perror("system id");
    if(system("echo ") == -1)
        perror("system echo");
    if(system("echo parent hostname: ") == -1)
        perror("system echo");
    if(system("hostname") == -1)
        perror("system hostname");
    if(system("echo") == -1)
        perror("system echo");
    if(system("echo") == -1)
        perror("system echo");
}

int main(int argc, char* argv[]) {
    // получаем аргументы для запуска доцернего процесса
    struct Args child_args;
    int show_parent_info;
    if(parseMainArgs(argc, argv, &child_args, &show_parent_info) == -1)
        return EXIT_FAILURE;

    // корневой каталог должен существовать
    if(pathExists(child_args.rootfs_path) == -1) {
        printf("Path to the root file system does not exists\n");
        return EXIT_FAILURE;
    }

    // вывод информации о родительском процессе
    if(show_parent_info)
        showParentInfo();

    // создаем канал
    if(pipe(pipefds) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    // создаём дочерний процесс в изолированных пространствах имен
    int flags = CLONE_NEWPID  | // PID
                CLONE_NEWNS   | // mount
                CLONE_NEWUSER | // user
                CLONE_NEWUTS  | // UTS
                CLONE_NEWIPC  | // IPC
                CLONE_NEWNET  | // network
                SIGCHLD; // хотим получить сигнал о завершении процесса
    pid_t pid = clone(childMain,
                      child_stack + sizeof(child_stack),
                      flags,
                      &child_args);

    if(pid == -1) {
        perror("clone");
        return EXIT_FAILURE;
    }

    close(pipefds[0]);

    // настраиваем отображение UID и GID для дочернего процесса
    if(setupIDmapping(pid) != 0)
        return EXIT_FAILURE;

    // сообщаем дочернему процессу, что можно начинать работу
    if(write(pipefds[1], "1", 1) == -1)
        perror("write pipe");
    close(pipefds[1]);

    // ждём завершения дочернего процесса
    int status;
    waitpid(pid, &status, 0);
    return EXIT_SUCCESS;
}