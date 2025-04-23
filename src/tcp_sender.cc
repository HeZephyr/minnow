#include "tcp_sender.hh"
#include "tcp_config.hh"

using namespace std;

// Test accessor for outstanding sequence numbers
uint64_t TCPSender::sequence_numbers_in_flight() const
{
  return bytes_in_flight_;
}

// Test accessor for retransmission counter
uint64_t TCPSender::consecutive_retransmissions() const
{
  return consecutive_retransmissions_;
}

void TCPSender::push( const TransmitFunction& transmit )
{
  // Calculate effective window size (treat 0 window as 1 for window probing)
  const uint16_t effective_window = window_size_ ? window_size_ : 1;

  // Continue sending while window allows and FIN not yet sent
  while ( bytes_in_flight_ < effective_window && !fin_sent_ ) {
    TCPSenderMessage msg = make_empty_message();

    // Set SYN flag if connection not yet initiated
    if ( !syn_sent_ ) {
      msg.SYN = true;
      syn_sent_ = true;
    }

    // Calculate available payload space considering window and existing flight
    const uint64_t remaining_capacity = effective_window - bytes_in_flight_;
    const size_t max_payload = min( remaining_capacity - msg.sequence_length(), // Account for SYN/FIN
                                    TCPConfig::MAX_PAYLOAD_SIZE );

    // Fill payload from input stream without exceeding capacity
    while ( reader().bytes_buffered() && msg.payload.size() < max_payload ) {
      const string_view data = reader().peek().substr( 0, max_payload - msg.payload.size() );
      msg.payload += data;
      reader().pop( data.size() );
    }

    // Set FIN flag if stream ended and window allows
    if ( !fin_sent_ && reader().is_finished() && ( remaining_capacity > msg.sequence_length() ) ) {
      msg.FIN = true;
      fin_sent_ = true;
    }

    // Skip empty segments (except for SYN/FIN)
    if ( msg.sequence_length() == 0 )
      break;

    // Transmit segment and update tracking
    transmit( msg );
    next_seqno_ += msg.sequence_length();
    bytes_in_flight_ += msg.sequence_length();
    outstanding_messages_.push( move( msg ) );

    // Start retransmission timer if not running
    if ( !timer_running_ ) {
      timer_running_ = true;
      timer_ = 0;
    }
  }
}

TCPSenderMessage TCPSender::make_empty_message() const
{
  // Construct base message with current sequence number
  return {
    .seqno = Wrap32::wrap( next_seqno_, isn_ ),
    .SYN = false,
    .payload = {},
    .FIN = false,
    .RST = input_.has_error() // Propagate stream error state
  };
}

void TCPSender::receive( const TCPReceiverMessage& msg )
{
  // Handle error states
  if ( input_.has_error() )
    return;
  if ( msg.RST ) {
    input_.set_error();
    return;
  }

  // Update receiver window size
  window_size_ = msg.window_size;

  // Process acknowledgment if present
  if ( !msg.ackno )
    return;

  // Convert relative ackno to absolute sequence space
  const uint64_t ack_abs = msg.ackno->unwrap( isn_, next_seqno_ );

  // Validate acknowledgment number
  if ( ack_abs > next_seqno_ )
    return; // Acknowledges unsent data

  bool acked = false;
  // Process all completely acknowledged segments
  while ( !outstanding_messages_.empty() ) {
    const auto& front_msg = outstanding_messages_.front();
    const uint64_t segment_end = ackno_ + front_msg.sequence_length();

    if ( segment_end > ack_abs )
      break; // Partial acknowledgment

    // Update tracking information
    acked = true;
    ackno_ = segment_end;
    bytes_in_flight_ -= front_msg.sequence_length();
    outstanding_messages_.pop();
  }

  // Reset timer state if any segments were acknowledged
  if ( acked ) {
    timer_ = 0;
    current_RTO_ms_ = initial_RTO_ms_;
    consecutive_retransmissions_ = 0;
    timer_running_ = !outstanding_messages_.empty();
  }
}

void TCPSender::tick( uint64_t ms_since_last_tick, const TransmitFunction& transmit )
{
  // Update timer only when active
  if ( timer_running_ ) {
    timer_ += ms_since_last_tick;
  }

  // Check for timeout condition
  if ( timer_running_ && timer_ >= current_RTO_ms_ && !outstanding_messages_.empty() ) {
    // Retransmit oldest unacknowledged segment
    transmit( outstanding_messages_.front() );

    // Apply exponential backoff only when window is open
    if ( window_size_ > 0 ) {
      consecutive_retransmissions_++;
      current_RTO_ms_ *= 2;
    }

    // Reset timer for next potential retransmission
    timer_ = 0;
  }
}