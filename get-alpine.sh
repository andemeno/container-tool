#!/bin/sh

# первый аргумент должен задавать путь к каталогу
target_dir=$1

if [ -z "$target_dir" ]; then
    # если путь не задан, используем значение по умолчанию
    target_dir=/tmp/alpine
fi

echo "Save to $target_dir"

# создаем target_dir, если он не существует
mkdir -p "$target_dir"

# архив скачиваем в /tmp
cd /tmp
# подходящую версию можно найти здесь https://alpinelinux.org/downloads/ (раздел mini root file system)
curl -LO https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86/alpine-minirootfs-3.24.2-x86.tar.gz

# распаковываем в target_dir
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C "$target_dir"