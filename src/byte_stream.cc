#include "byte_stream.hh"

using namespace std;

ByteStream::ByteStream( uint64_t capacity ) : capacity_( capacity ) {}

void Writer::push( string data )
{
  // If the stream is closed, we cannot push any more data.
  if (is_closed()) {
    return;
  }

  // calculate the number of bytes we can push
  size_t can_accept = min(available_capacity(), data.size());
  
  // only push as much as we can
  if (can_accept > 0) {
    buffer_.append(data.substr(0, can_accept));
    bytes_pushed_ += can_accept;
  }
}

void Writer::close()
{
  closed_ = true; // Set the closed status to true.
}

bool Writer::is_closed() const
{
  return closed_; // Return the closed status of the stream.
}

uint64_t Writer::available_capacity() const
{
  // Calculate the available capacity
  return capacity_ - (bytes_pushed_ - bytes_popped_); // Return the available capacity.
}

uint64_t Writer::bytes_pushed() const
{
  return bytes_pushed_; // Return the total number of bytes pushed to the stream.
}

string_view Reader::peek() const
{
  return string_view(buffer_); // Return a view of the current buffer.
}

void Reader::pop( uint64_t len )
{
  // ensure we don't pop more than we have
  len = min(len, bytes_buffered());

  buffer_.erase(0, len); // Remove `len` bytes from the buffer.
  bytes_popped_ += len; // Update the total number of bytes popped.
}

bool Reader::is_finished() const
{
  // Check if the stream is finished (closed and fully popped)
  return closed_ && bytes_buffered() == 0;
}

uint64_t Reader::bytes_buffered() const
{
  return bytes_pushed_ - bytes_popped_; // Return the number of bytes currently buffered.
}

uint64_t Reader::bytes_popped() const
{
  return bytes_popped_; // Return the total number of bytes popped from the stream.
}

