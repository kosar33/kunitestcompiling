#!/bin/bash
set -e
echo "🚀 Настраиваем Kuni LXC Sandbox..."

mkdir -p /tmp/kuni_sockets
chmod 777 /tmp/kuni_sockets

# 1. Сетевая изоляция
echo "🛡️ Настройка сетевой изоляции (iptables)..."
iptables -I INPUT -i lxcbr0 -p udp --dport 67 -j ACCEPT
iptables -I INPUT -i lxcbr0 -p udp --dport 53 -j ACCEPT
iptables -I INPUT -i lxcbr0 -p tcp --dport 53 -j ACCEPT
iptables -A INPUT -i lxcbr0 -j REJECT
iptables -I FORWARD -i lxcbr0 -d 192.168.0.0/16 -j REJECT
iptables -I FORWARD -i lxcbr0 -d 10.0.0.0/8 -j REJECT
iptables -I FORWARD -i lxcbr0 -d 172.16.0.0/12 -j REJECT

# 2. Установка LXC
apt-get update && apt-get install -y lxc lxc-templates lxc-utils bridge-utils iptables g++

# 3. Компиляция демона
echo "🔨 Компиляция worker_daemon..."
mkdir -p build_sandbox
g++ -O3 -static sandbox/worker_daemon.cpp -o build_sandbox/worker_daemon

# 4. Создание базового шаблона LXC
echo "🐳 Создание базового контейнера Alpine..."
lxc-destroy -n kuni_base -f || true
lxc-create -n kuni_base -t download -- -d alpine -r 3.19 -a amd64

# 5. Настройка контейнера
ROOTFS="/var/lib/lxc/kuni_base/rootfs"
cp build_sandbox/worker_daemon $ROOTFS/usr/local/bin/kuni_worker

lxc-start -n kuni_base
lxc-attach -n kuni_base -- apk update
lxc-attach -n kuni_base -- apk add --no-cache git tree bash curl

# Прописываем автозапуск
lxc-attach -n kuni_base -- sh -c "cat << 'EOF' > /etc/init.d/kuni_worker
#!/sbin/openrc-run
name=\"kuni_worker\"
command=\"/usr/local/bin/kuni_worker\"
command_background=true
pidfile=\"/run/kuni_worker.pid\"
EOF"
lxc-attach -n kuni_base -- chmod +x /etc/init.d/kuni_worker
lxc-attach -n kuni_base -- rc-update add kuni_worker default

lxc-stop -n kuni_base

cat << 'EOF' >> /var/lib/lxc/kuni_base/config
lxc.net.0.type = none
lxc.mount.entry = /tmp/kuni_sockets tmp/kuni_sockets none bind,create=dir 0 0
EOF

echo "✅ Установка завершена!"
