#!/bin/bash
set -e

show_help()
{
cat << EOF
usage: $(basename $BASH_SOURCE) host vnc_port [options]
Tunnels the vnc connection through ssh.
host: hostname
vnc_port: vnc server port at the host

Options after host and port are forwarded to wlvncc.

Environment variable SSH_PORT can be set for a custom remote ssh port.
EOF
    exit 1
}

find_free_port()
{
    let start=49152
    let range=5000
    local found_port=""
    for ((port = start; port < start + range; ++port)); do
        (echo -n >/dev/tcp/127.0.0.1/${port}) >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            found_port="$port"
            break
        fi
    done
    if [ -z "$found_port" ]; then
        found_port="0"
    fi
    echo $found_port
}

main()
{
    local remote_port=${SSH_PORT:-22}

    local host="$1"
    local port="$2"

    if [ -z "$port" ] || [ -z "$host" ]; then
        show_help
    fi
    shift; shift

    local temp_dir=$(mktemp -d)
    local master_file="${temp_dir}/wlvncc"
    local port_to_use=$(find_free_port)
    if [ $port_to_use -eq 0 ]; then
        echo "err: failed to find free local port"
        return 1
    fi
    echo "Using port $port_to_use"

    ssh -f -M -p${remote_port} -NL ${port_to_use}:localhost:$port $host -o ControlMaster=yes -o ControlPath=$master_file
    trap "ssh -p $port_to_use -o ControlMaster=no -o ControlPath=$master_file localhost -O exit 2> /dev/null ; rm -rf $temp_dir" INT QUIT TERM EXIT

    wlvncc localhost $port_to_use $@
}

main $@
