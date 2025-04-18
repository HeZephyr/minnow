#include "wrapping_integers.hh"
#include "debug.hh"

using namespace std;

// Convert absolute sequence number -> relative sequence number
Wrap32 Wrap32::wrap(uint64_t n, Wrap32 zero_point)
{
  // add the absolute sequence number to the ISN and take the lower 32 bits
  return Wrap32{static_cast<uint32_t>(n + zero_point.raw_value_)};
}

// Convert relative sequence number -> absolute sequence number
uint64_t Wrap32::unwrap(Wrap32 zero_point, uint64_t checkpoint) const
{
  // 1. Calculate the offset between the current 32-bit serial number and zero_point
  // This utilizes the wrap-around property of 32-bit unsigned integers
  uint32_t offset = raw_value_ - zero_point.raw_value_;
  
  // 2. calculate the "era" (this 2^32 cycle) where the checkpoint is located.
  uint64_t checkpoint_era = checkpoint >> 32;
  
  // 3. create three possible candidate values: the current era, the previous era, and the next era.
  uint64_t candidate1 = (checkpoint_era << 32) + offset;  // the current era
  uint64_t candidate2 = candidate1 + (1ULL << 32);        // the next era
  uint64_t candidate3 = 0;                                // the previous era
  if (checkpoint_era > 0) {
    candidate3 = candidate1 - (1ULL << 32);
  }
  
  // 4. calculate the distance of each candidate value from the checkpoint.
  uint64_t distance1 = (candidate1 > checkpoint) ? (candidate1 - checkpoint) : (checkpoint - candidate1);
  uint64_t distance2 = (candidate2 > checkpoint) ? (candidate2 - checkpoint) : (checkpoint - candidate2);
  uint64_t distance3 = (checkpoint_era > 0) ? 
    ((candidate3 > checkpoint) ? (candidate3 - checkpoint) : (checkpoint - candidate3)) : 
    UINT64_MAX; // If the previous era cannot be used, set a maximum value.
  
  // 5. return the candidate value closest to the checkpoint.
  if (distance1 <= distance2 && distance1 <= distance3) {
    return candidate1;
  } else if (distance2 <= distance1 && distance2 <= distance3) {
    return candidate2;
  } else {
    return candidate3;
  }
}