#pragma once

#include "byte_stream.hh"
#include "tcp_receiver_message.hh"
#include "tcp_sender_message.hh"

#include <functional>
#include <queue>

class TCPSender
{
public:
  /* Construct TCP sender with given default Retransmission Timeout and possible ISN */
  TCPSender( ByteStream&& input, Wrap32 isn, uint64_t initial_RTO_ms ) 
    : input_( std::move( input ) )
    , syn_sent_( false )
    , fin_sent_( false )
    , isn_( isn )
    , next_seqno_( isn_.unwrap( Wrap32( 0 ), 0 ) )
    , window_size_( 1 )
    , ackno_( 0 )
    , bytes_in_flight_( 0 )
    , initial_RTO_ms_( initial_RTO_ms )
    , current_RTO_ms_( initial_RTO_ms )
    , timer_running_( false )
    , total_time_elapsed_ms_( 0 )
    , time_last_segment_sent_ms_( 0 )
    , consecutive_retransmissions_( 0 )
    , outstanding_segments_()
  {}

  /* Generate an empty TCPSenderMessage */
  TCPSenderMessage make_empty_message() const;

  /* Receive and process a TCPReceiverMessage from the peer's receiver */
  void receive( const TCPReceiverMessage& msg );

  /* Type of the `transmit` function that the push and tick methods can use to send messages */
  using TransmitFunction = std::function<void( const TCPSenderMessage& )>;

  /* Push bytes from the outbound stream */
  void push( const TransmitFunction& transmit );

  /* Time has passed by the given # of milliseconds since the last time the tick() method was called */
  void tick( uint64_t ms_since_last_tick, const TransmitFunction& transmit );

  // Accessors
  uint64_t sequence_numbers_in_flight() const;  // For testing: how many sequence numbers are outstanding?
  uint64_t consecutive_retransmissions() const; // For testing: how many consecutive retransmissions have happened?
  const Writer& writer() const { return input_.writer(); }
  const Reader& reader() const { return input_.reader(); }
  Writer& writer() { return input_.writer(); }

private:
  Reader& reader() { return input_.reader(); }

  ByteStream input_;                     // the outbound stream used to read data to send
  bool syn_sent_;                        // has the SYN flag been sent?
  bool fin_sent_;                        // has the FIN flag been sent?

  Wrap32 isn_;                           // initial Sequence Number
  uint64_t next_seqno_;                  // next sequence number to be sent
  
  /* Window management */
  uint16_t window_size_;                 // size of the receiver's window
  uint64_t ackno_;                       // acknowledgment number of the receiver
  uint64_t bytes_in_flight_;             // number of bytes sent but not acknowledged

  /* Retransmission timer management */
  uint64_t initial_RTO_ms_;              // initial Retransmission Timeout in milliseconds
  uint64_t current_RTO_ms_;              // current Retransmission Timeout in milliseconds
  bool timer_running_;                   // is the timer running?
  uint64_t total_time_elapsed_ms_;       // Total time elapsed since sender started
  uint64_t time_last_segment_sent_ms_;   // time of the last segment sent
  uint64_t consecutive_retransmissions_; // number of consecutive retransmissions

  // Structure to track outstanding (sent but unacknowledged) segments
  struct OutstandingSegment {
    TCPSenderMessage message; // the segment message
    uint64_t time_sent_ms;    // time of the segment sent (in milliseconds)
  };

  /* Queue of outstanding segments that may need retransmission */
  std::queue<OutstandingSegment> outstanding_segments_; // queue of outstanding segments
};