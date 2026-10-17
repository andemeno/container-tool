#define _GNU_SOURCE
#include <errno.h>
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
#include <termios.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/stat.h>

static pid_t child_pid = -1;
static char child_stack[1024 * 1024];
static int pipefds[2];

// файловые дескрипторы master и slave концов псевдотерминала (PTY)
static int master_fd = -1;
static int slave_fd = -1;

// «родные» параметры терминала хоста
static struct termios orig_termios;

#define cmd_max_len 33

struct Args {
    // имя хоста
    const char* hostname;
    // путь к корневой файловой системе
    const char* rootfs_path;
    // процесс с аргументами, который нужно запустить в контейнере
    const char* cmd[cmd_max_len + 1];
    int interactive;
    int tty;
};

static void restoreTerminal() {
    // восстанавливать нужно, если создавали псевдотерминал
    if(master_fd >= 0 && isatty(STDIN_FILENO))
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void sigHandler(int sig) {
    // восстанавливаем настройки терминала хоста
    restoreTerminal();

    // пересылаем сигнал дочернему процессу
    if(child_pid > 0)
        kill(child_pid, sig);

    // завершаем родительский процесс
    _exit(128 + sig);
}

static int setupPTY() {
    // родительский процесс должен быть запущен из терминала
    if(!isatty(STDIN_FILENO)) {
        fprintf(stderr, "Cannot use -t: stdin is not a terminal\n");
        return -1;
    }

    // сохраняем «родные» параметры терминала хоста
    if(tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("tcgetattr");
        return -1;
    }

    // выполняем процедуру создания псевдотерминала
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if(master_fd == -1) {
        perror("posix_openpt");
        return -1;
    }

    if(grantpt(master_fd) == -1) {
        close(master_fd);
        perror("grantpt");
        return -1;
    }

    if(unlockpt(master_fd) == -1) {
        close(master_fd);
        perror("unlockpt");
        return -1;
    }

    char *name = ptsname(master_fd);
    if(!name) {
        fprintf(stderr, "ptsname return NULL");
        close(master_fd);
        return -1;
    }

    slave_fd = open(name, O_RDWR | O_NOCTTY);
    if(slave_fd == -1) {
        close(master_fd);
        return -1;
    }

    // регистрируем обработку сигналов, завершающих родительский процесс
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);
    signal(SIGHUP, sigHandler);

    // при штатном завершении родительского процесса тоже нужно восстанавливать терминал
    atexit(restoreTerminal);

    return slave_fd;
}

// функция дочернего процесса
int childMain(void* arg) {
    assert(arg != NULL);

    // ждём сообщения от родителя, что можно начинать работу
    close(pipefds[1]);
    char buf;
    if(read(pipefds[0], &buf, 1) != 1) {
        perror("child: read from pipefds");
        return EXIT_FAILURE;
    }
    close(pipefds[0]);

    const struct Args* args = (const struct Args*)arg;

    if(args->tty) {
        // делаем дочерний процесс лидером сессии, чтобы затем назначить управляющий терминал
        if(setsid() == -1) {
            perror("setsid");
            return EXIT_FAILURE;
        }

        // делаем slave_fd управляющим терминалом дочернего процесса
        if(ioctl(slave_fd, TIOCSCTTY, 0) == -1) {
            perror("TIOCSCTTY");
            return EXIT_FAILURE;
        }

        // перенаправляем стандартные потоки ввода вывода на slave-конец PTY
        if(args->interactive) { // нужна интерактивность
            if(dup2(slave_fd, STDIN_FILENO) ==-1) {
                perror("dup2 STDIN_FILENO");
                return EXIT_FAILURE;
            }
        } else {
            close(STDIN_FILENO);
            open("/dev/null", O_RDONLY);
        }

        if(dup2(slave_fd, STDOUT_FILENO) ==-1) {
            perror("dup2 STDOUT_FILENO");
            return EXIT_FAILURE;
        }
        if(dup2(slave_fd, STDERR_FILENO) ==-1) {
            perror("dup2 STDERR_FILENO");
            return EXIT_FAILURE;
        }

        // закрываем slave_fd, если он не является дескриптором стандартного потока
        if(slave_fd > STDERR_FILENO)
            close(slave_fd);

    } else if (!args->interactive) {
        // нам не нужна интерактивность и псевдотерминал
        close(STDIN_FILENO);
        open("/dev/null", O_RDONLY);
    }

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

// цикл ретрансляции данных:
//  - из stdin хоста в дочерний процесс;
//  - из дочернего процесса в stdout хоста.
static int relayCycle() {
    struct pollfd pfds[2];
    char buf[4096];
    ssize_t n;

    pfds[0].fd = STDIN_FILENO;
    pfds[0].events = POLLIN;

    pfds[1].fd = master_fd;
    pfds[1].events = POLLIN;

    while(1) {
        if(poll(pfds, 2, -1) == -1) {
            if(errno == EINTR)
                continue;
            return -1;
        }

        if(pfds[0].revents & POLLIN) {
            // есть данные от хоста
            n = read(STDIN_FILENO, buf, sizeof(buf));

            if(n <= 0)
                break; // Ctrl+D

            if(write(master_fd, buf, n) != n)
                break;
        }

        if(pfds[1].revents & POLLIN) {
            // есть данные от дочернего процесса
            n = read(master_fd, buf, sizeof(buf));

            if(n <= 0)
                break; // дочерний процесс завершился

            if(write(STDOUT_FILENO, buf, n) != n)
                break;
        }

        // завершаем цикл, если на дескрипторах ошибки
        if(pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if(pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
    }

    return 0;
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
    assert(show_parent_info != NULL);

    // аргументы командной строки
    static struct option long_opts[] = {
        {"rootfs", required_argument, 0,'r'},
        {"host", required_argument, 0,'n'},
        {"parent", no_argument, 0,'p'},
        {"interactive", no_argument, 0, 'i'},
        {"tty", no_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    child_args->hostname = NULL;
    child_args->rootfs_path = NULL;
    child_args->cmd[0] = NULL;
    child_args->interactive = 0;
    child_args->tty = 0;
    *show_parent_info = 0;

    int opt;
    while((opt = getopt_long(argc, argv, "r:n:pith", long_opts, NULL)) != -1) {
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
            case 'i':
                child_args->interactive = 1;
                break;
            case 't':
                child_args->tty = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s -r|--rootfs=PATH [-n|--hostname=NAME] [-i|--interactive] [-t|--tty] -- PROCESS [PROCESS ARGUMENTS]\n"
                                "Performs isolated PROCESS launch via namespaces (PID, mount, user, UTS, IPC, network)\n"
                                "\t-r, --rootfs=PATH\tpath to the root file system\n"
                                "\t-i, --interactive\tkeep STDIN open\n"
                                "\t-t, --tty\tallocate a pseudo-TTY\n"
                                "\t-n, --host=NAME\tname of the host into isolated namespace\n"
                                "\t-p, --parent\t\tshow parent process info\n"
                                "\t-h, --help\t\tthis message\n", argv[0]);
                return -1;
        }
    }

    if(child_args->rootfs_path == NULL) {
        fprintf(stderr, "Path to the root file system not specified\n");
        return -1;
    }

    if(optind < argc) {
        if(argc - optind > cmd_max_len) {
            fprintf(stderr, "Too much PROCESS arguments: %d (%d allowed)\n", argc - optind - 1, cmd_max_len - 1);
            return -1;
        }

        for(int i = optind; i < argc; i++)
            child_args->cmd[i - optind] = argv[i];
        child_args->cmd[argc - optind] = NULL;
    }

    if(child_args->cmd[0] == NULL) {
        fprintf(stderr, "Process to launch not specified\n");
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
    if(system("echo parent id: ; id ; echo ; echo parent hostname: ; hostname ; echo") == -1)
        perror("system(id; hostname)");
}

int main(int argc, char* argv[]) {
    // получаем аргументы для запуска доцернего процесса
    struct Args child_args;
    int show_parent_info;
    if(parseMainArgs(argc, argv, &child_args, &show_parent_info) == -1)
        return EXIT_FAILURE;

    // корневой каталог должен существовать
    if(pathExists(child_args.rootfs_path) == -1) {
        fprintf(stderr, "Path to the root file system does not exists\n");
        return EXIT_FAILURE;
    }

    // вывод информации о родительском процессе
    if(show_parent_info)
        showParentInfo();

    if(child_args.tty) {
        if(setupPTY() == -1)
            return EXIT_FAILURE;
    }

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
    child_pid = clone(childMain,
                      child_stack + sizeof(child_stack),
                      flags,
                      &child_args);

    if(child_pid == -1) {
        perror("clone");
        return EXIT_FAILURE;
    }

    close(pipefds[0]);

    // настраиваем отображение UID и GID для дочернего процесса
    if(setupIDmapping(child_pid) != 0)
        return EXIT_FAILURE;

    // сообщаем дочернему процессу, что можно начинать работу
    if(write(pipefds[1], "1", 1) == -1)
        perror("write pipe");
    close(pipefds[1]);

    if(child_args.tty) {
        // на хосте не нужен slave_fd
        close(slave_fd);

        // переводим терминал хоста в raw-режим
        struct termios raw = orig_termios;
        cfmakeraw(&raw);
        if(tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
            perror("tcsetattr raw");
            kill(child_pid, SIGTERM);
            return EXIT_FAILURE;
        }

        // запускаем ретрансляцию
        relayCycle();

        // завершаем работу с PTY
        restoreTerminal();
        close(master_fd);
    }

    // ждём завершения дочернего процесса
    int status;
    waitpid(child_pid, &status, 0);

    if(WIFEXITED(status)) // дочерний процесс завершился нормально
        return WEXITSTATUS(status);

    if(WIFSIGNALED(status)) // дочерний процесс завершился по сигналу
        return 128 + WTERMSIG(status);

    return EXIT_SUCCESS;
}