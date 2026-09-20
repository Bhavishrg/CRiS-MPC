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

# Give every unordered NPH process pair an independent bandwidth budget.
#
# NetNP creates one TCP socket for each directed edge sender -> receiver. Its
# stable server-port encoding is:
#
#   base_port + sender * 1024 + (receiver > sender ? receiver - 1 : receiver)
#
# For an unordered pair {i,j}, both directed sockets i->j and j->i are placed in
# the same HTB class. TCP packets with either server port as source are included
# too, so ACK traffic receives the same delay and consumes the same pair budget.
# Different pairs use different sibling classes and therefore do not share a
# bandwidth ceiling.
#
# NPH's --num-parties is the number of compute parties; the helper is one extra
# process. Thus `tc_nph_pairs 5 ...` creates C(6,2)=15 independently shaped
# pair classes.
#
# Usage:
#   tc_nph_pairs <compute-parties> <base-port> [latency] [bandwidth] [jitter] [correlation]
#
# Example:
#   tc_nph_pairs 5 14900 50ms 100Mbit
#   ./run.sh microbench_graphiti_init --protocol nph --num-parties 5 \
#       --port 14900 --graph-size 10000
#
# This setup matches IPv4 loopback traffic, which is what --peer 127.0.0.1 uses.
function tc_nph_pairs() {
    local num_compute_parties="${1:-}"
    local base_port="${2:-}"
    local latency="${3:-50ms}"
    local bandwidth="${4:-100Mbit}"
    local jitter="${5:-3ms}"
    local correlation="${6:-25%}"
    local fallback_bandwidth="${TC_FALLBACK_BANDWIDTH:-100Gbit}"

    if ! [[ "${num_compute_parties}" =~ ^[0-9]+$ ]] ||
       [ "${num_compute_parties}" -lt 2 ]; then
        echo "tc_nph_pairs: compute-parties must be an integer >= 2" >&2
        return 1
    fi
    if ! [[ "${base_port}" =~ ^[0-9]+$ ]] ||
       [ "${base_port}" -lt 1 ] || [ "${base_port}" -gt 65535 ]; then
        echo "tc_nph_pairs: base-port must be an integer in 1..65535" >&2
        return 1
    fi

    local total_processes=$((num_compute_parties + 1))
    local max_sender=$((total_processes - 1))
    local max_recv_index=$((total_processes - 2))
    local max_port=$((base_port + max_sender * 1024 + max_recv_index))
    if [ "${max_port}" -gt 65535 ]; then
        echo "tc_nph_pairs: NPH port range ends at ${max_port}, above 65535" >&2
        echo "tc_nph_pairs: choose a lower base port or fewer parties" >&2
        return 1
    fi

    # All unrelated loopback traffic falls into this effectively unshaped
    # class. Pair classes are direct siblings of it, so there is no common
    # 100-Mbit parent bottleneck.
    sudo tc qdisc del dev lo root 2>/dev/null || true
    sudo tc qdisc add dev lo root handle 1: htb default fff0
    sudo tc class add dev lo parent 1: classid 1:fff0 htb \
        rate "${fallback_bandwidth}" ceil "${fallback_bandwidth}"

    local class_minor=10
    local left
    local right
    for ((left = 0; left < total_processes; ++left)); do
        for ((right = left + 1; right < total_processes; ++right)); do
            local left_to_right_index=$((right > left ? right - 1 : right))
            local right_to_left_index=$((left > right ? left - 1 : left))
            local port_left_to_right=$((base_port + left * 1024 + left_to_right_index))
            local port_right_to_left=$((base_port + right * 1024 + right_to_left_index))
            local flowid="1:${class_minor}"

            sudo tc class add dev lo parent 1: classid "${flowid}" htb \
                rate "${bandwidth}" ceil "${bandwidth}"
            sudo tc qdisc add dev lo parent "${flowid}" handle "${class_minor}:" \
                netem delay "${latency}" "${jitter}" "${correlation}" \
                distribution normal

            # Data packets match destination server ports. Reverse TCP packets
            # such as ACKs match the corresponding source server ports.
            local port
            for port in "${port_left_to_right}" "${port_right_to_left}"; do
                sudo tc filter add dev lo protocol ip parent 1: prio 1 u32 \
                    match ip protocol 6 0xff \
                    match ip dport "${port}" 0xffff \
                    flowid "${flowid}"
                sudo tc filter add dev lo protocol ip parent 1: prio 1 u32 \
                    match ip protocol 6 0xff \
                    match ip sport "${port}" 0xffff \
                    flowid "${flowid}"
            done

            echo "NPH pair ${left}-${right}: ${bandwidth}, ${latency}, " \
                 "ports ${port_left_to_right}/${port_right_to_left}, class ${flowid}"
            class_minor=$((class_minor + 1))
        done
    done

    local num_pair_classes=$((total_processes * (total_processes - 1) / 2))
    echo "Configured ${num_pair_classes} independent NPH pair classes on lo."
    echo "Use the same base port (${base_port}) with run.sh --port."
}

# Give every NPH process one aggregate bandwidth budget shared by all of its
# connections. This models a host/NIC limit instead of an independent link
# limit for every process pair.
#
# For the directed socket sender -> receiver, packets sent by the application
# have the encoded server port as their destination and are charged to the
# sender. Reverse TCP traffic (ACKs, window updates, etc.) has that port as its
# source and is charged to the receiver. Consequently all outgoing traffic
# attributable to one process shares one HTB class and one bandwidth ceiling.
#
# NPH's --num-parties excludes the helper, so `tc_nph_parties 5 ...` creates
# six classes: five compute-party classes and one helper class.
#
# Usage:
#   tc_nph_parties <compute-parties> <base-port> [latency] [bandwidth] [jitter] [correlation]
#
# Example:
#   tc_nph_parties 5 14900 50ms 100Mbit
#   ./run.sh microbench_graphiti_init --protocol nph --num-parties 5 \
#       --port 14900 --graph-size 10000
#
# The bandwidth argument is the aggregate outgoing limit for each party, not
# a per-peer limit. This setup matches IPv4 traffic on 127.0.0.1.
function tc_nph_parties() {
    local num_compute_parties="${1:-}"
    local base_port="${2:-}"
    local latency="${3:-50ms}"
    local bandwidth="${4:-100Mbit}"
    local jitter="${5:-3ms}"
    local correlation="${6:-25%}"
    local fallback_bandwidth="${TC_FALLBACK_BANDWIDTH:-100Gbit}"

    if ! [[ "${num_compute_parties}" =~ ^[0-9]+$ ]] ||
       [ "${num_compute_parties}" -lt 2 ]; then
        echo "tc_nph_parties: compute-parties must be an integer >= 2" >&2
        return 1
    fi
    if ! [[ "${base_port}" =~ ^[0-9]+$ ]] ||
       [ "${base_port}" -lt 1 ] || [ "${base_port}" -gt 65535 ]; then
        echo "tc_nph_parties: base-port must be an integer in 1..65535" >&2
        return 1
    fi

    local total_processes=$((num_compute_parties + 1))
    local max_sender=$((total_processes - 1))
    local max_recv_index=$((total_processes - 2))
    local max_port=$((base_port + max_sender * 1024 + max_recv_index))
    if [ "${max_port}" -gt 65535 ]; then
        echo "tc_nph_parties: NPH port range ends at ${max_port}, above 65535" >&2
        echo "tc_nph_parties: choose a lower base port or fewer parties" >&2
        return 1
    fi

    sudo tc qdisc del dev lo root 2>/dev/null || true
    sudo tc qdisc add dev lo root handle 1: htb default fff0
    sudo tc class add dev lo parent 1: classid 1:fff0 htb \
        rate "${fallback_bandwidth}" ceil "${fallback_bandwidth}"

    local party
    for ((party = 0; party < total_processes; ++party)); do
        local class_minor=$((party + 10))
        local flowid="1:${class_minor}"

        sudo tc class add dev lo parent 1: classid "${flowid}" htb \
            rate "${bandwidth}" ceil "${bandwidth}"
        sudo tc qdisc add dev lo parent "${flowid}" handle "${class_minor}:" \
            netem delay "${latency}" "${jitter}" "${correlation}" \
            distribution normal
    done

    local sender
    local receiver
    for ((sender = 0; sender < total_processes; ++sender)); do
        for ((receiver = 0; receiver < total_processes; ++receiver)); do
            if [ "${sender}" -eq "${receiver}" ]; then
                continue
            fi

            local recv_index=$((receiver > sender ? receiver - 1 : receiver))
            local port=$((base_port + sender * 1024 + recv_index))
            local sender_flowid="1:$((sender + 10))"
            local receiver_flowid="1:$((receiver + 10))"

            # Application data on sender -> receiver.
            sudo tc filter add dev lo protocol ip parent 1: prio 1 u32 \
                match ip protocol 6 0xff \
                match ip dport "${port}" 0xffff \
                flowid "${sender_flowid}"

            # TCP traffic travelling back from receiver to sender.
            sudo tc filter add dev lo protocol ip parent 1: prio 1 u32 \
                match ip protocol 6 0xff \
                match ip sport "${port}" 0xffff \
                flowid "${receiver_flowid}"
        done
    done

    for ((party = 0; party < total_processes; ++party)); do
        echo "NPH party ${party}: aggregate ${bandwidth}, ${latency}, class 1:$((party + 10))"
    done
    echo "Configured ${total_processes} aggregate NPH party classes on lo."
    echo "Use the same base port (${base_port}) with run.sh --port."
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
