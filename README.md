# container-tool
#### Сборка проекта
Для сборки необходимо выполнить команды:
```bash
$ cd /path/to/sources
$ mkdir build
$ cmake -B ./build -DCMAKE_BUILD_TYPe=Release
$ cmake --build ./build
```
Исполняемый файл находится в каталоге /path/to/sources/build
#### Использование программы
```bash
$ ./container-tool --help
Usage: ./container-tool --rootfs=PATH [--hostname=NAME] -- PROCESS [PROCESS ARGUMENTS]
Performs isolated PROCESS launch via namespaces (PID, mount, user, UTS, IPC, network)
-r, --rootfs=PATH	path to the root file system
-n, --hostn=NAME	name of the host into isolated namespace
-p, --parent		show parent process info
-h, --help		    this message
```

П р и м е ч а н и е: использование программы не требует root-привелегий (запуск без sudo). Однако для моего хоста (Linux 7.0.0-34-generic #34~24.04.1-Ubuntu) потребовалось дополнительная настройка окружения:
```bash
sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
```
Этот параметр ядра управляет ограничениями службы AppArmor на создание пространств имен пользователей непривилигированными процессами. По умолчанию на моем хосте этот параметр включен (значение 1), и вызовы mount в дочернем процессе завершались с ошибкой `Operation not permitted`. Для учебных целей я его просто отключаю (до следующей перезагрузки хоста), но более безопасный путь - использоваение локальных профилей AppArmor с правилом `userns`.
П р и м е ч а н и е: перед запуском программы необходимо, чтобы каталог с корневой файловой системой контейнера уже существавал. Для его создания можно использовать скрипт get-alpine.sh
```bash
$ ./get-alpine.sh /path/to/rootfs-dir
```
#### Проверка работы
Ниже приведены листинги с результатами запуска программы
```bash
$ ./container-tool --rootfs=/tmp/alpine -- /bin/ls -l
total 0
drwxr-xr-x    2 root     root          1680 Sep  6  2024 bin
drwxrwxrwt    2 root     root            40 Sep 26 10:54 dev
drwxr-xr-x   17 root     root           740 Sep  6  2024 etc
drwxr-xr-x    2 root     root            40 Sep  6  2024 home
drwxr-xr-x    6 root     root           260 Sep  6  2024 lib
drwxr-xr-x    5 root     root           100 Sep  6  2024 media
drwxr-xr-x    2 root     root            40 Sep  6  2024 mnt
drwxr-xr-x    2 root     root            40 Sep 26 09:46 oldroot
drwxr-xr-x    2 root     root            40 Sep  6  2024 opt
dr-xr-xr-x  478 nobody   nobody           0 Sep 26 10:54 proc
drwx------    2 root     root            40 Sep  6  2024 root
drwxr-xr-x    2 root     root            40 Sep  6  2024 run
drwxr-xr-x    2 root     root          1260 Sep  6  2024 sbin
drwxr-xr-x    2 root     root            40 Sep  6  2024 srv
drwxr-xr-x    2 root     root            40 Sep  6  2024 sys
drwxrwxr-x    2 root     root            40 Sep  6  2024 tmp
drwxr-xr-x    7 root     root           140 Sep  6  2024 usr
drwxr-xr-x   12 root     root           260 Sep  6  2024 var

```
П р и м е ч а н и е: наличие каталога oldroot свидетельствует о том, что это файловая система внутри rootfs.
mount-пространства родительского и дочернего процесса (контейнера) изолированы.

```bash
$ ./container-tool --rootfs=/tmp/alpine /bin/ps aux
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/ps
    
$ ./container-tool --rootfs=/tmp/alpine --host=isolated --parent /bin/sh
parent id:
uid=1000(lux) gid=1000(lux) группы=1000(lux),4(adm),20(dialout),24(cdrom),27(sudo),30(dip),46(plugdev),100(users),114(lpadmin),125(vboxusers),126(wireshark),129(sbuild),984(docker)

parent hostname:
asus


/ # id
uid=0(root) gid=0(root) groups=65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),65534(nobody),0(root)
/ # hostname
isolated
/ # ip a
1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
/ # exit

```

Листинг показывает, что родительский и дочерний процессы:
 - изолированы на уровне идентификаторов процесов (PID)
 - изолированы на уровне идентификации пользователей (user) (имеют разные идентификаторы пользователя и группы, при этом дочерний процесс внутри своего пространства является root)
 - изолированы на сетевом уровне (network) (имеют различные имена хостов, в контейнере есть только loopback сетевой интерфейс).

`procfs` - это файловая система, которая дает доступ к структурам данных ядра и информации о работающих процессах, она привязан к PID-пространству того, кто её монтирует. Если попытаться смонтировать `procfs` без изоляции mount-пространства, то новая `procfs` появится в общем дереве монтирований хоста. Поскольку мы изолировали контейнер по PID, то идентификаторы процессов контейнера могуг дублировать идентификаторы процессов родителя. Значит хост в `/proc/1`  увидит процесс из PID-пространства контейнера, а хостовый процесс c PID=1 будет затёрт. В итоге, поведение хоста может стать непредсказуемым.
