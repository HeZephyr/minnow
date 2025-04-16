#include "reassembler.hh"

using namespace std;

Reassembler::Reassembler( ByteStream&& output )
  : output_( std::move( output ) ), next_index_( 0 ), eof_( false ), eof_index_( 0 ), unassembled_()
{}

void Reassembler::insert(uint64_t first_index, string data, bool is_last_substring)
{
    // handling the empty string case
    if (data.empty()) {
        if (is_last_substring) {
            eof_ = true;
            eof_index_ = first_index;
            
            // handling the empty string case
            if (next_index_ == eof_index_) {
                output_.writer().close();
            }
        }
        return;
    }
    
    // updating EOF information
    if (is_last_substring) {
        eof_ = true;
        eof_index_ = first_index + data.size();
    }
    
    // discard substrings that are completely within the scope of what has been processed
    if (first_index + data.size() <= next_index_) {
        // check if the substring is the last one, if so, close the stream
        if (eof_ && next_index_ == eof_index_) {
            output_.writer().close();
        }
        return;
    }
    
    // calculate ByteStream available capacity
    uint64_t available_capacity = output_.writer().available_capacity();
    
    // calculate the end position of this substring within the acceptable range
    uint64_t acceptable_end = next_index_ + available_capacity;
    
    // if the starting position of the substring exceeds the acceptable range, discard
    if (first_index >= acceptable_end) {
        return;
    }
    
    // extract the portion within the acceptable range from the substring
    uint64_t actual_start = max(first_index, next_index_);
    uint64_t actual_end = min(first_index + data.size(), acceptable_end);
    
    // offset and length of the valid part in the substring
    uint64_t offset = actual_start - first_index;
    uint64_t length = actual_end - actual_start;
    
    // extract the available portion
    string usable_data = data.substr(offset, length);
    uint64_t usable_index = actual_start;
    
    // if it can be written directly to the output stream
    if (usable_index == next_index_) {
        output_.writer().push(usable_data);
        next_index_ += usable_data.size();
        
        // check if there are more substrings that can be written.
        while (!unassembled_.empty()) {
            auto it = unassembled_.begin();
            if (it->first > next_index_) {
                break; // there are holes, cannot continue writing.
            }
            
            // calculate how many new bytes are in this substring.
            uint64_t overlap = next_index_ - it->first;
            if (overlap < it->second.size()) {
                string new_data = it->second.substr(overlap);
                output_.writer().push(new_data);
                next_index_ += new_data.size();
            }
            
            // remove the processed substring
            unassembled_.erase(it);
        }
    } else if (usable_data.size() > 0) {
        // add substring to the unassembled map 
        // first check for overlapping substrings, which may need to be merged
        auto it = unassembled_.lower_bound(usable_index);
        
        // check the preceding substring
        if (it != unassembled_.begin()) {
            auto prev_it = prev(it);
            uint64_t prev_end = prev_it->first + prev_it->second.size();
            
            // if the preceding substring overlaps or is adjacent to the current substring
            if (prev_end >= usable_index) {
                // extend the preceding substring
                if (prev_end < usable_index + usable_data.size()) {
                    uint64_t extend_len = usable_data.size() - (prev_end - usable_index);
                    prev_it->second += usable_data.substr(usable_data.size() - extend_len);
                }
                
                // if completely covered by the preceding substring, there is no need to store
                if (prev_end >= usable_index + usable_data.size()) {
                    // check if completed
                    if (eof_ && next_index_ == eof_index_) {
                        output_.writer().close();
                    }
                    return;
                }
                
                // update the pending substring information
                usable_index = prev_it->first;
                usable_data = prev_it->second;
                unassembled_.erase(prev_it);
            }
        }
        
        // check and merge the following substring
        while (it != unassembled_.end() && it->first <= usable_index + usable_data.size()) {
            uint64_t it_end = it->first + it->second.size();
            
            // if the following substring extends beyond the current substring, merge it
            if (it_end > usable_index + usable_data.size()) {
                uint64_t extend_len = it_end - (usable_index + usable_data.size());
                usable_data += it->second.substr(it->second.size() - extend_len);
            }
            
            auto to_erase = it;
            ++it;
            unassembled_.erase(to_erase);
        }
        
        // store the merged substring
        unassembled_[usable_index] = usable_data;
    }
    
    // check if all data has been processed
    if (eof_ && next_index_ == eof_index_) {
        output_.writer().close();
    }
}

uint64_t Reassembler::count_bytes_pending() const
{
    uint64_t total = 0;
    for (const auto& [index, data] : unassembled_) {
        // only calculate the unprocessed bytes
        if (index + data.size() > next_index_) {
            uint64_t valid_start = max(index, next_index_);
            total += (index + data.size()) - valid_start;
        }
    }
    return total;
}