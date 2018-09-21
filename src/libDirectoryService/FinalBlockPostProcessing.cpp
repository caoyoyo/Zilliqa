/**
* Copyright (c) 2018 Zilliqa 
* This source code is being disclosed to you solely for the purpose of your participation in 
* testing Zilliqa. You may view, compile and run the code for that purpose and pursuant to 
* the protocols and algorithms that are programmed into, and intended by, the code. You may 
* not do anything else with the code without express permission from Zilliqa Research Pte. Ltd., 
* including modifying or publishing the code (or any part of it), and developing or forming 
* another public or private blockchain network. This source code is provided ‘as is’ and no 
* warranties are given as to title or non-infringement, merchantability or fitness for purpose 
* and, to the extent permitted by law, all liability for your use of the code is disclaimed. 
* Some programs in this code are governed by the GNU General Public License v3.0 (available at 
* https://www.gnu.org/licenses/gpl-3.0.en.html) (‘GPLv3’). The programs that are governed by 
* GPLv3.0 are those programs that are located in the folders src/depends and tests/depends 
* and which include a reference to GPLv3 in their program files.
**/

#include <algorithm>
#include <chrono>
#include <thread>

#include "DirectoryService.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "depends/common/RLP.h"
#include "depends/libDatabase/MemoryDB.h"
#include "depends/libTrie/TrieDB.h"
#include "depends/libTrie/TrieHash.h"
#include "libCrypto/Sha2.h"
#include "libMediator/Mediator.h"
#include "libMessage/Messenger.h"
#include "libNetwork/P2PComm.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/SanityChecks.h"

using namespace std;
using namespace boost::multiprecision;

void DirectoryService::StoreFinalBlockToDisk()
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::StoreFinalBlockToDisk not expected to "
                    "be called from LookUp node.");
        return;
    }

    LOG_MARKER();

    // Add finalblock to txblockchain
    m_mediator.m_node->AddBlock(*m_finalBlock);
    m_mediator.m_currentEpochNum
        = m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum()
        + 1;

    // At this point, the transactions in the last Epoch is no longer useful, thus erase.
    // m_mediator.m_node->EraseCommittedTransactions(m_mediator.m_currentEpochNum
    //                                               - 2);

    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Storing Tx Block Number: "
                  << m_finalBlock->GetHeader().GetBlockNum() << " with Type: "
                  << to_string(m_finalBlock->GetHeader().GetType())
                  << ", Version: " << m_finalBlock->GetHeader().GetVersion()
                  << ", Timestamp: " << m_finalBlock->GetHeader().GetTimestamp()
                  << ", NumTxs: " << m_finalBlock->GetHeader().GetNumTxs());

    vector<unsigned char> serializedTxBlock;
    m_finalBlock->Serialize(serializedTxBlock, 0);
    BlockStorage::GetBlockStorage().PutTxBlock(
        m_finalBlock->GetHeader().GetBlockNum(), serializedTxBlock);
}

bool DirectoryService::SendFinalBlockToLookupNodes()
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::SendFinalBlockToLookupNodes not "
                    "expected to be called from LookUp node.");
        return true;
    }

    vector<unsigned char> finalblock_message
        = {MessageType::NODE, NodeInstructionType::FINALBLOCK};

    const uint64_t dsBlockNumber
        = m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum();

    vector<unsigned char> stateDelta;
    AccountStore::GetInstance().GetSerializedDelta(stateDelta);

    if (!Messenger::SetNodeFinalBlock(finalblock_message, MessageOffset::BODY,
                                      0, dsBlockNumber, m_consensusID,
                                      *m_finalBlock, stateDelta))
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Messenger::SetNodeFinalBlock failed.");
        return false;
    }

    m_mediator.m_lookup->SendMessageToLookupNodes(finalblock_message);

    return true;
}

void DirectoryService::DetermineShardsToSendFinalBlockTo(
    unsigned int& my_DS_cluster_num, unsigned int& my_shards_lo,
    unsigned int& my_shards_hi) const
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::DetermineShardsToSendFinalBlockTo not "
                    "expected to be called from LookUp node.");
        return;
    }

    // Multicast final block to my assigned shard's nodes - send FINALBLOCK message
    // Message = [Final block]

    // Multicast assignments:
    // 1. Divide DS committee into clusters of size 20
    // 2. Each cluster talks to all shard members in each shard
    //    DS cluster 0 => Shard 0
    //    DS cluster 1 => Shard 1
    //    ...
    //    DS cluster 0 => Shard (num of DS clusters)
    //    DS cluster 1 => Shard (num of DS clusters + 1)
    LOG_MARKER();

    unsigned int num_DS_clusters
        = m_mediator.m_DSCommittee->size() / DS_MULTICAST_CLUSTER_SIZE;
    if ((m_mediator.m_DSCommittee->size() % DS_MULTICAST_CLUSTER_SIZE) > 0)
    {
        num_DS_clusters++;
    }
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "DEBUG num of ds clusters " << num_DS_clusters)
    unsigned int shard_groups_count = m_shards.size() / num_DS_clusters;
    if ((m_shards.size() % num_DS_clusters) > 0)
    {
        shard_groups_count++;
    }
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "DEBUG num of shard group count " << shard_groups_count)

    my_DS_cluster_num = m_consensusMyID / DS_MULTICAST_CLUSTER_SIZE;
    my_shards_lo = my_DS_cluster_num * shard_groups_count;
    my_shards_hi = my_shards_lo + shard_groups_count - 1;

    if (my_shards_hi >= m_shards.size())
    {
        my_shards_hi = m_shards.size() - 1;
    }
}

void DirectoryService::SendFinalBlockToShardNodes(
    unsigned int my_DS_cluster_num, unsigned int my_shards_lo,
    unsigned int my_shards_hi)
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::SendFinalBlockToShardNodes not expected "
                    "to be called from LookUp node.");
        return;
    }

    // Too few target shards - avoid asking all DS clusters to send
    LOG_MARKER();

    if ((my_DS_cluster_num + 1) > m_shards.size())
    {
        return;
    }

    const uint64_t dsBlockNumber
        = m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum();

    vector<unsigned char> stateDelta;
    AccountStore::GetInstance().GetSerializedDelta(stateDelta);

    auto p = m_shards.begin();
    advance(p, my_shards_lo);

    for (unsigned int shardID = my_shards_lo; shardID <= my_shards_hi;
         shardID++)
    {
        vector<Peer> shard_peers;

        for (auto& kv : *p)
        {
            shard_peers.emplace_back(kv.second);
            LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                      " PubKey: "
                          << DataConversion::SerializableToHexStr(kv.first)
                          << " IP: " << kv.second.GetPrintableIPAddress()
                          << " Port: " << kv.second.m_listenPortHost);
        }

        vector<unsigned char> finalblock_message
            = {MessageType::NODE, NodeInstructionType::FINALBLOCK};

        if (!Messenger::SetNodeFinalBlock(
                finalblock_message, MessageOffset::BODY, shardID, dsBlockNumber,
                m_consensusID, *m_finalBlock, stateDelta))
        {
            LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "Messenger::SetNodeFinalBlock failed.");
            return;
        }

        SHA2<HASH_TYPE::HASH_VARIANT_256> sha256;
        sha256.Update(finalblock_message);
        vector<unsigned char> this_msg_hash = sha256.Finalize();
        LOG_STATE(
            "[INFOR]["
            << setw(15) << left << m_mediator.m_selfPeer.GetPrintableIPAddress()
            << "]["
            << DataConversion::Uint8VecToHexStr(this_msg_hash).substr(0, 6)
            << "]["
            << DataConversion::charArrToHexStr(m_mediator.m_dsBlockRand)
                   .substr(0, 6)
            << "]["
            << m_mediator.m_txBlockChain.GetLastBlock()
                    .GetHeader()
                    .GetBlockNum()
                + 1
            << "] FBBLKGEN");

        P2PComm::GetInstance().SendBroadcastMessage(shard_peers,
                                                    finalblock_message);

        p++;
    }
}

// void DirectoryService::StoreMicroBlocksToDisk()
// {
//     LOG_MARKER();
//     for(auto microBlock : m_microBlocks)
//     {

//         LOG_GENERAL(INFO,  "Storing Micro Block Hash: " << microBlock.GetHeader().GetTxRootHash() <<
//             " with Type: " << microBlock.GetHeader().GetType() <<
//             ", Version: " << microBlock.GetHeader().GetVersion() <<
//             ", Timestamp: " << microBlock.GetHeader().GetTimestamp() <<
//             ", NumTxs: " << microBlock.GetHeader().GetNumTxs());

//         vector<unsigned char> serializedMicroBlock;
//         microBlock.Serialize(serializedMicroBlock, 0);
//         BlockStorage::GetBlockStorage().PutMicroBlock(microBlock.GetHeader().GetTxRootHash(),
//                                                serializedMicroBlock);
//     }
//     m_microBlocks.clear();
// }

void DirectoryService::ProcessFinalBlockConsensusWhenDone()
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::ProcessFinalBlockConsensusWhenDone not "
                    "expected to be called from LookUp node.");
        return;
    }

    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Final block consensus is DONE!!!");

    // Clear microblock(s)
    // m_microBlocks.clear();

    if (m_mode == PRIMARY_DS)
    {
        LOG_STATE("[FBCON][" << setw(15) << left
                             << m_mediator.m_selfPeer.GetPrintableIPAddress()
                             << "]["
                             << m_mediator.m_txBlockChain.GetLastBlock()
                                    .GetHeader()
                                    .GetBlockNum()
                      + 1 << "] DONE");
    }

    // Update the final block with the co-signatures from the consensus
    m_finalBlock->SetCoSignatures(*m_consensusObject);

    //Coinbase
    SaveCoinbase(m_finalBlock->GetB1(), m_finalBlock->GetB2(), -1);

    // StoreMicroBlocksToDisk();
    StoreFinalBlockToDisk();

    AccountStore::GetInstance().CommitTemp();

    bool isVacuousEpoch
        = (m_consensusID >= (NUM_FINAL_BLOCK_PER_POW - NUM_VACUOUS_EPOCHS));
    if (isVacuousEpoch)
    {
        AccountStore::GetInstance().MoveUpdatesToDisk();
        BlockStorage::GetBlockStorage().PutMetadata(MetaType::DSINCOMPLETED,
                                                    {'0'});
        if (!LOOKUP_NODE_MODE)
        {
            BlockStorage::GetBlockStorage().PopFrontTxBodyDB();
        }
    }

    m_mediator.UpdateDSBlockRand();
    m_mediator.UpdateTxBlockRand();

    if (m_toSendTxnToLookup && !isVacuousEpoch)
    {
        m_mediator.m_node->CallActOnFinalblock();
    }

    // TODO: Refine this
    unsigned int nodeToSendToLookUpLo = COMM_SIZE / 4;
    unsigned int nodeToSendToLookUpHi
        = nodeToSendToLookUpLo + TX_SHARING_CLUSTER_SIZE;

    if (m_consensusMyID > nodeToSendToLookUpLo
        && m_consensusMyID < nodeToSendToLookUpHi)
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Part of the DS committeement (assigned) that will send the "
                  "Final Block to "
                  "the lookup nodes");
        SendFinalBlockToLookupNodes();
    }

    // uint8_t tx_sharing_mode
    //     = (m_sharingAssignment.size() > 0) ? DS_FORWARD_ONLY : ::IDLE;
    // m_mediator.m_node->ActOnFinalBlock(tx_sharing_mode, m_sharingAssignment);

    unsigned int my_DS_cluster_num;
    unsigned int my_shards_lo;
    unsigned int my_shards_hi;

    LOG_STATE(
        "[FLBLK]["
        << setw(15) << left << m_mediator.m_selfPeer.GetPrintableIPAddress()
        << "]["
        << m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum()
            + 1
        << "] BEFORE SENDING FINAL BLOCK");

    DetermineShardsToSendFinalBlockTo(my_DS_cluster_num, my_shards_lo,
                                      my_shards_hi);
    SendFinalBlockToShardNodes(my_DS_cluster_num, my_shards_lo, my_shards_hi);

    LOG_STATE(
        "[FLBLK]["
        << setw(15) << left << m_mediator.m_selfPeer.GetPrintableIPAddress()
        << "]["
        << m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum()
            + 1
        << "] AFTER SENDING FINAL BLOCK");

    AccountStore::GetInstance().InitTemp();
    m_stateDeltaFromShards.clear();
    m_allPoWConns.clear();
    ClearDSPoWSolns();
    ResetPoWSubmissionCounter();

    auto func = [this]() mutable -> void {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "START OF a new EPOCH");
        if (m_mediator.m_currentEpochNum % NUM_FINAL_BLOCK_PER_POW == 0)
        {
            LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "[PoW needed]");

            CleanFinalblockConsensusBuffer();

            m_mediator.m_node->CleanCreatedTransaction();

            m_mediator.m_node->CleanMicroblockConsensusBuffer();

            SetState(POW_SUBMISSION);
            cv_POWSubmission.notify_all();

            POW::GetInstance().EthashConfigureLightClient(
                m_mediator.m_dsBlockChain.GetLastBlock()
                    .GetHeader()
                    .GetBlockNum()
                + 1);
            m_consensusID = 0;
            m_mediator.m_node->m_consensusID = 0;
            m_mediator.m_node->m_consensusLeaderID = 0;
            if (m_mode == PRIMARY_DS)
            {
                LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                          "Waiting "
                              << NEW_NODE_SYNC_INTERVAL + POW_WINDOW_IN_SECONDS
                              << " seconds, accepting PoW submissions...");

                // Notify lookup that it's time to do PoW
                vector<unsigned char> startpow_message = {
                    MessageType::LOOKUP, LookupInstructionType::RAISESTARTPOW};
                m_mediator.m_lookup->SendMessageToLookupNodesSerial(
                    startpow_message);

                // New nodes poll DSInfo from the lookups every NEW_NODE_SYNC_INTERVAL
                // So let's add that to our wait time to allow new nodes to get SETSTARTPOW and submit a PoW
                this_thread::sleep_for(chrono::seconds(
                    NEW_NODE_SYNC_INTERVAL + POW_WINDOW_IN_SECONDS));

                RunConsensusOnDSBlock();
            }
            else
            {
                std::unique_lock<std::mutex> cv_lk(m_MutexCVDSBlockConsensus);

                // New nodes poll DSInfo from the lookups every NEW_NODE_SYNC_INTERVAL
                // So let's add that to our wait time to allow new nodes to get SETSTARTPOW and submit a PoW
                if (cv_DSBlockConsensus.wait_for(
                        cv_lk,
                        std::chrono::seconds(NEW_NODE_SYNC_INTERVAL
                                             + POW_BACKUP_WINDOW_IN_SECONDS))
                    == std::cv_status::timeout)
                {
                    LOG_GENERAL(INFO,
                                "Woken up from the sleep of "
                                    << NEW_NODE_SYNC_INTERVAL
                                        + POW_BACKUP_WINDOW_IN_SECONDS
                                    << " seconds");
                }
                else
                {
                    LOG_GENERAL(INFO,
                                "Received announcement message. Time to "
                                "run consensus.");
                }

                RunConsensusOnDSBlock();
            }
        }
        else
        {
            m_consensusID++;
            m_mediator.m_node->UpdateStateForNextConsensusRound();
            SetState(MICROBLOCK_SUBMISSION);
            m_dsStartedMicroblockConsensus = false;
            LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "[No PoW needed] Waiting for Microblock.");

            auto func1 = [this]() mutable -> void {
                m_mediator.m_node->CommitTxnPacketBuffer();
            };
            DetachedFunction(1, func1);

            CommitMBSubmissionMsgBuffer();

            std::unique_lock<std::mutex> cv_lk(
                m_MutexScheduleDSMicroBlockConsensus);
            if (cv_scheduleDSMicroBlockConsensus.wait_for(
                    cv_lk, std::chrono::seconds(MICROBLOCK_TIMEOUT))
                == std::cv_status::timeout)
            {
                LOG_GENERAL(WARNING,
                            "Timeout: Didn't receive all Microblock. Proceeds "
                            "without it");

                auto func2 = [this]() mutable -> void {
                    if (!m_dsStartedMicroblockConsensus)
                    {
                        m_dsStartedMicroblockConsensus = true;
                        m_mediator.m_node->RunConsensusOnMicroBlock();
                    }
                };

                DetachedFunction(1, func2);

                std::unique_lock<std::mutex> cv_lk(
                    m_MutexScheduleFinalBlockConsensus);
                if (cv_scheduleFinalBlockConsensus.wait_for(
                        cv_lk,
                        std::chrono::seconds(
                            DS_MICROBLOCK_CONSENSUS_OBJECT_TIMEOUT))
                    == std::cv_status::timeout)
                {
                    LOG_GENERAL(
                        WARNING,
                        "Timeout: Didn't finish DS Microblock. Proceeds "
                        "without it");

                    RunConsensusOnFinalBlock(true);
                }
            }
        }
    };

    DetachedFunction(1, func);
}

bool DirectoryService::ProcessFinalBlockConsensus(
    const vector<unsigned char>& message, unsigned int offset, const Peer& from)
{
    LOG_MARKER();

    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::ProcessFinalBlockConsensus not expected "
                    "to be called from LookUp node.");
        return true;
    }

    uint32_t consensus_id = 0;

    if (!m_consensusObject->GetConsensusID(message, offset, consensus_id))
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "GetConsensusID failed.");
        return false;
    }

    if (m_state != FINALBLOCK_CONSENSUS)
    {
        {
            lock_guard<mutex> h(m_mutexFinalBlockConsensusBuffer);
            m_finalBlockConsensusBuffer[consensus_id].push_back(
                make_pair(from, message));
        }

        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Process final block arrived early, saved to buffer");

        if (consensus_id == m_consensusID)
        {
            lock_guard<mutex> g(m_mutexPrepareRunFinalblockConsensus);
            cv_scheduleDSMicroBlockConsensus.notify_all();
            if (!m_dsStartedMicroblockConsensus)
            {
                m_dsStartedMicroblockConsensus = true;
            }
            cv_scheduleFinalBlockConsensus.notify_all();
            RunConsensusOnFinalBlock(true);
        }
    }
    else
    {
        if (consensus_id < m_consensusID)
        {
            LOG_GENERAL(WARNING,
                        "Consensus ID in message ("
                            << consensus_id << ") is smaller than current ("
                            << m_consensusID << ")");
            return false;
        }
        else if (consensus_id > m_consensusID)
        {
            LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "Buffer final block with larger consensus ID ("
                          << consensus_id << "), current (" << m_consensusID
                          << ")");

            {
                lock_guard<mutex> h(m_mutexFinalBlockConsensusBuffer);
                m_finalBlockConsensusBuffer[consensus_id].push_back(
                    make_pair(from, message));
            }
        }
        else
        {
            return ProcessFinalBlockConsensusCore(message, offset, from);
        }
    }

    return true;
}

void DirectoryService::CommitFinalBlockConsensusBuffer()
{
    lock_guard<mutex> g(m_mutexFinalBlockConsensusBuffer);

    for (const auto& i : m_finalBlockConsensusBuffer[m_consensusID])
    {
        auto runconsensus = [this, i]() {
            ProcessFinalBlockConsensusCore(i.second, MessageOffset::BODY,
                                           i.first);
        };
        DetachedFunction(1, runconsensus);
    }
}

void DirectoryService::CleanFinalblockConsensusBuffer()
{
    lock_guard<mutex> g(m_mutexFinalBlockConsensusBuffer);
    m_finalBlockConsensusBuffer.clear();
}

bool DirectoryService::ProcessFinalBlockConsensusCore(
    [[gnu::unused]] const vector<unsigned char>& message,
    [[gnu::unused]] unsigned int offset, [[gnu::unused]] const Peer& from)
{
    LOG_MARKER();

    if (!CheckState(PROCESS_FINALBLOCKCONSENSUS))
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Ignoring consensus message. I am at state " << m_state);
        return false;
    }

    // Consensus messages must be processed in correct sequence as they come in
    // It is possible for ANNOUNCE to arrive before correct DS state
    // In that case, state transition will occurs and ANNOUNCE will be processed.
    std::unique_lock<mutex> cv_lk(m_mutexProcessConsensusMessage);
    if (cv_processConsensusMessage.wait_for(
            cv_lk, std::chrono::seconds(CONSENSUS_MSG_ORDER_BLOCK_WINDOW),
            [this, message, offset]() -> bool {
                lock_guard<mutex> g(m_mutexConsensus);
                if (m_mediator.m_lookup->m_syncType != SyncType::NO_SYNC)
                {
                    LOG_GENERAL(WARNING,
                                "The node started the process of rejoining, "
                                "Ignore rest of "
                                "consensus msg.")
                    return false;
                }

                if (m_consensusObject == nullptr)
                {
                    LOG_GENERAL(WARNING,
                                "m_consensusObject is a nullptr. It has not "
                                "been initialized.")
                    return false;
                }
                return m_consensusObject->CanProcessMessage(message, offset);
            }))
    {
        // Correct order preserved
    }
    else
    {
        LOG_GENERAL(
            WARNING,
            "Timeout while waiting for correct order of Final Block consensus "
            "messages");
        return false;
    }

    lock_guard<mutex> g(m_mutexConsensus);

    if (!m_consensusObject->ProcessMessage(message, offset, from))
    {
        return false;
    }

    ConsensusCommon::State state = m_consensusObject->GetState();

    if (state == ConsensusCommon::State::DONE)
    {
        cv_viewChangeFinalBlock.notify_all();
        m_viewChangeCounter = 0;
        ProcessFinalBlockConsensusWhenDone();
    }
    else if (state == ConsensusCommon::State::ERROR)
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Oops, no consensus reached - what to do now???");

        if (m_consensusObject->GetConsensusErrorCode()
            == ConsensusCommon::FINALBLOCK_MISSING_MICROBLOCKS)
        {
            // Missing microblocks proposed by leader. Will attempt to fetch
            // missing microblocks from leader, set to a valid state to accept cosig1 and cosig2
            LOG_EPOCH(
                WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                "Oops, no consensus reached - consensus error. "
                "error number: "
                    << to_string(m_consensusObject->GetConsensusErrorCode())
                    << " error message: "
                    << (m_consensusObject->GetConsensusErrorMsg()));

            // Block till txn is fetched
            unique_lock<mutex> lock(m_mutexCVMissingMicroBlock);
            if (cv_MissingMicroBlock.wait_for(
                    lock, chrono::seconds(FETCHING_MISSING_TXNS_TIMEOUT))
                == std::cv_status::timeout)
            {
                LOG_EPOCH(WARNING,
                          to_string(m_mediator.m_currentEpochNum).c_str(),
                          "fetching missing microblocks timeout");
            }
            else
            {
                // Re-run consensus
                m_consensusObject->RecoveryAndProcessFromANewState(
                    ConsensusCommon::INITIAL);

                auto rerunconsensus = [this, message, offset, from]() {
                    ProcessFinalBlockConsensusCore(message, offset, from);
                };
                DetachedFunction(1, rerunconsensus);
                return true;
            }
        }

        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "No consensus reached. Wait for view change. ");
        return false;
    }
    else
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Consensus state = " << m_consensusObject->GetStateString());
        cv_processConsensusMessage.notify_all();
    }
    return true;
}
