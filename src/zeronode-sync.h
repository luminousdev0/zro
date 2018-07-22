// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef ZERONODE_SYNC_H
#define ZERONODE_SYNC_H

#include "chain.h"
#include "net.h"

#include <univalue.h>

class CZeronodeSync;

static const int ZERONODE_SYNC_FAILED          = -1;
static const int ZERONODE_SYNC_INITIAL         = 0;
static const int ZERONODE_SYNC_SPORKS          = 1;
static const int ZERONODE_SYNC_LIST            = 2;
static const int ZERONODE_SYNC_MNW             = 3;
static const int ZERONODE_SYNC_FINISHED        = 999;

static const int ZERONODE_SYNC_TICK_SECONDS    = 6;
static const int ZERONODE_SYNC_TIMEOUT_SECONDS = 30; // our blocks are 2.5 minutes so 30 seconds should be fine

//static const int ZERONODE_SYNC_ENOUGH_PEERS    = 6;
static const int ZERONODE_SYNC_ENOUGH_PEERS    = 3;

extern CZeronodeSync zeronodeSync;

//
// CZeronodeSync : Sync zeronode assets in stages
//

class CZeronodeSync
{
private:
    // Keep track of current asset
    int nRequestedZeronodeAssets;
    // Count peers we've requested the asset from
    int nRequestedZeronodeAttempt;

    // Time when current zeronode asset sync started
    int64_t nTimeAssetSyncStarted;

    // Last time when we received some zeronode asset ...
    int64_t nTimeLastZeronodeList;
    int64_t nTimeLastPaymentVote;
    int64_t nTimeLastGovernanceItem;
    // ... or failed
    int64_t nTimeLastFailure;

    // How many times we failed
    int nCountFailures;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    bool CheckNodeHeight(CNode* pnode, bool fDisconnectStuckNodes = false);
    void Fail();
    void ClearFulfilledRequests();

public:
    CZeronodeSync() { Reset(); }

    void AddedZeronodeList() { nTimeLastZeronodeList = GetTime(); }
    void AddedPaymentVote() { nTimeLastPaymentVote = GetTime(); }
    void AddedGovernanceItem() { nTimeLastGovernanceItem = GetTime(); };

    void SendGovernanceSyncRequest(CNode* pnode);

    bool IsFailed() { return nRequestedZeronodeAssets == ZERONODE_SYNC_FAILED; }
    bool IsBlockchainSynced(bool fBlockAccepted = false);
    bool IsZeronodeListSynced() { return nRequestedZeronodeAssets > ZERONODE_SYNC_LIST; }
    bool IsWinnersListSynced() { return nRequestedZeronodeAssets > ZERONODE_SYNC_MNW; }
    bool IsSynced() { return nRequestedZeronodeAssets == ZERONODE_SYNC_FINISHED; }

    int GetAssetID() { return nRequestedZeronodeAssets; }
    int GetAttempt() { return nRequestedZeronodeAttempt; }
    std::string GetAssetName();
    std::string GetSyncStatus();

    void Reset();
    void SwitchToNextAsset();

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    void ProcessTick();

    void UpdatedBlockTip(const CBlockIndex *pindex);
};

#endif
