#include "tcp_sender.hh"
#include "debug.hh"
#include "tcp_config.hh"

#include <algorithm>

using namespace std;

// This function is for testing only; don't add extra state to support it.
uint64_t TCPSender::sequence_numbers_in_flight() const
{
  return bytes_in_flight_;
}

// This function is for testing only; don't add extra state to support it.
uint64_t TCPSender::consecutive_retransmissions() const
{
  return consecutive_retransmissions_;
}

void TCPSender::push( const TransmitFunction& transmit )
{
  // calculate the current window size
  // special case: treat window size of 0 as 1 for the purpose of sending
  const uint16_t effective_window = window_size_ == 0 ? 1 : window_size_;
  const uint64_t available_window = effective_window > bytes_in_flight_ ? effective_window - bytes_in_flight_ : 0;

  // if the window size is 0, we cannot send any data
  if ( available_window == 0 ) {
    debug( "Window size is 0, cannot send data" );
    return;
  }

  // if we haven't send SYN yet, that's the first thing to send
  if ( !syn_sent_ ) {
    TCPSenderMessage syn_msg;
    syn_msg.seqno = isn_;  // Use the ISN directly for first segment
    syn_msg.SYN = true;

    // update state;
    syn_sent_ = true;
    next_seqno_ += 1; // SYN consumes one sequence number
    bytes_in_flight_ += 1;

    // store the outstanding segment
    outstanding_segments_.push( { syn_msg, total_time_elapsed_ms_ } );

    // send the SYN message
    transmit( syn_msg );

    // start the timer if it's not running
    if ( !timer_running_ ) {
      timer_running_ = true;
      time_last_segment_sent_ms_ = total_time_elapsed_ms_;
    }
    return; // only send SYN in this call
  }

  // try to fill the window with data segments
  uint64_t current_available_window = available_window;
  while ( current_available_window > 0 ) {
    // determine if we can add FIN (if stream is closed but FIN not yet sent)
    const bool can_add_fin = input_.writer().is_closed() && !fin_sent_;

    // check if we have data to read
    const bool has_data = input_.reader().bytes_buffered() > 0;

    // if no data and can't send FIN we're done
    if ( !has_data && !can_add_fin ) {
      debug( "No data to send and FIN not set, stopping" );
      break;
    }

    // create a new message
    TCPSenderMessage msg;
    msg.seqno = Wrap32(next_seqno_);

    // calculate how much data we can include in this segment
    uint64_t payload_size
      = min( current_available_window,
             min( static_cast<uint64_t>( TCPConfig::MAX_PAYLOAD_SIZE ), input_.reader().bytes_buffered() ) );

    // read data from the stream if we can send some
    if ( payload_size > 0 ) {
      string data;
      read( input_.reader(), payload_size, data );
      msg.payload = data;
    }

    // add FIN if appropriate (stream closed, FIN not sent yet, and room in window)
    if ( can_add_fin && ( msg.payload.size() + 1 <= current_available_window ) ) {
      msg.FIN = true;
      fin_sent_ = true;
    }

    // get the sequence length of this segment
    const size_t sequence_length = msg.sequence_length();

    // don't send empty segments
    if ( sequence_length == 0 ) {
      debug( "Empty segment, not sending" );
      break;
    }

    // update state
    next_seqno_ += sequence_length;
    bytes_in_flight_ += sequence_length;

    // store the outstanding segment
    outstanding_segments_.push( { msg, total_time_elapsed_ms_ } );

    // send the message
    transmit( msg );

    // start the timer if it's not running
    if ( !timer_running_ ) {
      timer_running_ = true;
      time_last_segment_sent_ms_ = total_time_elapsed_ms_;
    }
    
    // Update available window for next iteration
    if (sequence_length >= current_available_window) {
      break;
    }
    current_available_window -= sequence_length;
  }
}

TCPSenderMessage TCPSender::make_empty_message() const
{
  // create an empty message with the current sequence number
  TCPSenderMessage msg;
  msg.seqno = Wrap32( next_seqno_ );
  return msg;
}

void TCPSender::receive( const TCPReceiverMessage& msg )
{
  // update window size from receiver's message
  window_size_ = msg.window_size;

  // process acknowledgement number if present
  if ( msg.ackno.has_value()) {
    // Get the wrapped ackno value
    Wrap32 wrapped_ackno = msg.ackno.value();
    
    // Calculate the absolute (unwrapped) ackno value
    uint64_t abs_ackno = wrapped_ackno.unwrap(isn_, next_seqno_);
    
    // Check if the abs_ackno is greater than next_seqno_ (impossible ackno)
    if (abs_ackno > next_seqno_) {
      // Ignore impossible acknos completely
      debug("Ignoring impossible ackno that's beyond next_seqno");
      return;
    }
    
    // Only process if this is a new ackno
    if (abs_ackno > ackno_) {
      // update our record of the highest acknowledged sequence number
      ackno_ = abs_ackno;

      // new acknowledgement received - reset RTO to initial value
      current_RTO_ms_ = initial_RTO_ms_;

      // reset retransmission counter since we got a new ack
      consecutive_retransmissions_ = 0;

      // remove acknowledged segments from the queue
      while ( !outstanding_segments_.empty() ) {
        const auto& segment = outstanding_segments_.front();
        const uint64_t segment_seqno = segment.message.seqno.unwrap( isn_, next_seqno_ );
        const uint64_t segment_end = segment_seqno + segment.message.sequence_length();

        // if this segment is fully acknowledged, remove it
        if ( segment_end <= abs_ackno ) {
          bytes_in_flight_ -= segment.message.sequence_length();
          outstanding_segments_.pop();
        } else {
          break; // stop processing if we reach a segment that is not fully acknowledged
        }
      }

      // update timer state
      if ( outstanding_segments_.empty() ) {
        // if no more outstanding segments, stop the timer
        timer_running_ = false;
      } else if ( timer_running_ ) {
        // if timer was running and we received new ACK but still have segments, restart timer
        time_last_segment_sent_ms_ = total_time_elapsed_ms_;
      }
    }
  }
}

void TCPSender::tick( uint64_t ms_since_last_tick, const TransmitFunction& transmit )
{
  // update the total elapsed time
  total_time_elapsed_ms_ += ms_since_last_tick;

  // check if the retransmission timer is running and we have outstanding segments
  if ( timer_running_ && !outstanding_segments_.empty() ) {
    // check if the timer has expired
    if ( total_time_elapsed_ms_ - time_last_segment_sent_ms_ >= current_RTO_ms_ ) {
      // retransmit the first segment in the queue
      transmit( outstanding_segments_.front().message );

      // only update retransmission count and double RTO if window size is not zero
      if ( window_size_ > 0 ) {
        // increment the consecutive retransmission count
        consecutive_retransmissions_++;

        // exponential backoff: double the RTO
        current_RTO_ms_ *= 2;
      }

      // reset the timer
      time_last_segment_sent_ms_ = total_time_elapsed_ms_;
    }
  }
}