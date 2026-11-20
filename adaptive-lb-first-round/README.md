# Adaptive Load Balancer: SDN + TCP Socket Project

This project is being built in stages so each layer can be demonstrated and understood independently. The rubric requires low-level socket communication, concurrency, Mininet deployment, interaction with an SDN-controlled network, reproducible tests, dynamic behavior, a challenging scenario, performance comparison, and complete documentation.

## Stage plan

1. **TCP foundation:** multiple TCP servers and a concurrent C client. Verify protocol exchange, concurrency, errors, and latency.
2. **First-round SDN demo:** a Mininet client/server topology, an OpenFlow 1.3 Ryu controller, and static round-robin assignment for new TCP connections to a virtual service IP. Verify end-to-end socket-to-SDN behavior.
3. **Adaptive monitoring:** collect server health/load and switch port/flow statistics; choose healthy low-load backends and preserve each flow's selection.
4. **Robustness:** detect overload and failed backends, stop assigning new flows to them, and recover them after health checks pass.
5. **Evaluation:** compare a fixed/static policy with the adaptive policy under normal, skewed-load, overload, and server-failure cases. Record throughput, mean and tail latency, errors, per-server distribution, and recovery time.

## 1. Ubuntu VM setup

Use the existing Ubuntu VM from the course guide (Ubuntu 20.04 or 22.04 preferred), with at least 2 CPU cores, 4 GB RAM, and about 20 GB free disk. Enable hardware virtualization and give the VM a working NAT or bridged network connection for package installation. Mininet needs root privileges for network namespaces and virtual switches. If creating a new VM, Ubuntu 22.04 LTS is a good match for the course guide.

In an Ubuntu terminal, identify the release and install the C build tools for stage 1:

```bash
lsb_release -a
sudo apt update
sudo apt install -y build-essential git
```

Copy this project directory into Ubuntu (for example, via a VirtualBox shared folder), then change into that directory. `sources/` is reference material and should remain unchanged.

Install Mininet and Open vSwitch with Ubuntu packages (recommended in the course guide):

```bash
sudo apt update
sudo apt install -y mininet openvswitch-switch iproute2
sudo systemctl enable --now openvswitch-switch
sudo mn
```

At the `mininet>` prompt, check the default topology:

```text
nodes
net
pingall
exit
```

Expect `pingall` to report 0% packet loss. You can also run it as a single check:

```bash
sudo mn --switch ovsbr --test pingall
```

For the project topology smoke check, start a simple network with one switch and three hosts:

```bash
sudo mn --topo single,3
```

Run `pingall`, then `exit`. Use `sudo mn -c` to remove leftover Mininet state after experiments, and only when no Mininet experiment is running. The source-install method (`git clone` Mininet and `sudo ./util/install.sh -a`) is an advanced alternative; use it only if the Ubuntu package install fails or the course specifically requires source. Ryu is installed in a dedicated Python virtual environment in the first-round SDN stage so its dependencies do not alter Ubuntu's system Python.

## 2. Build and run stage 1

Compile:

```bash
make
```

In terminal 1, start server 1:

```bash
./server 5001 1 100
```

In terminal 2 and terminal 3, start two more servers:

```bash
./server 5002 2 100
./server 5003 3 100
```

The last argument is simulated service time in milliseconds. It lets you observe concurrent server handling; use `0` for no artificial delay.

In terminal 4, run 20 concurrent clients, each sending 5 sequential requests to server 1:

```bash
./client 127.0.0.1 5001 20 5
```

For a repeatable local socket test, build once and run the supplied script:

```bash
make
bash stage1_demo.sh
```

The script starts all three servers, sends 100 requests to each with 20 concurrent client threads, stops server 3, and verifies that a new connection to that failed server is reported as failed. It writes timestamped terminal logs under `results/`. This local test demonstrates the socket foundation; the first-round SDN demo below demonstrates controller-based routing.

Expected checks:

- The client summary reports `attempted=100`, `succeeded=100`, and `failed=0`.
- Server 1 logs requests from different client IDs and may show `active` greater than 1.
- Each reply identifies the server process and echoes its request.
- Stopping a server and running a client against its port produces connection errors and a nonzero exit status.

Stop each server with `Ctrl+C`. The three ports make independent server processes easy to test locally. In Mininet, separate hosts will instead run the same server program on the same port.

## 3. First-round milestone: static SDN integration

The first-round demo uses `topology.py` and `controller.py`. The client connects to virtual service `10.0.0.100:5000`; Ryu assigns each new TCP connection to one of three backends in round-robin order. The switch rewrites the request destination and the reply source, so the C client sees one consistent service address. This is the **static baseline**, not the final adaptive algorithm.

### Install Ryu in an isolated environment

On Ubuntu 20.04/22.04, from the project directory:

```bash
sudo apt install -y python3-venv python3-dev gcc libffi-dev libssl-dev libxml2-dev libxslt1-dev zlib1g-dev
python3 -m venv --system-site-packages .venv-ryu
. .venv-ryu/bin/activate
python -m pip install --upgrade pip
python -m pip install 'setuptools<58' wheel
python -m pip install ryu
deactivate
```

Ryu's documentation lists `pip install ryu` as its installation route but also says Ryu is not currently maintained. If installation fails with your Ubuntu/Python version, keep the error output; the maintained OpenStack `os-ken` project is an alternative only if your instructor accepts it.

### Run the first-round demo

Compile the C programs:

```bash
make
```

In terminal 1, start the controller from the project directory:

```bash
.venv-ryu/bin/ryu-manager --ofp-tcp-listen-port 6653 controller.py
```

In terminal 2, start the custom Mininet topology. Run cleanup only when no other Mininet experiment is active:

```bash
sudo mn -c
sudo mn --custom topology.py --topo albt \
  --controller remote,ip=127.0.0.1,port=6653 \
  --switch ovs,protocols=OpenFlow13
```

At the `mininet>` prompt, verify connectivity and configure the client's virtual-service ARP entry:

```text
pingall
h1 arp -s 10.0.0.100 00:00:00:00:00:64
```

`pingall` should report 0% packet loss. Start a server in each server host, replacing `/absolute/path/to/project` with the actual Ubuntu project directory:

```text
h2 /absolute/path/to/project/server 5000 2 100 > /tmp/server2.log 2>&1 &
h3 /absolute/path/to/project/server 5000 3 100 > /tmp/server3.log 2>&1 &
h4 /absolute/path/to/project/server 5000 4 100 > /tmp/server4.log 2>&1 &
```

From the client host, generate concurrent connections to the virtual service:

```text
h1 /absolute/path/to/project/client 10.0.0.100 5000 20 5
```

Check that client replies mention server IDs 2, 3, and 4 and the summary reports 100 successful requests. The controller terminal should show assignments across backend IPs `.2`, `.3`, and `.4`. This demonstrates that OpenFlow rules are steering C TCP traffic. The controller currently uses static round robin; monitoring, adaptive choices, and rerouting around failures are later milestones.

For the first-round presentation, show the architecture, `pingall`, the controller's switch connection and backend assignment logs, and the successful client summary. Be ready to explain how the virtual service IP is rewritten to a backend and how the reply is rewritten back to the service address.

## Stage 1 protocol and design

Each TCP connection sends one newline-terminated text request (`client=<id> request=<id>`) and receives one newline-terminated response. TCP is a byte stream, so the newline is the message boundary. The server uses one detached pthread per accepted connection; the client uses one pthread per simulated client. This is intentionally a small teaching protocol, not a production protocol.

## Stage 1 troubleshooting

- `make: command not found` or `gcc: command not found`: install `build-essential`.
- `bind: Address already in use`: stop the other process already listening on that port or choose another port.
- `Connection refused`: confirm the server is still running and that the client IP and port match.
- Permission denied while starting Mininet later: run Mininet with `sudo` as required by its network namespace setup.

## Reproducibility and deliverables

Keep the topology, controller, source code, run scripts, test cases, and measured results in version control. The rubric asks for the final demo code and documentation to be shared through GitHub; publish only after reviewing the repository contents and removing generated binaries or VM-specific secrets.

For the first-round presentation, show the architecture diagram above (or draw the client/server flow), explain the one-line TCP request/reply protocol, run `stage1_demo.sh`, and keep its generated logs as evidence. State clearly that Mininet/OpenFlow integration and adaptive server selection are later milestones.

## References

- [Mininet installation guide](https://mininet.org/download/)
- [Mininet walkthrough](https://mininet.org/walkthrough/)
- [Ryu documentation](https://ryu.readthedocs.io/en/latest/)
