/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <algorithm>
#include <functional>
#include <vector>

#ifdef HAVE_CONFIG_H
    #include "config.h"
#endif

#include "channel.h"
#include "common.h"
#include "completion_queue.h"
#include "channel_credentials.h"
#include "server.h"
#include "timeval.h"
#include "slice.h"

#include "grpc/grpc.h"
#include "grpc/grpc_security.h"

#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/vm/vm-regs.h"

namespace HPHP {

/*****************************************************************************/
/*                               Channel Data                                */
/*****************************************************************************/

Class* ChannelData::s_pClass{ nullptr };
const StaticString ChannelData::s_ClassName{ "Grpc\\Channel" };

Class* const ChannelData::getClass(void)
{
    if (!s_pClass)
    {
        s_pClass = Unit::lookupClass(s_ClassName.get());
        assert(s_pClass);
    }
    return s_pClass;
}

ChannelData::ChannelData(void) : m_pChannel{ nullptr }
{
}

ChannelData::ChannelData(grpc_channel* const channel) : m_pChannel{ channel }
{
}

ChannelData::~ChannelData(void)
{
    destroy();
}

void ChannelData::sweep(void)
{
    destroy();
}

void ChannelData::init(grpc_channel* channel, const bool owned, std::string&& hashKey)
{
    // destroy any existing channel data
    destroy();

    m_pChannel = channel;
    m_Owned = owned;
    m_HashKey = std::move(hashKey);
}

void ChannelData::destroy(void)
{
    if (m_pChannel)
    {
        if (m_Owned)
        {
            grpc_channel_destroy(m_pChannel);
        }
        m_pChannel = nullptr;
    }
}

/*****************************************************************************/
/*                            Channel Arguments                              */
/*****************************************************************************/

ChannelArgs::ChannelArgs(void)
{
    m_ChannelArgs.args = nullptr;
    m_ChannelArgs.num_args = 0;
}

ChannelArgs::~ChannelArgs(void)
{
    destroyArgs();
}

bool ChannelArgs::init(const Array& argsArray)
{
    // destroy existing
    destroyArgs();

    size_t elements{ static_cast<size_t>(argsArray.size()) };
    typedef std::pair<std::reference_wrapper<const std::string>,
                      std::reference_wrapper<const std::string>> ChannelArgsType;
    std::vector<ChannelArgsType> channelArgs;
    if (elements > 0)
    {
        m_ChannelArgs.args = req::calloc_raw_array<grpc_arg>(elements);
        m_ChannelArgs.num_args = elements;
        // reserve is needed so emplace_back below does not invalidate memory
        m_PHPData.reserve(elements);
        size_t count{ 0 };
        for (ArrayIter iter(argsArray); iter; ++iter, ++count)
        {
            Variant key{ iter.first() };
            if (key.isNull() || !key.isString())
            {
                destroyArgs();
                return false;
            }
            String keyStr{ key.toString() };
            std::string keyString{ keyStr.toCppString() };
            Variant value{ iter.second() };
            if (!value.isNull())
            {
                if (value.isInteger())
                {
                    // convert and store PHP data
                    int32_t valueInt{ value.toInt32() };
                    std::string valueString{ std::to_string(valueInt) };
                    m_PHPData.emplace_back(std::move(keyString), std::move(valueString));

                    m_ChannelArgs.args[count].value.integer = valueInt;
                    m_ChannelArgs.args[count].type = GRPC_ARG_INTEGER;
                }
                else if (value.isString())
                {
                    // convert and store PHP data
                    String valueStr{ value.toString() };
                    std::string valueString{ valueStr.toCppString() };
                    m_PHPData.emplace_back(std::move(keyString), std::move(valueString));
                    m_ChannelArgs.args[count].value.string = const_cast<char*>(m_PHPData[count].second.c_str());
                    m_ChannelArgs.args[count].type = GRPC_ARG_STRING;
                }
                else
                {
                    destroyArgs();
                    return false;
                }
                m_ChannelArgs.args[count].key = const_cast<char*>(m_PHPData[count].first.c_str());
                channelArgs.emplace_back(std::cref(m_PHPData[count].first),
                                         std::cref(m_PHPData[count].second));
            }
            else
            {
                destroyArgs();
                return false;
            }
        }
    }

    // sort the channel arguments via key then value
    auto sortLambda = [](const ChannelArgsType& pair1, const ChannelArgsType& pair2)
    {
        const std::string& p1String1{ pair1.first };
        const std::string& p1String2{ pair1.second };
        if (p1String1 != p1String2)
        {
            return (p1String1 < p1String2);
        }
        else
        {
            const std::string& p2String1{ pair2.first };
            const std::string& p2String2{ pair2.second };
            return (p2String1 < p2String2);
        }
    };
    std::sort(channelArgs.begin(), channelArgs.end(), sortLambda);

    for(const auto& argPair : channelArgs)
    {
        const std::string& string1{ argPair.first };
        const std::string& string2{ argPair.second };
        m_ConcatenatedArgs += string1 + string2;
    }

    m_HashKey = std::move(StringUtil::SHA1(m_ConcatenatedArgs, false).toCppString());

    return true;
}

void ChannelArgs::destroyArgs(void)
{
    // destroy channel args
    if (m_ChannelArgs.args)
    {
        req::destroy_raw_array(m_ChannelArgs.args, m_ChannelArgs.num_args);
        m_ChannelArgs.args = nullptr;
    }
    m_ChannelArgs.num_args = 0;

    // destroy PHP data
    m_PHPData.clear();

    // reset cached values
    m_HashKey.clear();
    m_ConcatenatedArgs.clear();
}

/*****************************************************************************/
/*                               Channel Cache                               */
/*****************************************************************************/

ChannelsCache::ChannelsCache(void) : m_ChannelMapMutex{}, m_ChannelMap{}
{
}

ChannelsCache::~ChannelsCache(void)
{
    WriteLock lock{ m_ChannelMapMutex };
    for(auto& channelPair : m_ChannelMap)
    {
        destroyChannel(channelPair.second);
    }
    m_ChannelMap.clear();
}

ChannelsCache& ChannelsCache::getChannelsCache(void)
{
    static ChannelsCache s_ChannelsCache;
    return s_ChannelsCache;
}

std::pair<bool, grpc_channel* const>
ChannelsCache::addChannel(const std::string& key, grpc_channel* const pChannel)
{
    {
        WriteLock lock{ m_ChannelMapMutex };
        auto insertPair = m_ChannelMap.emplace(key, pChannel);
        if (!insertPair.second) return std::make_pair(false, insertPair.first->second);
        else                    return std::make_pair(true, insertPair.first->second);
    }
}

grpc_channel* const ChannelsCache::getChannel(const std::string& channelHash)
{
    ReadLock lock{ m_ChannelMapMutex };

    auto itrFind = m_ChannelMap.find(channelHash);
    if (itrFind == m_ChannelMap.cend())
    {
        return nullptr;
    }
    else
    {
        return itrFind->second;
    }
}

bool ChannelsCache::hasChannel(const std::string& channelHash)
{
    return getChannel(channelHash) != nullptr;
}

void ChannelsCache::deleteChannel(const std::string& channelHash)
{
    {
        WriteLock lock(m_ChannelMapMutex);

        auto itrFind = m_ChannelMap.find(channelHash);
        if (itrFind != m_ChannelMap.cend())
        {
            destroyChannel(itrFind->second);
            m_ChannelMap.erase(itrFind);
        }
    }
}

size_t ChannelsCache::numChannels(void) const
{
    ReadLock lock{ m_ChannelMapMutex };
    return m_ChannelMap.size();
}

void ChannelsCache::destroyChannel(grpc_channel* const pChannel)
{
    grpc_channel_destroy(pChannel);
}

/*****************************************************************************/
/*                           HHVM Channel Methods                            */
/*****************************************************************************/

/**
 * Construct an instance of the Channel class.
 *
 * By default, the underlying grpc_channel is "persistent". That is, given
 * the same set of parameters passed to the constructor, the same underlying
 * grpc_channel will be returned.
 *
 * If the $args array contains a "credentials" key mapping to a
 * ChannelCredentials object, a secure channel will be created with those
 * credentials.
 *
 * If the $args array contains a "force_new" key mapping to a boolean value
 * of "true", a new underlying grpc_channel will be created regardless. If
 * there are any opened channels on the same hostname, user must manually
 * call close() on those dangling channels before the end of the PHP
 * script.
 *
 * @param string $target The hostname to associate with this channel
 * @param array $args_array The arguments to pass to the Channel
 */
void HHVM_METHOD(Channel, __construct,
                 const String& target,
                 const Array& args_array)
{
    VMRegGuard _;

    HHVM_TRACE_SCOPE("Channel Construct") // Debug Trace

    ChannelData* const pChannelData{ Native::data<ChannelData>(this_) };

    ChannelCredentialsData* pChannelCredentialsData{ nullptr };
    String credentialsKey{ "credentials" };
    Array argsArrayCopy{ args_array.copy() };
    if (argsArrayCopy.exists(credentialsKey, true))
    {
        Variant value{ argsArrayCopy[credentialsKey] };
        if(!value.isNull() && value.isObject())
        {
            ObjectData* objData{ value.getObjectData() };
            if (!objData->instanceof(String("Grpc\\ChannelCredentials")))
            {
                SystemLib::throwInvalidArgumentExceptionObject("credentials must be a Grpc\\ChannelCredentials object");
            }
            Object obj{ value.toObject() };
            pChannelCredentialsData = Native::data<ChannelCredentialsData>(obj);
        }

        argsArrayCopy.remove(credentialsKey, true);
    }

    bool force_new{ false };
    String forceNewKey{ "force_new" };
    if (argsArrayCopy.exists(forceNewKey, true))
    {
        Variant value{ argsArrayCopy[forceNewKey] };
        if (!value.isNull() && value.isBoolean())
        {
            force_new = value.toBoolean();
        }

        argsArrayCopy.remove(forceNewKey, true);
    }

    ChannelArgs channelArgs{};
    if (!channelArgs.init(argsArrayCopy))
    {
        SystemLib::throwInvalidArgumentExceptionObject("invalid channel arguments");
    }

    std::string strTarget{ target.toCppString() };
    std::string fullCacheKey{ StringUtil::SHA1(strTarget + channelArgs.concatenatedArgs(), false).toCppString() };
    if (pChannelCredentialsData != nullptr)
    {
        fullCacheKey += pChannelCredentialsData->hashKey();
    }
    grpc_channel* pChannel{ ChannelsCache::getChannelsCache().getChannel(fullCacheKey) };

    if (!force_new && pChannel)
    {
        pChannelData->init(pChannel, false, std::move(fullCacheKey));
    }
    else
    {
        if (force_new && pChannel)
        {
            // TODO: deleting an existing channel is problematic
            // the channel cache really needs to track all channels associated with
            // each hash along with a reference count on the channel and then if there are more
            // than 1 channels associated with the hash to delete the one with no references
            // during channel close
            // delete existing channel
            // ChannelsCache::getChannelsCache().deleteChannel(fullCacheKey);
        }

        if (!pChannelCredentialsData)
        {
            // no credentials create insecure channel
            pChannel = grpc_insecure_channel_create(strTarget.c_str(), &channelArgs.args(), nullptr);
        }
        else
        {
            // create secure chhanel
            pChannel = grpc_secure_channel_create(pChannelCredentialsData->credentials(),
                                                  strTarget.c_str(), &channelArgs.args(), nullptr);
        }

        if (!pChannel)
        {
            SystemLib::throwBadMethodCallExceptionObject("failed to create channel");
        }

        std::pair<bool, grpc_channel* const>
            addResult{ ChannelsCache::getChannelsCache().addChannel(fullCacheKey, pChannel) };
        if (!addResult.first)
        {
            // channel already cached.  This can happen if we currently force new and now
            // have two channels with same hash or via race conditions with multiple create channels
            // at same time
            //SystemLib::throwBadMethodCallExceptionObject("failed to create channel");

            // delete new channel and use existing
            grpc_channel_destroy(pChannel);
            pChannel = addResult.second;
        }
        pChannelData->init(pChannel, false, std::move(fullCacheKey));
    }
    //std::cout << ChannelsCache::getChannelsCache().numChannels() << std::endl;
}

/**
 * Get the endpoint this call/stream is connected to
 * @return string The URI of the endpoint
 */
String HHVM_METHOD(Channel, getTarget)
{
    VMRegGuard _;

    HHVM_TRACE_SCOPE("Channel getTarget") // Debug Trace

    ChannelData* const pChannelData{ Native::data<ChannelData>(this_) };

    if (!pChannelData->channel())
    {
        SystemLib::throwBadMethodCallExceptionObject("Channel already closed.");
    }

    return String{ grpc_channel_get_target(pChannelData->channel()), CopyString };
}

/**
 * Get the connectivity state of the channel
 * @param bool $try_to_connect Try to connect on the channel (optional)
 * @return long The grpc connectivity state
 */
int64_t HHVM_METHOD(Channel, getConnectivityState,
                    bool try_to_connect /* = false */)
{
    VMRegGuard _;

    ChannelData* const pChannelData{ Native::data<ChannelData>(this_) };

    if (!pChannelData->channel())
    {
        SystemLib::throwBadMethodCallExceptionObject("Channel already closed.");
    }

    grpc_connectivity_state state{ grpc_channel_check_connectivity_state(pChannelData->channel(),
                                                                         try_to_connect ? 1 : 0) };

    return static_cast<int64_t>(state);
}

/**
 * Watch the connectivity state of the channel until it changed
 * @param long $last_state The previous connectivity state of the channel
 * @param Timeval $deadline_obj The deadline this function should wait until
 * @return bool If the connectivity state changes from last_state
 *              before deadline
 */
bool HHVM_METHOD(Channel, watchConnectivityState,
                 int64_t last_state,
                 const Object& deadline)
{
    VMRegGuard _;

    HHVM_TRACE_SCOPE("Channel watchConnectivityState") // Debug Trace

    ChannelData* const pChannelData{ Native::data<ChannelData>(this_) };

    if (!pChannelData->channel())
    {
        SystemLib::throwBadMethodCallExceptionObject("Channel already closed.");
    }

    TimevalData* const pTimevalDataDeadline{ Native::data<TimevalData>(deadline) };

    // TODO: In order to perform this we need to get the queue associated with the channel
    // which is associated with the call since each call gets own queue

   /* grpc_channel_watch_connectivity_state(pChannelData->channel(),
                                          static_cast<grpc_connectivity_state>(last_state),
                                          pTimevalDataDeadline->time(),
                                          CompletionQueue::getClientQueue().queue(),
                                          nullptr);

    grpc_event event( grpc_completion_queue_pluck(CompletionQueue::getClientQueue().queue(),
                                                  nullptr,
                                                  gpr_inf_future(GPR_CLOCK_REALTIME), nullptr) );

    return (event.success != 0);*/
    return true;
}

/**
 * Close the channel
 * @return void
 */
void HHVM_METHOD(Channel, close)
{
    VMRegGuard _;

    HHVM_TRACE_SCOPE("Channel close") // Debug Trace

    ChannelData* const pChannelData{ Native::data<ChannelData>(this_) };

    if (!pChannelData->channel())
    {
        SystemLib::throwBadMethodCallExceptionObject("Channel already closed.");
    }
    pChannelData->init(nullptr, false, std::string{}); // mark channel closed
}

} // namespace HPHP