function tc_off() {
    sudo tc qdisc del dev lo root
}

function tc_lan() {
    local latency="${1:-1ms}"
    local bandwidth="${2:-1Gbit}"

    sudo tc qdisc del dev lo root
    sudo tc qdisc add dev lo root handle 1:0 htb default 10
    sudo tc class add dev lo parent 1:0 classid 1:10 htb rate "${bandwidth}"
    sudo tc qdisc add dev lo parent 1:10 handle 10:0 netem delay "${latency}" 0.03ms 5% distribution normal
}

function tc_wan() {
    local latency="${1:-50ms}"
    local bandwidth="${2:-100Mbit}"

    sudo tc qdisc del dev lo root
    sudo tc qdisc add dev lo root handle 1:0 htb default 10
    sudo tc class add dev lo parent 1:0 classid 1:10 htb rate "${bandwidth}"
    sudo tc qdisc add dev lo parent 1:10 handle 10:0 netem delay "${latency}" 3ms 25% distribution normal
}

# Raise the kernel-wide caps on socket send/receive buffer sizes.
# setsockopt(SO_SNDBUF/SO_RCVBUF, ...) silently truncates to
# net.core.wmem_max / net.core.rmem_max, so a large increaseSocketBuffers()
# call in the benchmark code has no effect until these are raised too.
# Default caps buffer requests at ~256KB-2MB depending on distro, which is
# far smaller than the bandwidth-delay product under emulated high-latency
# links, causing large sends/recvs to stall.
#
# Usage: raise_socket_mem_max [bytes]   (default: 256 MiB)
function raise_socket_mem_max() {
    local bytes="${1:-268435456}"

    sudo sysctl -w net.core.rmem_max="${bytes}"
    sudo sysctl -w net.core.wmem_max="${bytes}"
    sudo sysctl -w net.ipv4.tcp_rmem="4096 87380 ${bytes}"
    sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 ${bytes}"
}

