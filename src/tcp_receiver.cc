#include "tcp_receiver.hh"

using namespace std;

void TCPReceiver::receive(TCPSenderMessage message) {
  // Handle RST flag
  if (message.RST) {
    reassembler_.reader().set_error();
    return;
  }

  // Process SYN flag (connection establishment)
  if (message.SYN && !isn_.has_value()) {
    isn_ = message.seqno;
  }
  
  // Don't process data until we've received a SYN
  if (!isn_.has_value()) {
    return;
  }
  
  // Calculate absolute sequence number and stream index
  const uint64_t checkpoint = reassembler_.writer().bytes_pushed();
  const uint64_t abs_seqno = message.seqno.unwrap(isn_.value(), checkpoint);
  
  // Determine the stream index (0-indexed position in the stream)
  const uint64_t stream_index = message.SYN ? 0 : abs_seqno - 1;
  
  // Insert data into reassembler
  reassembler_.insert(stream_index, message.payload, message.FIN);
}

TCPReceiverMessage TCPReceiver::send() const {
  TCPReceiverMessage msg;
  
  // set ackno only if we've established a connection (received SYN)
  if (isn_.has_value()) {
    // calculate absolute ackno: SYN(1) + bytes_pushed + FIN(if applicable)
    const uint64_t abs_ackno = 1 + reassembler_.writer().bytes_pushed() 
                              + (reassembler_.writer().is_closed() ? 1 : 0);
    
    // Convert to 32-bit wrapped sequence number
    msg.ackno = Wrap32::wrap(abs_ackno, isn_.value());
  }
  
  // Set window size (cap at max uint16_t value)
  const uint64_t window_size = reassembler_.reader().writer().available_capacity();
  msg.window_size = static_cast<uint16_t>(min(window_size, static_cast<uint64_t>(UINT16_MAX)));
  
  // Set RST flag if stream has an error
  msg.RST = reassembler_.reader().has_error();
  
  return msg;
}