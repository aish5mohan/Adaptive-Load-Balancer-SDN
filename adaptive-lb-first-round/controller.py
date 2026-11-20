"""First-round Ryu controller: OpenFlow 1.3 TCP round-robin backend selection.

Clients connect to virtual IP 10.0.0.100:5000. For each new TCP flow, the
controller rewrites the destination to one of three server hosts and installs
forward and reverse rules. This is the static SDN baseline; adaptive metrics
are intentionally a later milestone.
"""

import itertools
import logging
import socket

from ryu.base import app_manager
from ryu.controller import ofp_event
from ryu.controller.handler import CONFIG_DISPATCHER, MAIN_DISPATCHER, set_ev_cls
from ryu.ofproto import ofproto_v1_3
from ryu.lib.packet import packet, ethernet, ipv4, tcp, ether_types


LOG = logging.getLogger("adaptive_lb")
VIRTUAL_IP = "10.0.0.100"
VIRTUAL_MAC = "00:00:00:00:00:64"
SERVICE_PORT = 5000
CLIENT_PORT = 1
BACKENDS = (
    {"ip": "10.0.0.2", "mac": "00:00:00:00:00:02", "port": 2},
    {"ip": "10.0.0.3", "mac": "00:00:00:00:00:03", "port": 3},
    {"ip": "10.0.0.4", "mac": "00:00:00:00:00:04", "port": 4},
)


class RoundRobinLoadBalancer(app_manager.RyuApp):
    OFP_VERSIONS = [ofproto_v1_3.OFP_VERSION]

    def __init__(self, *args, **kwargs):
        super(RoundRobinLoadBalancer, self).__init__(*args, **kwargs)
        self.backend_cycle = itertools.cycle(BACKENDS)
        self.routes = {}
        self.mac_to_port = {}

    @set_ev_cls(ofp_event.EventOFPSwitchFeatures, CONFIG_DISPATCHER)
    def switch_features_handler(self, event):
        datapath = event.msg.datapath
        parser = datapath.ofproto_parser
        ofproto = datapath.ofproto
        # Send unknown packets to the controller for backend selection.
        match = parser.OFPMatch()
        actions = [parser.OFPActionOutput(ofproto.OFPP_CONTROLLER,
                                          ofproto.OFPCML_NO_BUFFER)]
        self._add_flow(datapath, 0, match, actions)
        LOG.info("OpenFlow 1.3 switch connected: dpid=%s", datapath.id)

    @set_ev_cls(ofp_event.EventOFPPacketIn, MAIN_DISPATCHER)
    def packet_in_handler(self, event):
        message = event.msg
        datapath = message.datapath
        parser = datapath.ofproto_parser
        ofproto = datapath.ofproto
        in_port = message.match.get("in_port")

        pkt = packet.Packet(message.data)
        eth = pkt.get_protocol(ethernet.ethernet)
        ip = pkt.get_protocol(ipv4.ipv4)
        segment = pkt.get_protocol(tcp.tcp)
        if eth is None:
            return

        learned = self.mac_to_port.setdefault(datapath.id, {})
        learned[eth.src] = in_port

        if (ip is not None and ip.dst == VIRTUAL_IP and segment is not None and
                segment.dst_port == SERVICE_PORT and in_port == CLIENT_PORT):
            self._handle_new_client_packet(
                datapath, message, in_port, eth, ip, segment
            )
            return

        # Basic learning-switch behavior keeps ARP and pingall useful during
        # topology setup and provides ordinary L2 forwarding outside the VIP.
        out_port = learned.get(eth.dst)
        if out_port is None:
            out_port = datapath.ofproto.OFPP_FLOOD
        actions = [datapath.ofproto_parser.OFPActionOutput(out_port)]
        if out_port != datapath.ofproto.OFPP_FLOOD:
            match = datapath.ofproto_parser.OFPMatch(
                in_port=in_port, eth_src=eth.src, eth_dst=eth.dst
            )
            self._add_flow(datapath, 1, match, actions, idle_timeout=60)
        packet_out = datapath.ofproto_parser.OFPPacketOut(
            datapath=datapath,
            buffer_id=datapath.ofproto.OFP_NO_BUFFER,
            in_port=in_port,
            actions=actions,
            data=message.data,
        )
        datapath.send_msg(packet_out)
        if ip is None or segment is None:
            return

        # Once a flow has been installed, response packets should not reach
        # this handler. Log unexpected TCP packets to make setup errors visible.
        if segment is not None:
            LOG.warning("Unmatched TCP packet: %s:%s -> %s:%s",
                        ip.src, segment.src_port, ip.dst, segment.dst_port)

    def _handle_new_client_packet(self, datapath, message, in_port, eth, ip, segment):
        parser = datapath.ofproto_parser
        ofproto = datapath.ofproto
        backend = next(self.backend_cycle)
        client_ip = ip.src
        client_mac = eth.src
        client_port = segment.src_port
        route_key = (datapath.id, client_ip, client_port, SERVICE_PORT)
        self.routes[route_key] = backend

        forward_match = parser.OFPMatch(
            in_port=in_port,
            eth_type=ether_types.ETH_TYPE_IP,
            ip_proto=socket.IPPROTO_TCP,
            ipv4_src=client_ip,
            ipv4_dst=VIRTUAL_IP,
            tcp_src=client_port,
            tcp_dst=SERVICE_PORT,
        )
        forward_actions = [
            parser.OFPActionSetField(eth_dst=backend["mac"]),
            parser.OFPActionSetField(ipv4_dst=backend["ip"]),
            parser.OFPActionOutput(backend["port"]),
        ]

        reverse_match = parser.OFPMatch(
            in_port=backend["port"],
            eth_type=ether_types.ETH_TYPE_IP,
            ip_proto=socket.IPPROTO_TCP,
            ipv4_src=backend["ip"],
            ipv4_dst=client_ip,
            tcp_src=SERVICE_PORT,
            tcp_dst=client_port,
        )
        reverse_actions = [
            parser.OFPActionSetField(eth_src=VIRTUAL_MAC),
            parser.OFPActionSetField(eth_dst=client_mac),
            parser.OFPActionSetField(ipv4_src=VIRTUAL_IP),
            parser.OFPActionOutput(CLIENT_PORT),
        ]

        # Install both directions before forwarding the first packet.
        self._add_flow(datapath, 20, forward_match, forward_actions, idle_timeout=300)
        self._add_flow(datapath, 20, reverse_match, reverse_actions, idle_timeout=300)

        packet_actions = [
            parser.OFPActionSetField(eth_dst=backend["mac"]),
            parser.OFPActionSetField(ipv4_dst=backend["ip"]),
            parser.OFPActionOutput(backend["port"]),
        ]
        packet_out = parser.OFPPacketOut(
            datapath=datapath,
            buffer_id=ofproto.OFP_NO_BUFFER,
            in_port=in_port,
            actions=packet_actions,
            data=message.data,
        )
        datapath.send_msg(packet_out)
        LOG.info("Assigned %s:%s -> %s:%s to backend %s (%s)",
                 client_ip, client_port, VIRTUAL_IP, SERVICE_PORT,
                 backend["ip"], backend["port"])

    @staticmethod
    def _add_flow(datapath, priority, match, actions, idle_timeout=0):
        parser = datapath.ofproto_parser
        ofproto = datapath.ofproto
        instructions = [parser.OFPInstructionActions(
            ofproto.OFPIT_APPLY_ACTIONS, actions
        )]
        flow = parser.OFPFlowMod(
            datapath=datapath,
            priority=priority,
            match=match,
            instructions=instructions,
            idle_timeout=idle_timeout,
        )
        datapath.send_msg(flow)
