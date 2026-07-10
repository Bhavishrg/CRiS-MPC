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
