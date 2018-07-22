// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ZERONODEMAN_H
#define ZERONODEMAN_H

#include "zeronode.h"
#include "sync.h"

using namespace std;

class CZeronodeMan;

extern CZeronodeMan mnodeman;

/**
 * Provides a forward and reverse index between MN vin's and integers.
 *
 * This mapping is normally add-only and is expected to be permanent
 * It is only rebuilt if the size of the index exceeds the expected maximum number
 * of MN's and the current number of known MN's.
 *
 * The external interface to this index is provided via delegation by CZeronodeMan
 */
class CZeronodeIndex
{
public: // Types
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

    typedef std::map<int,CTxIn> rindex_m_t;

    typedef rindex_m_t::iterator rindex_m_it;

    typedef rindex_m_t::const_iterator rindex_m_cit;

private:
    int                  nSize;

    index_m_t            mapIndex;

    rindex_m_t           mapReverseIndex;

public:
    CZeronodeIndex();

    int GetSize() const {
        return nSize;
    }

    /// Retrieve zeronode vin by index
    bool Get(int nIndex, CTxIn& vinZeronode) const;

    /// Get index of a zeronode vin
    int GetZeronodeIndex(const CTxIn& vinZeronode) const;

    void AddZeronodeVIN(const CTxIn& vinZeronode);

    void Clear();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapIndex);
        if(ser_action.ForRead()) {
            RebuildIndex();
        }
    }

private:
    void RebuildIndex();

};

class CZeronodeMan
{
public:
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

private:
    static const int MAX_EXPECTED_INDEX_SIZE = 30000;

    /// Only allow 1 index rebuild per hour
    static const int64_t MIN_INDEX_REBUILD_TIME = 3600;

    static const std::string SERIALIZATION_VERSION_STRING;

    static const int DSEG_UPDATE_SECONDS        = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS      = 100;

    static const int MIN_POSE_PROTO_VERSION     = 70203;
    static const int MAX_POSE_CONNECTIONS       = 10;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;

    static const int MNB_RECOVERY_QUORUM_TOTAL      = 10;
    static const int MNB_RECOVERY_QUORUM_REQUIRED   = 6;
    static const int MNB_RECOVERY_MAX_ASK_ENTRIES   = 10;
    static const int MNB_RECOVERY_WAIT_SECONDS      = 60;
    static const int MNB_RECOVERY_RETRY_SECONDS     = 3 * 60 * 60;


    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    // map to hold all MNs
    std::vector<CZeronode> vZeronodes;
    // who's asked for the Zeronode list and the last time
    std::map<CNetAddr, int64_t> mAskedUsForZeronodeList;
    // who we asked for the Zeronode list and the last time
    std::map<CNetAddr, int64_t> mWeAskedForZeronodeList;
    // which Zeronodes we've asked for
    std::map<COutPoint, std::map<CNetAddr, int64_t> > mWeAskedForZeronodeListEntry;
    // who we asked for the zeronode verification
    std::map<CNetAddr, CZeronodeVerification> mWeAskedForVerification;

    // these maps are used for zeronode recovery from ZERONODE_NEW_START_REQUIRED state
    std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > > mMnbRecoveryRequests;
    std::map<uint256, std::vector<CZeronodeBroadcast> > mMnbRecoveryGoodReplies;
    std::list< std::pair<CService, uint256> > listScheduledMnbRequestConnections;

    int64_t nLastIndexRebuildTime;

    CZeronodeIndex indexZeronodes;

    CZeronodeIndex indexZeronodesOld;

    /// Set when index has been rebuilt, clear when read
    bool fIndexRebuilt;

    /// Set when zeronodes are added, cleared when CGovernanceManager is notified
    bool fZeronodesAdded;

    /// Set when zeronodes are removed, cleared when CGovernanceManager is notified
    bool fZeronodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    int64_t nLastWatchdogVoteTime;

    friend class CZeronodeSync;

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, std::pair<int64_t, CZeronodeBroadcast> > mapSeenZeronodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CZeronodePing> mapSeenZeronodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CZeronodeVerification> mapSeenZeronodeVerification;
    // keep track of dsq count to prevent zeronodes from gaming darksend queue
    int64_t nDsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING; 
            READWRITE(strVersion);
        }

        READWRITE(vZeronodes);
        READWRITE(mAskedUsForZeronodeList);
        READWRITE(mWeAskedForZeronodeList);
        READWRITE(mWeAskedForZeronodeListEntry);
        READWRITE(mMnbRecoveryRequests);
        READWRITE(mMnbRecoveryGoodReplies);
        READWRITE(nLastWatchdogVoteTime);
        READWRITE(nDsqCount);

        READWRITE(mapSeenZeronodeBroadcast);
        READWRITE(mapSeenZeronodePing);
        READWRITE(indexZeronodes);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CZeronodeMan();

    /// Add an entry
    bool Add(CZeronode &mn);

    /// Ask (source) node for mnb
    void AskForMN(CNode *pnode, const CTxIn &vin);
    void AskForMnb(CNode *pnode, const uint256 &hash);

    /// Check all Zeronodes
    void Check();

    /// Check all Zeronodes and remove inactive
    void CheckAndRemove();

    /// Clear Zeronode vector
    void Clear();

    /// Count Zeronodes filtered by nProtocolVersion.
    /// Zeronode nProtocolVersion should match or be above the one specified in param here.
    int CountZeronodes(int nProtocolVersion = -1);
    /// Count enabled Zeronodes filtered by nProtocolVersion.
    /// Zeronode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1);

    /// Count Zeronodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void DsegUpdate(CNode* pnode);

    /// Find an entry
    CZeronode* Find(const CScript &payee);
    CZeronode* Find(const CTxIn& vin);
    CZeronode* Find(const CPubKey& pubKeyZeronode);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const CPubKey& pubKeyZeronode, CZeronode& zeronode);
    bool Get(const CTxIn& vin, CZeronode& zeronode);

    /// Retrieve zeronode vin by index
    bool Get(int nIndex, CTxIn& vinZeronode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexZeronodes.Get(nIndex, vinZeronode);
    }

    bool GetIndexRebuiltFlag() {
        LOCK(cs);
        return fIndexRebuilt;
    }

    /// Get index of a zeronode vin
    int GetZeronodeIndex(const CTxIn& vinZeronode) {
        LOCK(cs);
        return indexZeronodes.GetZeronodeIndex(vinZeronode);
    }

    /// Get old index of a zeronode vin
    int GetZeronodeIndexOld(const CTxIn& vinZeronode) {
        LOCK(cs);
        return indexZeronodesOld.GetZeronodeIndex(vinZeronode);
    }

    /// Get zeronode VIN for an old index value
    bool GetZeronodeVinForIndexOld(int nZeronodeIndex, CTxIn& vinZeronodeOut) {
        LOCK(cs);
        return indexZeronodesOld.Get(nZeronodeIndex, vinZeronodeOut);
    }

    /// Get index of a zeronode vin, returning rebuild flag
    int GetZeronodeIndex(const CTxIn& vinZeronode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexZeronodes.GetZeronodeIndex(vinZeronode);
    }

    void ClearOldZeronodeIndex() {
        LOCK(cs);
        indexZeronodesOld.Clear();
        fIndexRebuilt = false;
    }

    bool Has(const CTxIn& vin);

    zeronode_info_t GetZeronodeInfo(const CTxIn& vin);

    zeronode_info_t GetZeronodeInfo(const CPubKey& pubKeyZeronode);

    char* GetNotQualifyReason(CZeronode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount);

    /// Find an entry in the zeronode list that is next to be paid
    CZeronode* GetNextZeronodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount);
    /// Same as above but use current block height
    CZeronode* GetNextZeronodeInQueueForPayment(bool fFilterSigTime, int& nCount);

    /// Find a random entry
    CZeronode* FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion = -1);

    std::vector<CZeronode> GetFullZeronodeVector() { return vZeronodes; }

    std::vector<std::pair<int, CZeronode> > GetZeronodeRanks(int nBlockHeight = -1, int nMinProtocol=0);
    int GetZeronodeRank(const CTxIn &vin, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);
    CZeronode* GetZeronodeByRank(int nRank, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);

    void ProcessZeronodeConnections();
    std::pair<CService, std::set<uint256> > PopScheduledMnbRequestConnection();

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

    void DoFullVerificationStep();
    void CheckSameAddr();
    bool SendVerifyRequest(const CAddress& addr, const std::vector<CZeronode*>& vSortedByAddr);
    void SendVerifyReply(CNode* pnode, CZeronodeVerification& mnv);
    void ProcessVerifyReply(CNode* pnode, CZeronodeVerification& mnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CZeronodeVerification& mnv);

    /// Return the number of (unique) Zeronodes
    int size() { return vZeronodes.size(); }

    std::string ToString() const;

    /// Update zeronode list and maps using provided CZeronodeBroadcast
    void UpdateZeronodeList(CZeronodeBroadcast mnb);
    /// Perform complete check and only then update list and maps
    bool CheckMnbAndUpdateZeronodeList(CNode* pfrom, CZeronodeBroadcast mnb, int& nDos);
    bool IsMnbRecoveryRequested(const uint256& hash) { return mMnbRecoveryRequests.count(hash); }

    void UpdateLastPaid();

    void CheckAndRebuildZeronodeIndex();

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;;
    }

    bool IsWatchdogActive();
    void UpdateWatchdogVoteTime(const CTxIn& vin);
    bool AddGovernanceVote(const CTxIn& vin, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void CheckZeronode(const CTxIn& vin, bool fForce = false);
    void CheckZeronode(const CPubKey& pubKeyZeronode, bool fForce = false);

    int GetZeronodeState(const CTxIn& vin);
    int GetZeronodeState(const CPubKey& pubKeyZeronode);

    bool IsZeronodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetZeronodeLastPing(const CTxIn& vin, const CZeronodePing& mnp);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    /**
     * Called to notify CGovernanceManager that the zeronode index has been updated.
     * Must be called while not holding the CZeronodeMan::cs mutex
     */
    void NotifyZeronodeUpdates();

};

#endif
