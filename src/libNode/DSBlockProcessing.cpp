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

#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include <chrono>
#include <functional>
#include <thread>

#include "Node.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "depends/common/RLP.h"
#include "depends/libDatabase/MemoryDB.h"
#include "depends/libTrie/TrieDB.h"
#include "depends/libTrie/TrieHash.h"
#include "libConsensus/ConsensusUser.h"
#include "libCrypto/Sha2.h"
#include "libData/AccountData/Account.h"
#include "libData/AccountData/AccountStore.h"
#include "libData/AccountData/Transaction.h"
#include "libMediator/Mediator.h"
#include "libMessage/Messenger.h"
#include "libNetwork/Whitelist.h"
#include "libPOW/pow.h"
#include "libUtils/BitVector.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/HashUtils.h"
#include "libUtils/Logger.h"
#include "libUtils/SanityChecks.h"
#include "libUtils/TimeLockedFunction.h"
#include "libUtils/TimeUtils.h"

using namespace std;
using namespace boost::multiprecision;

void Node::StoreDSBlockToDisk(const DSBlock& dsblock)
{
    LOG_MARKER();

    m_mediator.m_dsBlockChain.AddBlock(dsblock);
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Storing DS Block Number: "
                  << dsblock.GetHeader().GetBlockNum() << " with Nonce: "
                  << dsblock.GetHeader().GetNonce() << ", DS PoW Difficulty: "
                  << to_string(dsblock.GetHeader().GetDSDifficulty())
                  << ", Difficulty: "
                  << to_string(dsblock.GetHeader().GetDifficulty())
                  << ", Timestamp: " << dsblock.GetHeader().GetTimestamp());

    // Update the rand1 value for next PoW
    m_mediator.UpdateDSBlockRand();

    // Store DS Block to disk
    vector<unsigned char> serializedDSBlock;
    dsblock.Serialize(serializedDSBlock, 0);

    BlockStorage::GetBlockStorage().PutDSBlock(
        dsblock.GetHeader().GetBlockNum(), serializedDSBlock);
    m_mediator.m_ds->m_latestActiveDSBlockNum
        = dsblock.GetHeader().GetBlockNum();
    BlockStorage::GetBlockStorage().PutMetadata(
        LATESTACTIVEDSBLOCKNUM,
        DataConversion::StringToCharArray(
            to_string(m_mediator.m_ds->m_latestActiveDSBlockNum)));
    if (!LOOKUP_NODE_MODE)
    {
        BlockStorage::GetBlockStorage().PushBackTxBodyDB(
            dsblock.GetHeader().GetBlockNum());
    }
}

void Node::UpdateDSCommiteeComposition(const Peer& winnerpeer)
{
    LOG_MARKER();

    // Update my view of the DS committee
    // 1. Insert new leader at the head of the queue
    // 2. Pop out the oldest backup from the tail of the queue
    // Note: If I am the primary, push a placeholder with ip=0 and port=0 in place of my real port
    Peer peer;

    if (!(m_mediator.m_selfKey.second
          == m_mediator.m_dsBlockChain.GetLastBlock()
                 .GetHeader()
                 .GetMinerPubKey()))
    {
        peer = winnerpeer;
    }

    m_mediator.m_DSCommittee->emplace_front(make_pair(
        m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetMinerPubKey(),
        peer));
    m_mediator.m_DSCommittee->pop_back();
}

bool Node::CheckWhetherDSBlockNumIsLatest(const uint64_t dsblockNum)
{
    LOG_MARKER();

    uint64_t latestBlockNumInBlockchain
        = m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum();

    if (dsblockNum < latestBlockNumInBlockchain + 1)
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "We are processing duplicated blocks\n"
                      << "cur block num: " << latestBlockNumInBlockchain << "\n"
                      << "incoming block num: " << dsblockNum);
        return false;
    }
    else if (dsblockNum > latestBlockNumInBlockchain + 1)
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Missing of some DS blocks. Requested: "
                      << dsblockNum
                      << " while Present: " << latestBlockNumInBlockchain);
        // Todo: handle missing DS blocks.
        return false;
    }

    return true;
}

bool Node::VerifyDSBlockCoSignature(const DSBlock& dsblock)
{
    LOG_MARKER();

    unsigned int index = 0;
    unsigned int count = 0;

    const vector<bool>& B2 = dsblock.GetB2();
    if (m_mediator.m_DSCommittee->size() != B2.size())
    {
        LOG_GENERAL(WARNING,
                    "Mismatch: DS committee size = "
                        << m_mediator.m_DSCommittee->size()
                        << ", co-sig bitmap size = " << B2.size());
        return false;
    }

    // Generate the aggregated key
    vector<PubKey> keys;
    for (auto const& kv : *m_mediator.m_DSCommittee)
    {
        if (B2.at(index))
        {
            keys.emplace_back(kv.first);
            count++;
        }
        index++;
    }

    if (count != ConsensusCommon::NumForConsensus(B2.size()))
    {
        LOG_GENERAL(WARNING, "Cosig was not generated by enough nodes");
        return false;
    }

    shared_ptr<PubKey> aggregatedKey = MultiSig::AggregatePubKeys(keys);
    if (aggregatedKey == nullptr)
    {
        LOG_GENERAL(WARNING, "Aggregated key generation failed");
        return false;
    }

    // Verify the collective signature
    vector<unsigned char> message;
    dsblock.GetHeader().Serialize(message, 0);
    dsblock.GetCS1().Serialize(message, DSBlockHeader::SIZE);
    BitVector::SetBitVector(message, DSBlockHeader::SIZE + BLOCK_SIG_SIZE,
                            dsblock.GetB1());
    if (!Schnorr::GetInstance().Verify(message, 0, message.size(),
                                       dsblock.GetCS2(), *aggregatedKey))
    {
        LOG_GENERAL(WARNING, "Cosig verification failed");
        for (auto& kv : keys)
        {
            LOG_GENERAL(WARNING, kv);
        }
        return false;
    }

    return true;
}

void Node::LogReceivedDSBlockDetails([[gnu::unused]] const DSBlock& dsblock)
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "I the lookup node have deserialized the DS Block");
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "dsblock.GetHeader().GetDifficulty(): "
                      << (int)dsblock.GetHeader().GetDifficulty());
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "dsblock.GetHeader().GetNonce(): "
                      << dsblock.GetHeader().GetNonce());
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "dsblock.GetHeader().GetBlockNum(): "
                      << dsblock.GetHeader().GetBlockNum());
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "dsblock.GetHeader().GetMinerPubKey(): "
                      << dsblock.GetHeader().GetMinerPubKey());
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "dsblock.GetHeader().GetLeaderPubKey(): "
                      << dsblock.GetHeader().GetLeaderPubKey());
    }
}

bool Node::LoadShardingStructure()
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "Node::LoadShardingStructure not expected to be called "
                    "from LookUp node.");
        return true;
    }

    m_numShards = m_mediator.m_ds->m_shards.size();

    // Check the shard ID against the deserialized structure
    if (m_myShardID >= m_mediator.m_ds->m_shards.size())
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Shard ID " << m_myShardID << " >= num shards "
                              << m_mediator.m_ds->m_shards.size());
        return false;
    }

    const auto& my_shard = m_mediator.m_ds->m_shards.at(m_myShardID);

    // m_myShardMembers->clear();
    m_myShardMembers.reset(new std::deque<pair<PubKey, Peer>>);

    // All nodes; first entry is leader
    unsigned int index = 0;
    for (const auto& shardNode : my_shard)
    {
        m_myShardMembers->emplace_back(std::get<SHARD_NODE_PUBKEY>(shardNode),
                                       std::get<SHARD_NODE_PEER>(shardNode));

        // Zero out my IP to avoid sending to myself
        if (m_mediator.m_selfPeer == m_myShardMembers->back().second)
        {
            m_consensusMyID = index; // Set my ID
            m_myShardMembers->back().second.m_listenPortHost = 0;
        }

        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  " PubKey: "
                      << DataConversion::SerializableToHexStr(
                             m_myShardMembers->back().first)
                      << " IP: "
                      << m_myShardMembers->back().second.GetPrintableIPAddress()
                      << " Port: "
                      << m_myShardMembers->back().second.m_listenPortHost);

        index++;
    }

    return true;
}

void Node::LoadTxnSharingInfo()
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "Node::LoadTxnSharingInfo not expected to be called from "
                    "LookUp node.");
        return;
    }

    LOG_MARKER();

    m_txnSharingIAmSender = false;
    m_txnSharingIAmForwarder = false;
    m_txnSharingAssignedNodes.clear();

    // m_txnSharingAssignedNodes below is basically just the combination of ds_receivers, shard_receivers, and shard_senders
    // We will get rid of this inefficiency eventually

    m_txnSharingAssignedNodes.emplace_back();

    for (auto& m_DSReceiver : m_mediator.m_ds->m_DSReceivers)
    {
        m_txnSharingAssignedNodes.back().emplace_back(m_DSReceiver);
    }

    for (unsigned int i = 0; i < m_mediator.m_ds->m_shardReceivers.size(); i++)
    {
        m_txnSharingAssignedNodes.emplace_back();

        for (unsigned int j = 0;
             j < m_mediator.m_ds->m_shardReceivers.at(i).size(); j++)
        {
            m_txnSharingAssignedNodes.back().emplace_back(
                m_mediator.m_ds->m_shardReceivers.at(i).at(j));

            if ((i == m_myShardID)
                && (m_txnSharingAssignedNodes.back().back()
                    == m_mediator.m_selfPeer))
            {
                m_txnSharingIAmForwarder = true;
            }
        }

        m_txnSharingAssignedNodes.emplace_back();

        for (unsigned int j = 0;
             j < m_mediator.m_ds->m_shardSenders.at(i).size(); j++)
        {
            m_txnSharingAssignedNodes.back().emplace_back(
                m_mediator.m_ds->m_shardSenders.at(i).at(j));

            if ((i == m_myShardID)
                && (m_txnSharingAssignedNodes.back().back()
                    == m_mediator.m_selfPeer))
            {
                m_txnSharingIAmSender = true;
            }
        }
    }
}

void Node::StartFirstTxEpoch()
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "Node::StartFirstTxEpoch not expected to be called from "
                    "LookUp node.");
        return;
    }

    LOG_MARKER();

    // Check if I am the leader or backup of the shard
    if (m_mediator.m_selfKey.second == m_myShardMembers->front().first)
    {
        m_isPrimary = true;
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "I am leader of the sharded committee");

        LOG_STATE("[IDENT][" << std::setw(15) << std::left
                             << m_mediator.m_selfPeer.GetPrintableIPAddress()
                             << "][" << m_myShardID << "][0  ] SCLD");
    }
    else
    {
        m_isPrimary = false;

        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "I am backup member of the sharded committee");

        LOG_STATE("[SHSTU][" << setw(15) << left
                             << m_mediator.m_selfPeer.GetPrintableIPAddress()
                             << "]["
                             << m_mediator.m_txBlockChain.GetLastBlock()
                                    .GetHeader()
                                    .GetBlockNum()
                      + 1 << "] RECEIVED SHARDING STRUCTURE");

        LOG_STATE("[IDENT][" << std::setw(15) << std::left
                             << m_mediator.m_selfPeer.GetPrintableIPAddress()
                             << "][" << m_myShardID << "][" << std::setw(3)
                             << std::left << m_consensusMyID << "] SCBK");
    }

    // Choose 4 other nodes to be sender of microblock to ds committee.
    // TODO: Randomly choose these nodes?
    m_isMBSender = false;
    unsigned int numOfMBSender = 5;
    if (m_myShardMembers->size() < numOfMBSender)
    {
        numOfMBSender = m_myShardMembers->size();
    }

    // Shard leader will not have the flag set
    for (unsigned int i = 1; i < numOfMBSender; i++)
    {
        if (m_mediator.m_selfKey.second == m_myShardMembers->at(i).first)
        {
            // Selected node to be sender of its shard's micrblock
            m_isMBSender = true;
            break;
        }
    }

    m_consensusLeaderID = 0;
    CommitTxnPacketBuffer();

    auto main_func3 = [this]() mutable -> void { RunConsensusOnMicroBlock(); };

    DetachedFunction(1, main_func3);
}

bool Node::ProcessDSBlock(const vector<unsigned char>& message,
                          unsigned int cur_offset,
                          [[gnu::unused]] const Peer& from)
{
    LOG_MARKER();

    lock_guard<mutex> g(m_mutexDSBlock);

    if (!LOOKUP_NODE_MODE)
    {
        if (!CheckState(PROCESS_DSBLOCK))
        {
            return false;
        }

        // For running from genesis
        if (m_mediator.m_lookup->m_syncType != SyncType::NO_SYNC)
        {
            m_mediator.m_lookup->m_syncType = SyncType::NO_SYNC;
            if (m_fromNewProcess)
            {
                m_fromNewProcess = false;
            }

            // Are these necessary? Commented out for now
            //AccountStore::GetInstance().MoveUpdatesToDisk();
            //m_runFromLate = false;
        }
    }
    else
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "I the lookup node have received the DS Block");
    }

    DSBlock dsblock;
    uint32_t shardID;
    Peer newleaderIP;

    m_mediator.m_ds->m_shards.clear();
    m_mediator.m_ds->m_DSReceivers.clear();
    m_mediator.m_ds->m_shardReceivers.clear();
    m_mediator.m_ds->m_shardSenders.clear();

    if (!Messenger::GetNodeDSBlock(
            message, cur_offset, shardID, dsblock, newleaderIP,
            m_mediator.m_ds->m_shards, m_mediator.m_ds->m_DSReceivers,
            m_mediator.m_ds->m_shardReceivers, m_mediator.m_ds->m_shardSenders))
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Messenger::GetNodeDSBlock failed.");
        return false;
    }

    m_myShardID = shardID;

    LogReceivedDSBlockDetails(dsblock);

    // Checking for freshness of incoming DS Block
    if (!CheckWhetherDSBlockNumIsLatest(dsblock.GetHeader().GetBlockNum()))
    {
        return false;
    }

    // Check the signature of this DS block
    if (!VerifyDSBlockCoSignature(dsblock))
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "DSBlock co-sig verification failed");
        return false;
    }

    // Add to block chain and Store the DS block to disk.
    StoreDSBlockToDisk(dsblock);

    LOG_STATE(
        "[DSBLK]["
        << setw(15) << left << m_mediator.m_selfPeer.GetPrintableIPAddress()
        << "]["
        << m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum()
            + 1
        << "] RECEIVED DSBLOCK");

    if (LOOKUP_NODE_MODE)
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "I the lookup node have stored the DS Block");
    }

    m_mediator.UpdateDSBlockRand(); // Update the rand1 value for next PoW
    UpdateDSCommiteeComposition(newleaderIP);

    if (!LOOKUP_NODE_MODE)
    {
        uint32_t ds_size = m_mediator.m_DSCommittee->size();
        POW::GetInstance().StopMining();

        // If I am the next DS leader -> need to set myself up as a DS node
        if (m_mediator.m_selfKey.second
            == m_mediator.m_dsBlockChain.GetLastBlock()
                   .GetHeader()
                   .GetMinerPubKey())
        {
            LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "I won PoW :-) I am now in the DS committee !");

            // Process sharding structure as a DS node
            if (!m_mediator.m_ds->ProcessShardingStructure(
                    m_mediator.m_ds->m_shards,
                    m_mediator.m_ds->m_publicKeyToShardIdMap,
                    m_mediator.m_ds->m_mapNodeReputation))
            {
                return false;
            }

            // Process txn sharing assignments as a DS node
            m_mediator.m_ds->ProcessTxnBodySharingAssignment();

            // Update my ID
            m_mediator.m_ds->m_consensusID
                = m_mediator.m_currentEpochNum == 1 ? 1 : 0;
            m_mediator.m_ds->m_consensusMyID = 0;

            //(We're getting rid of this eventually Clean up my txns coz I am DS)
            m_mediator.m_node->CleanCreatedTransaction();

            uint16_t lastBlockHash = 0;
            if (m_mediator.m_currentEpochNum > 1)
            {
                lastBlockHash = HashUtils::SerializableToHash16Bits(
                    m_mediator.m_txBlockChain.GetLastBlock());
            }

            {

                lock_guard<mutex> g(m_mediator.m_mutexDSCommittee);
                unsigned int ds_size = (m_mediator.m_DSCommittee)->size();

                if (lastBlockHash % ds_size == 0)
                {
                    //I am the new DS committee leader
                    m_mediator.m_ds->m_mode
                        = DirectoryService::Mode::PRIMARY_DS;
                    LOG_EPOCHINFO(
                        to_string(m_mediator.m_currentEpochNum).c_str(),
                        DS_LEADER_MSG);
                    LOG_STATE("[IDENT]["
                              << std::setw(15) << std::left
                              << m_mediator.m_selfPeer.GetPrintableIPAddress()
                              << "][0     ] DSLD");
                }
                else
                {
                    m_mediator.m_ds->m_mode = DirectoryService::Mode::BACKUP_DS;
                    LOG_EPOCHINFO(
                        to_string(m_mediator.m_currentEpochNum).c_str(),
                        DS_BACKUP_MSG);
                }
            }
            m_mediator.m_ds->m_consensusLeaderID = lastBlockHash % ds_size;
            m_mediator.m_ds->StartFirstTxEpoch();
            //m_mediator.m_ds->m_mode = DirectoryService::Mode::PRIMARY_DS;
        }
        // If I am a shard node
        else
        {
            LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "I lost PoW :-( Better luck next time!");

            // Process sharding structure as a shard node
            if (!LoadShardingStructure())
            {
                return false;
            }

            // Process txn sharing assignments as a shard node
            LoadTxnSharingInfo();

            // Finally, start as a shard node
            StartFirstTxEpoch();
        }
    }
    else
    {
        // Process sharding structure as a lookup node
        m_mediator.m_lookup->ProcessEntireShardingStructure();

        if (m_mediator.m_lookup->GetIsServer() && USE_REMOTE_TXN_CREATOR)
        {
            m_mediator.m_lookup->SenderTxnBatchThread();
        }
    }
    return true;
}
