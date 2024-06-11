// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "CANDataConsumer.h"
#include "CANDataTypes.h"
#include "CANDecoder.h"
#include "EnumUtility.h"
#include "LoggingModule.h"
#include "MessageTypes.h"
#include "TraceModule.h"
#include <algorithm>
#include <array>
#include <linux/can.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Aws
{
namespace IoTFleetWise
{

CANDataConsumer::CANDataConsumer( SignalBufferPtr signalBufferPtr )
    : mSignalBufferPtr{ std::move( signalBufferPtr ) }
{
}

bool
CANDataConsumer::findDecoderMethod( CANChannelNumericID channelId,
                                    uint32_t &messageId,
                                    const CANDecoderDictionary::CANMsgDecoderMethodType &decoderMethod,
                                    CANMessageDecoderMethod &currentMessageDecoderMethod )
{
    auto outerMapIt = decoderMethod.find( channelId );
    if ( outerMapIt != decoderMethod.cend() )
    {
        auto it = outerMapIt->second.find( messageId );

        if ( it != outerMapIt->second.cend() )
        {
            currentMessageDecoderMethod = it->second;
            return true;
        }
        it = outerMapIt->second.find( messageId & CAN_EFF_MASK );

        if ( it != outerMapIt->second.cend() )
        {
            messageId = messageId & CAN_EFF_MASK;
            currentMessageDecoderMethod = it->second;
            return true;
        }
    }
    return false;
}

void
CANDataConsumer::processMessage( CANChannelNumericID channelId,
                                 std::shared_ptr<const CANDecoderDictionary> &dictionary,
                                 uint32_t messageId,
                                 const uint8_t *data,
                                 size_t dataLength,
                                 Timestamp timestamp )
{
    // Skip if the dictionary was invalidated during message processing:
    if ( dictionary == nullptr )
    {
        return;
    }
    TraceSection traceSection =
        ( ( channelId < static_cast<CANChannelNumericID>( toUType( TraceSection::CAN_DECODER_CYCLE_19 ) -
                                                          toUType( TraceSection::CAN_DECODER_CYCLE_0 ) ) )
              ? static_cast<TraceSection>( channelId + toUType( TraceSection::CAN_DECODER_CYCLE_0 ) )
              : TraceSection::CAN_DECODER_CYCLE_19 );
    TraceModule::get().sectionBegin( traceSection );
    // get decoderMethod from the decoder dictionary
    const auto &decoderMethod = dictionary->canMessageDecoderMethod;
    // a set of signalID specifying which signal to collect
    const auto &signalIDsToCollect = dictionary->signalIDsToCollect;
    // check if this CAN message ID on this CAN Channel has the decoder method
    CANMessageDecoderMethod currentMessageDecoderMethod;
    // The value of messageId may be changed by the findDecoderMethod function. This is a
    // workaround as the cloud as of now does not send extended id messages.
    // If the decoder method for this message is not found in
    // decoderMethod dictionary, we check for the same id without the MSB set.
    // The message id which has a decoderMethod gets passed into messageId
    if ( findDecoderMethod( channelId, messageId, decoderMethod, currentMessageDecoderMethod ) )
    {
        // format to be used for decoding
        const auto &format = currentMessageDecoderMethod.format;
        const auto &collectType = currentMessageDecoderMethod.collectType;

        // Create Collected Data Frame
        CollectedDataFrame collectedDataFrame;
        // Check if we want to collect RAW CAN Frame; If so we also need to ensure Buffer is valid
        if ( ( mSignalBufferPtr.get() != nullptr ) && ( ( collectType == CANMessageCollectType::RAW ) ||
                                                        ( collectType == CANMessageCollectType::RAW_AND_DECODE ) ) )
        {
            // prepare the raw CAN Frame
            struct CollectedCanRawFrame canRawFrame;
            canRawFrame.frameID = messageId;
            canRawFrame.channelId = channelId;
            canRawFrame.receiveTime = timestamp;
            // CollectedCanRawFrame receive up to 64 CAN Raw Bytes
            canRawFrame.size = std::min( static_cast<uint8_t>( dataLength ), MAX_CAN_FRAME_BYTE_SIZE );
            std::copy( data, data + canRawFrame.size, canRawFrame.data.begin() );
            // collectedDataFrame will be pushed to the Buffer for next stage to consume
            collectedDataFrame.mCollectedCanRawFrame = std::make_shared<CollectedCanRawFrame>( canRawFrame );
        }
        // check if we want to decode can frame into signals and collect signals
        if ( ( mSignalBufferPtr.get() != nullptr ) && ( ( collectType == CANMessageCollectType::DECODE ) ||
                                                        ( collectType == CANMessageCollectType::RAW_AND_DECODE ) ) )
        {
            if ( format.isValid() )
            {
                std::vector<CANDecodedSignal> decodedSignals;
                if ( CANDecoder::decodeCANMessage( data, dataLength, format, signalIDsToCollect, decodedSignals ) )
                {    
                    FWE_LOG_INFO(" Decoding signals completed....")
                    // Create vector of Collected Signal Object
                    CollectedSignalsGroup collectedSignalsGroup;
                    for ( auto const &signal : decodedSignals )
                    {
                        // Create Collected Signal Object
                        struct CollectedSignal collectedSignal;
                        const auto signalType = signal.mSignalType;
                        switch ( signalType )
                        {
                        case SignalType::UINT64:
                            collectedSignal = CollectedSignal{ signal.mSignalID,
                                                               timestamp,
                                                               signal.mPhysicalValue.signalValue.uint64Val,
                                                               signal.mSignalType };
                            FWE_LOG_INFO("after decoding....Collected Signals UINT64 " + std::to_string(signal.mPhysicalValue.signalValue.uint64Val) );
                            break;
                        case SignalType::INT64:
                            collectedSignal = CollectedSignal{ signal.mSignalID,
                                                               timestamp,
                                                               signal.mPhysicalValue.signalValue.int64Val,
                                                               signal.mSignalType };
                            FWE_LOG_INFO(" after decoding.....Collected Signals INT64 " + std::to_string(signal.mPhysicalValue.signalValue.int64Val) );
                            break;
                        default:
                            collectedSignal = CollectedSignal{ signal.mSignalID,
                                                               timestamp,
                                                               signal.mPhysicalValue.signalValue.doubleVal,
                                                               signal.mSignalType };
                            FWE_LOG_INFO("after decoding....Collected Signals double " + std::to_string(signal.mPhysicalValue.signalValue.doubleVal) );
                            break;
                        }
                        // Only add valid signals to the vector
                        if ( collectedSignal.signalID != INVALID_SIGNAL_ID )
                        {
                            collectedSignalsGroup.push_back( collectedSignal );
                        }
                    }
                    FWE_LOG_INFO("Signals are collectd and stored...")
                    collectedDataFrame.mCollectedSignals = collectedSignalsGroup;
                }
                else
                {
                    // The decoding was not fully successful
                    FWE_LOG_WARN( "CAN Frame " + std::to_string( messageId ) + " decoding failed! " );
                }
            }
            else
            {
                // The CAN Message format is not valid, report as warning
                FWE_LOG_WARN( "CANMessageFormat Invalid for format message id: " + std::to_string( format.mMessageID ) +
                              " can message id: " + std::to_string( messageId ) +
                              " on CAN Channel Id: " + std::to_string( channelId ) );
            }
        }

        TraceModule::get().sectionEnd( traceSection );

        // Increase all queue metrics before pushing data to the buffer
        TraceModule::get().incrementAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_DATA_FRAMES );

        auto collectedSignals = collectedDataFrame.mCollectedSignals.size();
        TraceModule::get().addToAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_SIGNALS,
                                                collectedSignals );

        bool canRawFrameCollected = collectedDataFrame.mCollectedCanRawFrame != nullptr;
        if ( canRawFrameCollected )
        {
            TraceModule::get().incrementAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_CAN );
        }

        if ( !mSignalBufferPtr->push( std::move( collectedDataFrame ) ) )
        {
            TraceModule::get().decrementAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_DATA_FRAMES );

            if ( canRawFrameCollected )
            {
                TraceModule::get().decrementAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_CAN );
            }

            TraceModule::get().subtractFromAtomicVariable( TraceAtomicVariable::QUEUE_CONSUMER_TO_INSPECTION_SIGNALS,
                                                           collectedSignals );

            FWE_LOG_WARN( "Signal buffer full" );
        }
    }
}

} // namespace IoTFleetWise
} // namespace Aws
