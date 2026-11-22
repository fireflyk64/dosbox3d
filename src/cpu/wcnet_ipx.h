/*
 *  The room's IPX network (docs/armada.md): every machine of a room is a node of one IPX network, its packets carried
 *  by the room's links instead of DOSBox's UDP tunnel (IPXNET).
 *
 *  Nothing of a game is hooked.  The emulator's IPX driver (src/hardware/ipx.cpp: interrupt 7Ah, the multiplexer's
 *  7A00h, the sockets, the event control blocks, the service routines) is DOSBox's; only where a packet goes is
 *  ours.  Anything written for IPX runs on it: Wing Commander Armada's NetBIOS (Novell's NETBIOS.EXE, which GOG ships
 *  with the game, on top of IPX), its Proving Grounds (IPX itself).
 *
 *  The host is seat 0 (the room's, or a rig's TCP listener); a guest opens a link to it.  A packet goes whole, its IPX
 *  header as the program wrote it: the host passes on what is for a guest, and a broadcast to everybody.  Reliable
 *  and in order by default (IPX promises neither, but programs written for a quiet LAN rarely cope with a lost
 *  packet; NetBIOS asks again for its own); WCIPXLOSSY=1 sends them best effort.
 */
#ifndef WCNET_IPX_H_
#define WCNET_IPX_H_

#include "dosbox.h"

namespace wc {

typedef void (*IpxReceiver)(Bit8u *packet, Bit16s len);

// A room (WCROOM) or a rig's peer (WCHOST, WCPORT) to carry IPX over, and not alone (WCSOLO=1, WCNET=0).
bool ipx_room_configured();
// The node this machine is on the room's network: made once (WCIPXNODE=<12 hex digits> for a rig).
void ipx_room_node(Bit8u node[6]);
// IPX's start: packets for this machine go to `rx` (from ipx_room_tick).
void ipx_room_init(IpxReceiver rx);
// Every emulated millisecond: the room found and its links looked at.
void ipx_room_tick();
// A packet of this machine's for the others (its header says where: a node, or every node).
void ipx_room_send(const Bit8u *packet, int len);
void ipx_room_shutdown();
// For the log and the page: "room WC-4821: 2 machines" etc.
const char *ipx_room_line();

}  // namespace wc

#endif
