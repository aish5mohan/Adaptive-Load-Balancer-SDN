"""Small deterministic Mininet topology for the first SDN milestone."""

from mininet.topo import Topo


class AdaptiveLoadBalancerTopo(Topo):
    """One client and three TCP servers connected to one OpenFlow switch."""

    def build(self):
        switch = self.addSwitch("s1", protocols="OpenFlow13")
        client = self.addHost(
            "h1", ip="10.0.0.1/24", mac="00:00:00:00:00:01"
        )
        server1 = self.addHost(
            "h2", ip="10.0.0.2/24", mac="00:00:00:00:00:02"
        )
        server2 = self.addHost(
            "h3", ip="10.0.0.3/24", mac="00:00:00:00:00:03"
        )
        server3 = self.addHost(
            "h4", ip="10.0.0.4/24", mac="00:00:00:00:00:04"
        )

        # Explicit switch ports are part of the controller's backend map.
        self.addLink(client, switch, port2=1)
        self.addLink(server1, switch, port2=2)
        self.addLink(server2, switch, port2=3)
        self.addLink(server3, switch, port2=4)


topos = {"albt": AdaptiveLoadBalancerTopo}
