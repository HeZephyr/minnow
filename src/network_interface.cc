#include <iostream>

#include "arp_message.hh"
#include "debug.hh"
#include "ethernet_frame.hh"
#include "exception.hh"
#include "helpers.hh"
#include "network_interface.hh"

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface( string_view name,
                                    shared_ptr<OutputPort> port,
                                    const EthernetAddress& ethernet_address,
                                    const Address& ip_address )
  : name_( name )
  , port_( notnull( "OutputPort", move( port ) ) )
  , ethernet_address_( ethernet_address )
  , ip_address_( ip_address )
{
  cerr << "DEBUG: Network interface has Ethernet address " << to_string( ethernet_address_ ) << " and IP address "
       << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but
//! may also be another host if directly connected to the same network as the destination) Note: the Address type
//! can be converted to a uint32_t (raw 32-bit IP address) by using the Address::ipv4_numeric() method.
void NetworkInterface::send_datagram( const InternetDatagram& dgram, const Address& next_hop )
{
  // Convert the Address to a numeric representation for easier lookup
  const AddressNumber next_hop_ip = next_hop.ipv4_numeric();

  // Check if the next hop's Ethernet address is already in the ARP cache
  auto arp_it = arp_cache_.find( next_hop_ip );
  if ( arp_it != arp_cache_.end() ) {
    // If found, get the Ethernet address and transmit the datagram immediately
    const EthernetAddress& next_hop_ethernet_address = arp_it->second.ethernet_address;
    transmit( { { next_hop_ethernet_address, ethernet_address_, EthernetHeader::TYPE_IPv4 }, serialize( dgram ) } );
    return;
  }

  // If not found, queue the datagram for later transmission
  pending_datagrams_[next_hop_ip].emplace_back( dgram );

  // Check if an ARP request for this IP is already pending
  if ( pending_datagram_timers_.find( next_hop_ip )!= pending_datagram_timers_.end() ) {
    // If yes, do nothing; an ARP request is already in flight
    return;
  }

  // If no ARP request is pending, send one
  // Start a timer to track the ARP request
  pending_datagram_timers_.emplace( next_hop_ip, Timer {} );

  // Construct the ARP request message
  const ARPMessage arp_request = {
    .opcode = ARPMessage::OPCODE_REQUEST,
    .sender_ethernet_address = ethernet_address_,
    .sender_ip_address = ip_address_.ipv4_numeric(),
    .target_ethernet_address = {}, // Target Ethernet address is unknown, hence left empty
    .target_ip_address = next_hop_ip
  };

  // Encapsulate the ARP request in an Ethernet frame and transmit it (broadcast)
  transmit({ { ETHERNET_BROADCAST, ethernet_address_, EthernetHeader::TYPE_ARP }, serialize(arp_request) });

}

//! \param[in] frame the incoming Ethernet frame
void NetworkInterface::recv_frame( EthernetFrame frame )
{
  // Ignore frames not destined for this interface's Ethernet address or the broadcast address
  if ( frame.header.dst != ethernet_address_ && frame.header.dst != ETHERNET_BROADCAST ) {
    return;
  }

  // Check the Ethernet frame type
  if ( frame.header.type == EthernetHeader::TYPE_IPv4 ) {
    // If it's an IPv4 datagram, try to parse it
    InternetDatagram dgram;
    if ( parse( dgram, frame.payload ) ) {
      // If parsing is successful, push the datagram onto the received queue
      datagrams_received_.push( move( dgram ) );
    }
    return; // Processing finished for IPv4 frames
  }

  if ( frame.header.type == EthernetHeader::TYPE_ARP ) {
    // If it's an ARP message, try to parse it
    ARPMessage msg;
    if ( !parse( msg, frame.payload ) ) {
      // If parsing fails, ignore the frame
      return;
    }

    // Learn the mapping from the sender's IP and Ethernet addresses
    const AddressNumber sender_ip = msg.sender_ip_address;
    const EthernetAddress sender_eth = msg.sender_ethernet_address;
    // Add or update the entry in the ARP cache, resetting its timer
    arp_cache_[sender_ip] = { sender_eth, 0 };

    // Check if this is an ARP request specifically for our IP address
    if ( msg.opcode == ARPMessage::OPCODE_REQUEST && msg.target_ip_address == ip_address_.ipv4_numeric() ) {
      // If yes, construct an ARP reply
      ARPMessage arp_reply = {
        .opcode = ARPMessage::OPCODE_REPLY,
        .sender_ethernet_address = ethernet_address_, // Our Ethernet address
        .sender_ip_address = ip_address_.ipv4_numeric(), // Our IP address
        .target_ethernet_address = sender_eth, // The original sender's Ethernet address
        .target_ip_address = sender_ip // The original sender's IP address
      };

      // Encapsulate the ARP reply in an Ethernet frame and transmit it back to the sender
      transmit({ { sender_eth, ethernet_address_, EthernetHeader::TYPE_ARP }, serialize(arp_reply) });
    }

    // Check if we have any pending datagrams waiting for this sender's Ethernet address
    auto it = pending_datagrams_.find(sender_ip);
    if ( it != pending_datagrams_.end() ) {
      // If yes, iterate through the pending datagrams
      for ( const auto& dgram : it->second ) {
        // Transmit each pending datagram using the now-known Ethernet address
        transmit({ { sender_eth, ethernet_address_, EthernetHeader::TYPE_IPv4 }, serialize(dgram) });
      }
      // Remove the entry from the pending datagrams map
      pending_datagrams_.erase(it);
      // Remove the corresponding timer for the ARP request
      pending_datagram_timers_.erase(sender_ip);
    }
  }
}


//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick( const size_t ms_since_last_tick )
{
  // Iterate through the ARP cache to expire old entries
  for ( auto it = arp_cache_.begin(); it != arp_cache_.end(); ) {
    // Increment the timer for each entry
    it->second.timer += ms_since_last_tick;
    // Check if the entry has exceeded its time-to-live (TTL)
    if ( it->second.timer >= ARP_ENTRY_TTL_ms ) {
      // If expired, remove the entry from the cache
      it = arp_cache_.erase( it );
    } else {
      // Otherwise, move to the next entry
      it++;
    }
  }

  // Iterate through the pending ARP request timers to expire old requests
  for ( auto it = pending_datagram_timers_.begin(); it != pending_datagram_timers_.end(); ) {
    // Increment the timer for each pending request
    it->second += ms_since_last_tick;
    // Check if the request has exceeded its timeout period
    if ( it->second >= ARP_REQUEST_PERIOD_ms ) {
      // If expired, remove the corresponding pending datagrams
      pending_datagrams_.erase( it->first );
      // Remove the timer entry itself
      it = pending_datagram_timers_.erase( it );
    } else {
      // Otherwise, move to the next entry
      it++;
    }
  }
}
