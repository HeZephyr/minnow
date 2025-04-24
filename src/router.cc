#include "router.hh"
#include "debug.hh"

#include <iostream>

using namespace std;

// route_prefix: The "up-to-32-bit" IPv4 address prefix to match the datagram's destination address against
// prefix_length: For this route to be applicable, how many high-order (most-significant) bits of
//    the route_prefix will need to match the corresponding bits of the datagram's destination address?
// next_hop: The IP address of the next hop. Will be empty if the network is directly attached to the router (in
//    which case, the next hop address should be the datagram's final destination).
// interface_num: The index of the interface to send the datagram out on.
void Router::add_route( const uint32_t route_prefix,
                        const uint8_t prefix_length,
                        const optional<Address> next_hop,
                        const size_t interface_num )
{
  cerr << "DEBUG: adding route " << Address::from_ipv4_numeric( route_prefix ).ip() << "/"
       << static_cast<int>( prefix_length ) << " => " << ( next_hop.has_value() ? next_hop->ip() : "(direct)" )
       << " on interface " << interface_num << "\n";

  // truncate route_prefix to prefix_length bit valid prefixes
  uint32_t masked_prefix = (prefix_length == 0) ? 0 : (route_prefix >> (32 - prefix_length));

  // Insert into routing table (sorted by prefix length)
  routing_table_[prefix_length][masked_prefix] = { interface_num, next_hop };
}

// Go through all the interfaces, and route every incoming datagram to its proper outgoing interface.
void Router::route()
{
  for ( const auto& interface : interfaces_ ) {
    auto& datagrams_received = interface->datagrams_received();

    // process all received datagrams
    while ( !datagrams_received.empty() ) {
      InternetDatagram datagram = move( datagrams_received.front() );
      datagrams_received.pop();

      // TTL ≤ 1 means that it cannot be forwarded any more, it is discarded.
      if ( datagram.header.ttl <= 1 ) {
        continue;
      }

      // TTL reduces 1 and recalculates the checksum
      datagram.header.ttl -= 1;
      datagram.header.compute_checksum();

      // Match the route entry with the longest prefix
      auto routeEntry = match( datagram.header.dst );
      if ( !routeEntry.has_value() ) {
        continue;
      }

      // If there is no next_hop, send directly to the destination IP.
      Address target = routeEntry->next_hop.value_or( Address::from_ipv4_numeric(datagram.header.dst) );

      // Send it out through the corresponding interface
      interfaces_[routeEntry->interface_num]->send_datagram( datagram, target );
    }
  }
}

// Route match function: returns the longest prefix match.
optional<Router::RouteEntry> Router::match( uint32_t dst_addr ) const noexcept
{
  for ( int len = 31; len >= 0; --len ) {
    uint32_t masked = (len == 0) ? 0 : (dst_addr >> (32 - len));
    const auto& table = routing_table_[len];
    auto it = table.find( masked );
    if ( it != table.end() ) {
      return it->second;
    }
  }
  
  return nullopt;
}
