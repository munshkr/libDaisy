#include "midi_parser.h"

using namespace daisy;

bool MidiParser::Parse(uint8_t byte, MidiEvent* event_out)
{
    // reset parser when status byte is received
    bool did_parse = false;

    if((byte & kStatusByteMask) && pstate_ != ParserSysEx)
    {
        pstate_ = ParserEmpty;
    }
    switch(pstate_)
    {
        case ParserEmpty:
            // check byte for valid Status Byte
            if(byte & kStatusByteMask)
            {
                // Get MessageType, and Channel
                incoming_message_.channel = byte & kChannelMask;
                incoming_message_.type
                    = static_cast<MidiMessageType>((byte & kMessageMask) >> 4);
                if((byte & 0xF8) == 0xF8)
                    incoming_message_.type = SystemRealTime;

                // Validate, and move on.
                if(incoming_message_.type < MessageLast)
                {
                    pstate_ = ParserHasStatus;

                    if(incoming_message_.type == SystemCommon)
                    {
                        incoming_message_.channel = 0;
                        incoming_message_.sc_type
                            = static_cast<SystemCommonType>(byte & 0x07);
                        //sysex
                        if(incoming_message_.sc_type == SystemExclusive)
                        {
                            pstate_ = ParserSysEx;
                        }
                        //short circuit
                        else if(incoming_message_.sc_type > SongSelect)
                        {
                            pstate_ = ParserEmpty;
                            if(event_out != nullptr)
                            {
                                *event_out = incoming_message_;
                            }
                            did_parse = true;
                        }
                    }
                    else if(incoming_message_.type == SystemRealTime)
                    {
                        incoming_message_.srt_type
                            = static_cast<SystemRealTimeType>(
                                byte & kSystemRealTimeMask);

                        //short circuit to start
                        pstate_ = ParserEmpty;
                        if(event_out != nullptr)
                        {
                            *event_out = incoming_message_;
                        }
                        did_parse = true;
                    }
                    else // Channel Voice or Channel Mode
                    {
                        running_status_ = incoming_message_.type;
                    }
                }
                // Else we'll keep waiting for a valid incoming status byte
            }
            else
            {
                // Handle as running status
                incoming_message_.type    = running_status_;
                incoming_message_.data[0] = byte & kDataByteMask;
                //check for single byte running status, really this only applies to channel pressure though
                if(running_status_ == ChannelPressure
                   || running_status_ == ProgramChange
                   || incoming_message_.sc_type == MTCQuarterFrame
                   || incoming_message_.sc_type == SongSelect)
                {
                    //Send the single byte update
                    pstate_ = ParserEmpty;
                    if(event_out != nullptr)
                    {
                        *event_out = incoming_message_;
                    }
                    did_parse = true;
                }
                else
                {
                    pstate_ = ParserHasData0; //we need to get the 2nd byte yet.
                }
            }
            break;
        case ParserHasStatus:
            if((byte & kStatusByteMask) == 0)
            {
                incoming_message_.data[0] = byte & kDataByteMask;
                if(running_status_ == ChannelPressure
                   || running_status_ == ProgramChange
                   || incoming_message_.sc_type == MTCQuarterFrame
                   || incoming_message_.sc_type == SongSelect)
                {
                    //these are just one data byte, so we short circuit back to start
                    pstate_ = ParserEmpty;
                    if(event_out != nullptr)
                    {
                        *event_out = incoming_message_;
                    }
                    did_parse = true;
                }
                else
                {
                    pstate_ = ParserHasData0;
                }

                //ChannelModeMessages (reserved Control Changes)
                if(running_status_ == ControlChange
                   && incoming_message_.data[0] > 119)
                {
                    incoming_message_.type    = ChannelMode;
                    running_status_           = ChannelMode;
                    incoming_message_.cm_type = static_cast<ChannelModeType>(
                        incoming_message_.data[0] - 120);
                }
            }
            else
            {
                // invalid message go back to start ;p
                pstate_ = ParserEmpty;
            }
            break;
        case ParserHasData0:
            if((byte & kStatusByteMask) == 0)
            {
                incoming_message_.data[1] = byte & kDataByteMask;

                //velocity 0 NoteOns are NoteOffs
                if(running_status_ == NoteOn && incoming_message_.data[1] == 0)
                {
                    incoming_message_.type = NoteOff;
                }

                // At this point the message is valid, and we can complete this MidiEvent
                if(event_out != nullptr)
                {
                    *event_out = incoming_message_;
                }
                did_parse = true;
            }
            else
            {
                // invalid message go back to start ;p
                pstate_ = ParserEmpty;
            }
            // Regardless, of whether the data was valid or not we go back to empty
            // because either the message is queued for handling or its not.
            pstate_ = ParserEmpty;
            break;
        case ParserSysEx:
            // end of sysex
            if(byte == 0xf7)
            {
                if(sysex_overflow_)
                {
                    sysex_buf_.Flush();
                    sysex_chunk_len_   = 0;
                    sysex_chunk_count_ = 0;
                    sysex_overflow_    = false;
                }
                else
                {
                    pstate_ = ParserEmpty;
                    produceSysexChunk(event_out, true);
                    did_parse = true;
                }
            }
            else
            {
                if(!sysex_overflow_ && sysex_buf_.writable() > 0)
                {
                    sysex_buf_.Write(byte);
                    if(++sysex_chunk_len_ >= SYSEX_BUF_CHUNK_LEN)
                    {
                        produceSysexChunk(event_out, false);
                        did_parse = true;
                    }
                }
                else
                {
                    // TODO: this could probably be handled better -
                    // if client is not consuming bytes fast enough (or at all)
                    // ignore bytes until end of packet, then purge ringbuf
                    sysex_overflow_ = true;
                }
            }
            break;
        default: break;
    }

    return did_parse;
}

void MidiParser::Reset()
{
    pstate_                       = ParserEmpty;
    sysex_chunk_len_              = 0;
    sysex_chunk_count_            = 0;
    sysex_overflow_               = false;
    incoming_message_.type        = MessageLast;
    incoming_message_.sysex_chunk = SysexChunk<>();
}

void MidiParser::produceSysexChunk(MidiEvent* event_out, bool msg_ended)
{
    auto type = SysexChunk<>::Type::SeqIntermediate;
    if(sysex_chunk_count_ == 0)
    {
        if(msg_ended)
        {
            type = SysexChunk<>::Type::Individual;
        }
        else
        {
            type = SysexChunk<>::Type::SeqFirst;
            sysex_chunk_count_++;
        }
    }
    else if(msg_ended)
    {
        sysex_chunk_count_ = 0;
        type               = SysexChunk<>::Type::SeqLast;
    }
    else
    {
        sysex_chunk_count_++;
    }

    if(event_out != nullptr)
    {
        *event_out = incoming_message_;
        event_out->sysex_chunk
            = SysexChunk<>(type, &sysex_buf_, sysex_chunk_len_);
    }

    sysex_chunk_len_ = 0;
}
