// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activezeronode.h"
#include "addrman.h"
#include "darksend.h"
//#include "governance.h"
#include "zeronode-payments.h"
#include "zeronode-sync.h"
#include "zeronodeman.h"
#include "netfulfilledman.h"
#include "util.h"

/** Zeronode manager */
CZeronodeMan mnodeman;

const std::string CZeronodeMan::SERIALIZATION_VERSION_STRING = "CZeronodeMan-Version-4";

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, CZeronode*>& t1,
                    const std::pair<int, CZeronode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<int64_t, CZeronode*>& t1,
                    const std::pair<int64_t, CZeronode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

CZeronodeIndex::CZeronodeIndex()
    : nSize(0),
      mapIndex(),
      mapReverseIndex()
{}

bool CZeronodeIndex::Get(int nIndex, CTxIn& vinZeronode) const
{
    rindex_m_cit it = mapReverseIndex.find(nIndex);
    if(it == mapReverseIndex.end()) {
        return false;
    }
    vinZeronode = it->second;
    return true;
}

int CZeronodeIndex::GetZeronodeIndex(const CTxIn& vinZeronode) const
{
    index_m_cit it = mapIndex.find(vinZeronode);
    if(it == mapIndex.end()) {
        return -1;
    }
    return it->second;
}

void CZeronodeIndex::AddZeronodeVIN(const CTxIn& vinZeronode)
{
    index_m_it it = mapIndex.find(vinZeronode);
    if(it != mapIndex.end()) {
        return;
    }
    int nNextIndex = nSize;
    mapIndex[vinZeronode] = nNextIndex;
    mapReverseIndex[nNextIndex] = vinZeronode;
    ++nSize;
}

void CZeronodeIndex::Clear()
{
    mapIndex.clear();
    mapReverseIndex.clear();
    nSize = 0;
}
struct CompareByAddr

{
    bool operator()(const CZeronode* t1,
                    const CZeronode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

void CZeronodeIndex::RebuildIndex()
{
    nSize = mapIndex.size();
    for(index_m_it it = mapIndex.begin(); it != mapIndex.end(); ++it) {
        mapReverseIndex[it->second] = it->first;
    }
}

CZeronodeMan::CZeronodeMan() : cs(),
  vZeronodes(),
  mAskedUsForZeronodeList(),
  mWeAskedForZeronodeList(),
  mWeAskedForZeronodeListEntry(),
  mWeAskedForVerification(),
  mMnbRecoveryRequests(),
  mMnbRecoveryGoodReplies(),
  listScheduledMnbRequestConnections(),
  nLastIndexRebuildTime(0),
  indexZeronodes(),
  indexZeronodesOld(),
  fIndexRebuilt(false),
  fZeronodesAdded(false),
  fZeronodesRemoved(false),
//  vecDirtyGovernanceObjectHashes(),
  nLastWatchdogVoteTime(0),
  mapSeenZeronodeBroadcast(),
  mapSeenZeronodePing(),
  nDsqCount(0)
{}

bool CZeronodeMan::Add(CZeronode &mn)
{
    LOCK(cs);

    CZeronode *pmn = Find(mn.vin);
    if (pmn == NULL) {
        LogPrint("zeronode", "CZeronodeMan::Add -- Adding new Zeronode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
        vZeronodes.push_back(mn);
        indexZeronodes.AddZeronodeVIN(mn.vin);
        fZeronodesAdded = true;
        return true;
    }

    return false;
}

void CZeronodeMan::AskForMN(CNode* pnode, const CTxIn &vin)
{
    if(!pnode) return;

    LOCK(cs);

    std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it1 = mWeAskedForZeronodeListEntry.find(vin.prevout);
    if (it1 != mWeAskedForZeronodeListEntry.end()) {
        std::map<CNetAddr, int64_t>::iterator it2 = it1->second.find(pnode->addr);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CZeronodeMan::AskForMN -- Asking same peer %s for missing zeronode entry again: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CZeronodeMan::AskForMN -- Asking new peer %s for missing zeronode entry: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CZeronodeMan::AskForMN -- Asking peer %s for missing zeronode entry for the first time: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
    }
    mWeAskedForZeronodeListEntry[vin.prevout][pnode->addr] = GetTime() + DSEG_UPDATE_SECONDS;

    pnode->PushMessage(NetMsgType::DSEG, vin);
}

void CZeronodeMan::Check()
{
    LOCK(cs);

//    LogPrint("zeronode", "CZeronodeMan::Check -- nLastWatchdogVoteTime=%d, IsWatchdogActive()=%d\n", nLastWatchdogVoteTime, IsWatchdogActive());

    BOOST_FOREACH(CZeronode& mn, vZeronodes) {
        mn.Check();
    }
}

void CZeronodeMan::CheckAndRemove()
{
    if(!zeronodeSync.IsZeronodeListSynced()) return;

    LogPrintf("CZeronodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateZeronodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent zeronodes, prepare structures and make requests to reasure the state of inactive ones
        std::vector<CZeronode>::iterator it = vZeronodes.begin();
        std::vector<std::pair<int, CZeronode> > vecZeronodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES zeronode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        while(it != vZeronodes.end()) {
            CZeronodeBroadcast mnb = CZeronodeBroadcast(*it);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if ((*it).IsOutpointSpent()) {
                LogPrint("zeronode", "CZeronodeMan::CheckAndRemove -- Removing Zeronode: %s  addr=%s  %i now\n", (*it).GetStateString(), (*it).addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenZeronodeBroadcast.erase(hash);
                mWeAskedForZeronodeListEntry.erase((*it).vin.prevout);

                // and finally remove it from the list
//                it->FlagGovernanceItemsAsDirty();
                it = vZeronodes.erase(it);
                fZeronodesRemoved = true;
            } else {
                bool fAsk = pCurrentBlockIndex &&
                            (nAskForMnbRecovery > 0) &&
                            zeronodeSync.IsSynced() &&
                            it->IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash);
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CNetAddr> setRequested;
                    // calulate only once and only when it's needed
                    if(vecZeronodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(pCurrentBlockIndex->nHeight);
                        vecZeronodeRanks = GetZeronodeRanks(nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL zeronodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecZeronodeRanks.size(); i++) {
                        // avoid banning
                        if(mWeAskedForZeronodeListEntry.count(it->vin.prevout) && mWeAskedForZeronodeListEntry[it->vin.prevout].count(vecZeronodeRanks[i].second.addr)) continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecZeronodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if(fAskedForMnbRecovery) {
                        LogPrint("zeronode", "CZeronodeMan::CheckAndRemove -- Recovery initiated, zeronode=%s\n", it->vin.prevout.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for ZERONODE_NEW_START_REQUIRED zeronodes
        LogPrint("zeronode", "CZeronodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CZeronodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint("zeronode", "CZeronodeMan::CheckAndRemove -- reprocessing mnb, zeronode=%s\n", itMnbReplies->second[0].vin.prevout.ToStringShort());
                    // mapSeenZeronodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateZeronodeList(NULL, itMnbReplies->second[0], nDos);
                }
                LogPrint("zeronode", "CZeronodeMan::CheckAndRemove -- removing mnb recovery reply, zeronode=%s, size=%d\n", itMnbReplies->second[0].vin.prevout.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > >::iterator itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in ZERONODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Zeronode list
        std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForZeronodeList.begin();
        while(it1 != mAskedUsForZeronodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForZeronodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Zeronode list
        it1 = mWeAskedForZeronodeList.begin();
        while(it1 != mWeAskedForZeronodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForZeronodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Zeronodes we've asked for
        std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it2 = mWeAskedForZeronodeListEntry.begin();
        while(it2 != mWeAskedForZeronodeListEntry.end()){
            std::map<CNetAddr, int64_t>::iterator it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForZeronodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        std::map<CNetAddr, CZeronodeVerification>::iterator it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenZeronodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenZeronodePing
        std::map<uint256, CZeronodePing>::iterator it4 = mapSeenZeronodePing.begin();
        while(it4 != mapSeenZeronodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint("zeronode", "CZeronodeMan::CheckAndRemove -- Removing expired Zeronode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenZeronodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenZeronodeVerification
        std::map<uint256, CZeronodeVerification>::iterator itv2 = mapSeenZeronodeVerification.begin();
        while(itv2 != mapSeenZeronodeVerification.end()){
            if((*itv2).second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS){
                LogPrint("zeronode", "CZeronodeMan::CheckAndRemove -- Removing expired Zeronode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenZeronodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CZeronodeMan::CheckAndRemove -- %s\n", ToString());

        if(fZeronodesRemoved) {
            CheckAndRebuildZeronodeIndex();
        }
    }

    if(fZeronodesRemoved) {
        NotifyZeronodeUpdates();
    }
}

void CZeronodeMan::Clear()
{
    LOCK(cs);
    vZeronodes.clear();
    mAskedUsForZeronodeList.clear();
    mWeAskedForZeronodeList.clear();
    mWeAskedForZeronodeListEntry.clear();
    mapSeenZeronodeBroadcast.clear();
    mapSeenZeronodePing.clear();
    nDsqCount = 0;
    nLastWatchdogVoteTime = 0;
    indexZeronodes.Clear();
    indexZeronodesOld.Clear();
}

int CZeronodeMan::CountZeronodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinZeronodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CZeronode& mn, vZeronodes) {
        if(mn.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CZeronodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinZeronodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CZeronode& mn, vZeronodes) {
        if(mn.nProtocolVersion < nProtocolVersion || !mn.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 zeronodes are allowed in 12.1, saving this for later
int CZeronodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    BOOST_FOREACH(CZeronode& mn, vZeronodes)
        if ((nNetworkType == NET_IPV4 && mn.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mn.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mn.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CZeronodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForZeronodeList.find(pnode->addr);
            if(it != mWeAskedForZeronodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CZeronodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", pnode->addr.ToString());
                return;
            }
        }
    }
    
    pnode->PushMessage(NetMsgType::DSEG, CTxIn());
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForZeronodeList[pnode->addr] = askAgain;

    LogPrint("zeronode", "CZeronodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CZeronode* CZeronodeMan::Find(const CScript &payee)
{
    LOCK(cs);

    BOOST_FOREACH(CZeronode& mn, vZeronodes)
    {
        if(GetScriptForDestination(mn.pubKeyCollateralAddress.GetID()) == payee)
            return &mn;
    }
    return NULL;
}

CZeronode* CZeronodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CZeronode& mn, vZeronodes)
    {
        if(mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return NULL;
}

CZeronode* CZeronodeMan::Find(const CPubKey &pubKeyZeronode)
{
    LOCK(cs);

    BOOST_FOREACH(CZeronode& mn, vZeronodes)
    {
        if(mn.pubKeyZeronode == pubKeyZeronode)
            return &mn;
    }
    return NULL;
}

bool CZeronodeMan::Get(const CPubKey& pubKeyZeronode, CZeronode& zeronode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CZeronode* pMN = Find(pubKeyZeronode);
    if(!pMN)  {
        return false;
    }
    zeronode = *pMN;
    return true;
}

bool CZeronodeMan::Get(const CTxIn& vin, CZeronode& zeronode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CZeronode* pMN = Find(vin);
    if(!pMN)  {
        return false;
    }
    zeronode = *pMN;
    return true;
}

zeronode_info_t CZeronodeMan::GetZeronodeInfo(const CTxIn& vin)
{
    zeronode_info_t info;
    LOCK(cs);
    CZeronode* pMN = Find(vin);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

zeronode_info_t CZeronodeMan::GetZeronodeInfo(const CPubKey& pubKeyZeronode)
{
    zeronode_info_t info;
    LOCK(cs);
    CZeronode* pMN = Find(pubKeyZeronode);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

bool CZeronodeMan::Has(const CTxIn& vin)
{
    LOCK(cs);
    CZeronode* pMN = Find(vin);
    return (pMN != NULL);
}

char* CZeronodeMan::GetNotQualifyReason(CZeronode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount)
{
    if (!mn.IsValidForPayment()) {
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'not valid for payment'");
        return reasonStr;
    }
    // //check protocol version
    if (mn.nProtocolVersion < mnpayments.GetMinZeronodePaymentsProto()) {
        // LogPrintf("Invalid nProtocolVersion!\n");
        // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
        // LogPrintf("mnpayments.GetMinZeronodePaymentsProto=%s!\n", mnpayments.GetMinZeronodePaymentsProto());
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'Invalid nProtocolVersion', nProtocolVersion=%d", mn.nProtocolVersion);
        return reasonStr;
    }
    //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
    if (mnpayments.IsScheduled(mn, nBlockHeight)) {
        // LogPrintf("mnpayments.IsScheduled!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'is scheduled'");
        return reasonStr;
    }
    //it's too new, wait for a cycle
    if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
        // LogPrintf("it's too new, wait for a cycle!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'too new', sigTime=%s, will be qualifed after=%s",
                DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
        return reasonStr;
    }
    //make sure it has at least as many confirmations as there are zeronodes
    if (mn.GetCollateralAge() < nMnCount) {
        // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
        // LogPrintf("nMnCount=%s!\n", nMnCount);
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'collateralAge < znCount', collateralAge=%d, znCount=%d", mn.GetCollateralAge(), nMnCount);
        return reasonStr;
    }
    return NULL;
}

//
// Deterministically select the oldest/best zeronode to pay on the network
//
CZeronode* CZeronodeMan::GetNextZeronodeInQueueForPayment(bool fFilterSigTime, int& nCount)
{
    if(!pCurrentBlockIndex) {
        nCount = 0;
        return NULL;
    }
    return GetNextZeronodeInQueueForPayment(pCurrentBlockIndex->nHeight, fFilterSigTime, nCount);
}

CZeronode* CZeronodeMan::GetNextZeronodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount)
{
    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    CZeronode *pBestZeronode = NULL;
    std::vector<std::pair<int, CZeronode*> > vecZeronodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */
    int nMnCount = CountEnabled();
    int index = 0;
    BOOST_FOREACH(CZeronode &mn, vZeronodes)
    {
        index += 1;
        // LogPrintf("index=%s, mn=%s\n", index, mn.ToString());
        /*if (!mn.IsValidForPayment()) {
            LogPrint("zeronodeman", "Zeronode, %s, addr(%s), not-qualified: 'not valid for payment'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        // //check protocol version
        if (mn.nProtocolVersion < mnpayments.GetMinZeronodePaymentsProto()) {
            // LogPrintf("Invalid nProtocolVersion!\n");
            // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
            // LogPrintf("mnpayments.GetMinZeronodePaymentsProto=%s!\n", mnpayments.GetMinZeronodePaymentsProto());
            LogPrint("zeronodeman", "Zeronode, %s, addr(%s), not-qualified: 'invalid nProtocolVersion'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (mnpayments.IsScheduled(mn, nBlockHeight)) {
            // LogPrintf("mnpayments.IsScheduled!\n");
            LogPrint("zeronodeman", "Zeronode, %s, addr(%s), not-qualified: 'IsScheduled'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's too new, wait for a cycle
        if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
            // LogPrintf("it's too new, wait for a cycle!\n");
            LogPrint("zeronodeman", "Zeronode, %s, addr(%s), not-qualified: 'it's too new, wait for a cycle!', sigTime=%s, will be qualifed after=%s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
            continue;
        }
        //make sure it has at least as many confirmations as there are zeronodes
        if (mn.GetCollateralAge() < nMnCount) {
            // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
            // LogPrintf("nMnCount=%s!\n", nMnCount);
            LogPrint("zeronodeman", "Zeronode, %s, addr(%s), not-qualified: 'mn.GetCollateralAge() < nMnCount', CollateralAge=%d, nMnCount=%d\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), mn.GetCollateralAge(), nMnCount);
            continue;
        }*/
        char* reasonStr = GetNotQualifyReason(mn, nBlockHeight, fFilterSigTime, nMnCount);
        if (reasonStr != NULL) {
            LogPrint("zeronodeman", "Zeronode, %s, addr(%s), qualify %s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), reasonStr);
            delete [] reasonStr;
            continue;
        }
        vecZeronodeLastPaid.push_back(std::make_pair(mn.GetLastPaidBlock(), &mn));
    }
    nCount = (int)vecZeronodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCount < nMnCount / 3) {
        // LogPrintf("Need Return, nCount=%s, nMnCount/3=%s\n", nCount, nMnCount/3);
        return GetNextZeronodeInQueueForPayment(nBlockHeight, false, nCount);
    }

    // Sort them low to high
    sort(vecZeronodeLastPaid.begin(), vecZeronodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CZeronode::GetNextZeronodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return NULL;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    BOOST_FOREACH (PAIRTYPE(int, CZeronode*)& s, vecZeronodeLastPaid){
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestZeronode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    return pBestZeronode;
}

CZeronode* CZeronodeMan::FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinZeronodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CZeronodeMan::FindRandomNotInVec -- %d enabled zeronodes, %d zeronodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return NULL;

    // fill a vector of pointers
    std::vector<CZeronode*> vpZeronodesShuffled;
    BOOST_FOREACH(CZeronode &mn, vZeronodes) {
        vpZeronodesShuffled.push_back(&mn);
    }

    InsecureRand insecureRand;
    // shuffle pointers
    std::random_shuffle(vpZeronodesShuffled.begin(), vpZeronodesShuffled.end(), insecureRand);
    bool fExclude;

    // loop through
    BOOST_FOREACH(CZeronode* pmn, vpZeronodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        BOOST_FOREACH(const CTxIn &txinToExclude, vecToExclude) {
            if(pmn->vin.prevout == txinToExclude.prevout) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("zeronode", "CZeronodeMan::FindRandomNotInVec -- found, zeronode=%s\n", pmn->vin.prevout.ToStringShort());
        return pmn;
    }

    LogPrint("zeronode", "CZeronodeMan::FindRandomNotInVec -- failed\n");
    return NULL;
}

int CZeronodeMan::GetZeronodeRank(const CTxIn& vin, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CZeronode*> > vecZeronodeScores;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return -1;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CZeronode& mn, vZeronodes) {
        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive) {
            if(!mn.IsEnabled()) continue;
        }
        else {
            if(!mn.IsValidForPayment()) continue;
        }
        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecZeronodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecZeronodeScores.rbegin(), vecZeronodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CZeronode*)& scorePair, vecZeronodeScores) {
        nRank++;
        if(scorePair.second->vin.prevout == vin.prevout) return nRank;
    }

    return -1;
}

std::vector<std::pair<int, CZeronode> > CZeronodeMan::GetZeronodeRanks(int nBlockHeight, int nMinProtocol)
{
    std::vector<std::pair<int64_t, CZeronode*> > vecZeronodeScores;
    std::vector<std::pair<int, CZeronode> > vecZeronodeRanks;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return vecZeronodeRanks;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CZeronode& mn, vZeronodes) {

        if(mn.nProtocolVersion < nMinProtocol || !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecZeronodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecZeronodeScores.rbegin(), vecZeronodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CZeronode*)& s, vecZeronodeScores) {
        nRank++;
        vecZeronodeRanks.push_back(std::make_pair(nRank, *s.second));
    }

    return vecZeronodeRanks;
}

CZeronode* CZeronodeMan::GetZeronodeByRank(int nRank, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CZeronode*> > vecZeronodeScores;

    LOCK(cs);

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight)) {
        LogPrintf("CZeronode::GetZeronodeByRank -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight);
        return NULL;
    }

    // Fill scores
    BOOST_FOREACH(CZeronode& mn, vZeronodes) {

        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive && !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecZeronodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecZeronodeScores.rbegin(), vecZeronodeScores.rend(), CompareScoreMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CZeronode*)& s, vecZeronodeScores){
        rank++;
        if(rank == nRank) {
            return s.second;
        }
    }

    return NULL;
}

void CZeronodeMan::ProcessZeronodeConnections()
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if(pnode->fZeronode) {
            if(darkSendPool.pSubmittedToZeronode != NULL && pnode->addr == darkSendPool.pSubmittedToZeronode->addr) continue;
            // LogPrintf("Closing Zeronode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    }
}

std::pair<CService, std::set<uint256> > CZeronodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}


void CZeronodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

//    LogPrint("zeronode", "CZeronodeMan::ProcessMessage, strCommand=%s\n", strCommand);
    if(fLiteMode) return; // disable all Dash specific functionality
    if(!zeronodeSync.IsBlockchainSynced()) return;

    if (strCommand == NetMsgType::MNANNOUNCE) { //Zeronode Broadcast
        CZeronodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        LogPrintf("MNANNOUNCE -- Zeronode announce, zeronode=%s\n", mnb.vin.prevout.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateZeronodeList(pfrom, mnb, nDos)) {
            // use announced Zeronode as a peer
            addrman.Add(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            Misbehaving(pfrom->GetId(), nDos);
        }

        if(fZeronodesAdded) {
            NotifyZeronodeUpdates();
        }
    } else if (strCommand == NetMsgType::MNPING) { //Zeronode Ping

        CZeronodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        LogPrint("zeronode", "MNPING -- Zeronode ping, zeronode=%s\n", mnp.vin.prevout.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenZeronodePing.count(nHash)) return; //seen
        mapSeenZeronodePing.insert(std::make_pair(nHash, mnp));

        LogPrint("zeronode", "MNPING -- Zeronode ping, zeronode=%s new\n", mnp.vin.prevout.ToStringShort());

        // see if we have this Zeronode
        CZeronode* pmn = mnodeman.Find(mnp.vin);

        // too late, new MNANNOUNCE is required
        if(pmn && pmn->IsNewStartRequired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a zeronode entry once
        AskForMN(pfrom, mnp.vin);

    } else if (strCommand == NetMsgType::DSEG) { //Get Zeronode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after zeronode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!zeronodeSync.IsSynced()) return;

        CTxIn vin;
        vRecv >> vin;

        LogPrint("zeronode", "DSEG -- Zeronode list, zeronode=%s\n", vin.prevout.ToStringShort());

        LOCK(cs);

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForZeronodeList.find(pfrom->addr);
                if (i != mAskedUsForZeronodeList.end()){
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("DSEG -- peer already asked me for the list, peer=%d\n", pfrom->id);
                        return;
                    }
                }
                int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
                mAskedUsForZeronodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int nInvCount = 0;

        BOOST_FOREACH(CZeronode& mn, vZeronodes) {
            if (vin != CTxIn() && vin != mn.vin) continue; // asked for specific vin but we are not there yet
            if (mn.addr.IsRFC1918() || mn.addr.IsLocal()) continue; // do not send local network zeronode
            if (mn.IsUpdateRequired()) continue; // do not send outdated zeronodes

            LogPrint("zeronode", "DSEG -- Sending Zeronode entry: zeronode=%s  addr=%s\n", mn.vin.prevout.ToStringShort(), mn.addr.ToString());
            CZeronodeBroadcast mnb = CZeronodeBroadcast(mn);
            uint256 hash = mnb.GetHash();
            pfrom->PushInventory(CInv(MSG_ZERONODE_ANNOUNCE, hash));
            pfrom->PushInventory(CInv(MSG_ZERONODE_PING, mn.lastPing.GetHash()));
            nInvCount++;

            if (!mapSeenZeronodeBroadcast.count(hash)) {
                mapSeenZeronodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));
            }

            if (vin == mn.vin) {
                LogPrintf("DSEG -- Sent 1 Zeronode inv to peer %d\n", pfrom->id);
                return;
            }
        }

        if(vin == CTxIn()) {
            pfrom->PushMessage(NetMsgType::SYNCSTATUSCOUNT, ZERONODE_SYNC_LIST, nInvCount);
            LogPrintf("DSEG -- Sent %d Zeronode invs to peer %d\n", nInvCount, pfrom->id);
            return;
        }
        // smth weird happen - someone asked us for vin we have no idea about?
        LogPrint("zeronode", "DSEG -- No invs sent to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::MNVERIFY) { // Zeronode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CZeronodeVerification mnv;
        vRecv >> mnv;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some zeronode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some zeronode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

// Verification of zeronodes via unique direct requests.

void CZeronodeMan::DoFullVerificationStep()
{
    if(activeZeronode.vin == CTxIn()) return;
    if(!zeronodeSync.IsSynced()) return;

    std::vector<std::pair<int, CZeronode> > vecZeronodeRanks = GetZeronodeRanks(pCurrentBlockIndex->nHeight - 1, MIN_POSE_PROTO_VERSION);

    // Need LOCK2 here to ensure consistent locking order because the SendVerifyRequest call below locks cs_main
    // through GetHeight() signal in ConnectNode
    LOCK2(cs_main, cs);

    int nCount = 0;

    int nMyRank = -1;
    int nRanksTotal = (int)vecZeronodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    std::vector<std::pair<int, CZeronode> >::iterator it = vecZeronodeRanks.begin();
    while(it != vecZeronodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint("zeronode", "CZeronodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.vin == activeZeronode.vin) {
            nMyRank = it->first;
            LogPrint("zeronode", "CZeronodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d zeronodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this zeronode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to MAX_POSE_CONNECTIONS zeronodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecZeronodeRanks.size()) return;

    std::vector<CZeronode*> vSortedByAddr;
    BOOST_FOREACH(CZeronode& mn, vZeronodes) {
        vSortedByAddr.push_back(&mn);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecZeronodeRanks.begin() + nOffset;
    while(it != vecZeronodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("zeronode", "CZeronodeMan::DoFullVerificationStep -- Already %s%s%s zeronode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.vin.prevout.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecZeronodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint("zeronode", "CZeronodeMan::DoFullVerificationStep -- Verifying zeronode %s rank %d/%d address %s\n",
                    it->second.vin.prevout.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if(SendVerifyRequest(CAddress(it->second.addr, NODE_NETWORK), vSortedByAddr)) {
            nCount++;
            if(nCount >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecZeronodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    LogPrint("zeronode", "CZeronodeMan::DoFullVerificationStep -- Sent verification requests to %d zeronodes\n", nCount);
}

// This function tries to find zeronodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CZeronodeMan::CheckSameAddr()
{
    if(!zeronodeSync.IsSynced() || vZeronodes.empty()) return;

    std::vector<CZeronode*> vBan;
    std::vector<CZeronode*> vSortedByAddr;

    {
        LOCK(cs);

        CZeronode* pprevZeronode = NULL;
        CZeronode* pverifiedZeronode = NULL;

        BOOST_FOREACH(CZeronode& mn, vZeronodes) {
            vSortedByAddr.push_back(&mn);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        BOOST_FOREACH(CZeronode* pmn, vSortedByAddr) {
            // check only (pre)enabled zeronodes
            if(!pmn->IsEnabled() && !pmn->IsPreEnabled()) continue;
            // initial step
            if(!pprevZeronode) {
                pprevZeronode = pmn;
                pverifiedZeronode = pmn->IsPoSeVerified() ? pmn : NULL;
                continue;
            }
            // second+ step
            if(pmn->addr == pprevZeronode->addr) {
                if(pverifiedZeronode) {
                    // another zeronode with the same ip is verified, ban this one
                    vBan.push_back(pmn);
                } else if(pmn->IsPoSeVerified()) {
                    // this zeronode with the same ip is verified, ban previous one
                    vBan.push_back(pprevZeronode);
                    // and keep a reference to be able to ban following zeronodes with the same ip
                    pverifiedZeronode = pmn;
                }
            } else {
                pverifiedZeronode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevZeronode = pmn;
        }
    }

    // ban duplicates
    BOOST_FOREACH(CZeronode* pmn, vBan) {
        LogPrintf("CZeronodeMan::CheckSameAddr -- increasing PoSe ban score for zeronode %s\n", pmn->vin.prevout.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CZeronodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<CZeronode*>& vSortedByAddr)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("zeronode", "CZeronodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    CNode* pnode = ConnectNode(addr, NULL, false, true);
    if(pnode == NULL) {
        LogPrintf("CZeronodeMan::SendVerifyRequest -- can't connect to node to verify it, addr=%s\n", addr.ToString());
        return false;
    }

    netfulfilledman.AddFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
    // use random nonce, store it and require node to reply with correct one later
    CZeronodeVerification mnv(addr, GetRandInt(999999), pCurrentBlockIndex->nHeight - 1);
    mWeAskedForVerification[addr] = mnv;
    LogPrintf("CZeronodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);

    return true;
}

void CZeronodeMan::SendVerifyReply(CNode* pnode, CZeronodeVerification& mnv)
{
    // only zeronodes can sign this, why would someone ask regular node?
    if(!fZNode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
//        // peer should not ask us that often
        LogPrintf("ZeronodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("ZeronodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strMessage = strprintf("%s%d%s", activeZeronode.service.ToString(), mnv.nonce, blockHash.ToString());

    if(!darkSendSigner.SignMessage(strMessage, mnv.vchSig1, activeZeronode.keyZeronode)) {
        LogPrintf("ZeronodeMan::SendVerifyReply -- SignMessage() failed\n");
        return;
    }

    std::string strError;

    if(!darkSendSigner.VerifyMessage(activeZeronode.pubKeyZeronode, mnv.vchSig1, strMessage, strError)) {
        LogPrintf("ZeronodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
        return;
    }

    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CZeronodeMan::ProcessVerifyReply(CNode* pnode, CZeronodeVerification& mnv)
{
    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintf("CZeronodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CZeronodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CZeronodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("ZeronodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

//    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintf("CZeronodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CZeronode* prealZeronode = NULL;
        std::vector<CZeronode*> vpZeronodesToBan;
        std::vector<CZeronode>::iterator it = vZeronodes.begin();
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(), mnv.nonce, blockHash.ToString());
        while(it != vZeronodes.end()) {
            if(CAddress(it->addr, NODE_NETWORK) == pnode->addr) {
                if(darkSendSigner.VerifyMessage(it->pubKeyZeronode, mnv.vchSig1, strMessage1, strError)) {
                    // found it!
                    prealZeronode = &(*it);
                    if(!it->IsPoSeVerified()) {
                        it->DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated zeronode
                    if(activeZeronode.vin == CTxIn()) continue;
                    // update ...
                    mnv.addr = it->addr;
                    mnv.vin1 = it->vin;
                    mnv.vin2 = activeZeronode.vin;
                    std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                            mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());
                    // ... and sign it
                    if(!darkSendSigner.SignMessage(strMessage2, mnv.vchSig2, activeZeronode.keyZeronode)) {
                        LogPrintf("ZeronodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                        return;
                    }

                    std::string strError;

                    if(!darkSendSigner.VerifyMessage(activeZeronode.pubKeyZeronode, mnv.vchSig2, strMessage2, strError)) {
                        LogPrintf("ZeronodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                        return;
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mnv.Relay();

                } else {
                    vpZeronodesToBan.push_back(&(*it));
                }
            }
            ++it;
        }
        // no real zeronode found?...
        if(!prealZeronode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CZeronodeMan::ProcessVerifyReply -- ERROR: no real zeronode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CZeronodeMan::ProcessVerifyReply -- verified real zeronode %s for addr %s\n",
                    prealZeronode->vin.prevout.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        BOOST_FOREACH(CZeronode* pmn, vpZeronodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrint("zeronode", "CZeronodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealZeronode->vin.prevout.ToStringShort(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        LogPrintf("CZeronodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake zeronodes, addr %s\n",
                    (int)vpZeronodesToBan.size(), pnode->addr.ToString());
    }
}

void CZeronodeMan::ProcessVerifyBroadcast(CNode* pnode, const CZeronodeVerification& mnv)
{
    std::string strError;

    if(mapSeenZeronodeVerification.find(mnv.GetHash()) != mapSeenZeronodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenZeronodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
        LogPrint("zeronode", "ZeronodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    pCurrentBlockIndex->nHeight, mnv.nBlockHeight, pnode->id);
        return;
    }

    if(mnv.vin1.prevout == mnv.vin2.prevout) {
        LogPrint("zeronode", "ZeronodeMan::ProcessVerifyBroadcast -- ERROR: same vins %s, peer=%d\n",
                    mnv.vin1.prevout.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("ZeronodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    int nRank = GetZeronodeRank(mnv.vin2, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION);

    if (nRank == -1) {
        LogPrint("zeronode", "CZeronodeMan::ProcessVerifyBroadcast -- Can't calculate rank for zeronode %s\n",
                    mnv.vin2.prevout.ToStringShort());
        return;
    }

    if(nRank > MAX_POSE_RANK) {
        LogPrint("zeronode", "CZeronodeMan::ProcessVerifyBroadcast -- Mastrernode %s is not in top %d, current rank %d, peer=%d\n",
                    mnv.vin2.prevout.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString());
        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());

        CZeronode* pmn1 = Find(mnv.vin1);
        if(!pmn1) {
            LogPrintf("CZeronodeMan::ProcessVerifyBroadcast -- can't find zeronode1 %s\n", mnv.vin1.prevout.ToStringShort());
            return;
        }

        CZeronode* pmn2 = Find(mnv.vin2);
        if(!pmn2) {
            LogPrintf("CZeronodeMan::ProcessVerifyBroadcast -- can't find zeronode2 %s\n", mnv.vin2.prevout.ToStringShort());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CZeronodeMan::ProcessVerifyBroadcast -- addr %s do not match %s\n", mnv.addr.ToString(), pnode->addr.ToString());
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn1->pubKeyZeronode, mnv.vchSig1, strMessage1, strError)) {
            LogPrintf("ZeronodeMan::ProcessVerifyBroadcast -- VerifyMessage() for zeronode1 failed, error: %s\n", strError);
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn2->pubKeyZeronode, mnv.vchSig2, strMessage2, strError)) {
            LogPrintf("ZeronodeMan::ProcessVerifyBroadcast -- VerifyMessage() for zeronode2 failed, error: %s\n", strError);
            return;
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CZeronodeMan::ProcessVerifyBroadcast -- verified zeronode %s for addr %s\n",
                    pmn1->vin.prevout.ToStringShort(), pnode->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        BOOST_FOREACH(CZeronode& mn, vZeronodes) {
            if(mn.addr != mnv.addr || mn.vin.prevout == mnv.vin1.prevout) continue;
            mn.IncreasePoSeBanScore();
            nCount++;
            LogPrint("zeronode", "CZeronodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mn.vin.prevout.ToStringShort(), mn.addr.ToString(), mn.nPoSeBanScore);
        }
        LogPrintf("CZeronodeMan::ProcessVerifyBroadcast -- PoSe score incresed for %d fake zeronodes, addr %s\n",
                    nCount, pnode->addr.ToString());
    }
}

std::string CZeronodeMan::ToString() const
{
    std::ostringstream info;

    info << "Zeronodes: " << (int)vZeronodes.size() <<
            ", peers who asked us for Zeronode list: " << (int)mAskedUsForZeronodeList.size() <<
            ", peers we asked for Zeronode list: " << (int)mWeAskedForZeronodeList.size() <<
            ", entries in Zeronode list we asked for: " << (int)mWeAskedForZeronodeListEntry.size() <<
            ", zeronode index size: " << indexZeronodes.GetSize() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

void CZeronodeMan::UpdateZeronodeList(CZeronodeBroadcast mnb)
{
    try {
        LogPrintf("CZeronodeMan::UpdateZeronodeList\n");
        LOCK2(cs_main, cs);
        mapSeenZeronodePing.insert(std::make_pair(mnb.lastPing.GetHash(), mnb.lastPing));
        mapSeenZeronodeBroadcast.insert(std::make_pair(mnb.GetHash(), std::make_pair(GetTime(), mnb)));

        LogPrintf("CZeronodeMan::UpdateZeronodeList -- zeronode=%s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());

        CZeronode *pmn = Find(mnb.vin);
        if (pmn == NULL) {
            CZeronode mn(mnb);
            if (Add(mn)) {
                zeronodeSync.AddedZeronodeList();
            }
        } else {
            CZeronodeBroadcast mnbOld = mapSeenZeronodeBroadcast[CZeronodeBroadcast(*pmn).GetHash()].second;
            if (pmn->UpdateFromNewBroadcast(mnb)) {
                zeronodeSync.AddedZeronodeList();
                mapSeenZeronodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } catch (const std::exception &e) {
        PrintExceptionContinue(&e, "UpdateZeronodeList");
    }
}

bool CZeronodeMan::CheckMnbAndUpdateZeronodeList(CNode* pfrom, CZeronodeBroadcast mnb, int& nDos)
{
    // Need LOCK2 here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- zeronode=%s\n", mnb.vin.prevout.ToStringShort());

        uint256 hash = mnb.GetHash();
        if (mapSeenZeronodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- zeronode=%s seen\n", mnb.vin.prevout.ToStringShort());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if (GetTime() - mapSeenZeronodeBroadcast[hash].first > ZERONODE_NEW_START_REQUIRED_SECONDS - ZERONODE_MIN_MNP_SECONDS * 2) {
                LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- zeronode=%s seen update\n", mnb.vin.prevout.ToStringShort());
                mapSeenZeronodeBroadcast[hash].first = GetTime();
                zeronodeSync.AddedZeronodeList();
            }
            // did we ask this node for it?
            if (pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- mnb=%s seen request\n", hash.ToString());
                if (mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if (mnb.lastPing.sigTime > mapSeenZeronodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CZeronode mnTemp = CZeronode(mnb);
                        mnTemp.Check();
                        LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetTime() - mnb.lastPing.sigTime) / 60, mnTemp.GetStateString());
                        if (mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- zeronode=%s seen good\n", mnb.vin.prevout.ToStringShort());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenZeronodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- zeronode=%s new\n", mnb.vin.prevout.ToStringShort());

        if (!mnb.SimpleCheck(nDos)) {
            LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- SimpleCheck() failed, zeronode=%s\n", mnb.vin.prevout.ToStringShort());
            return false;
        }

        // search Zeronode list
        CZeronode *pmn = Find(mnb.vin);
        if (pmn) {
            CZeronodeBroadcast mnbOld = mapSeenZeronodeBroadcast[CZeronodeBroadcast(*pmn).GetHash()].second;
            if (!mnb.Update(pmn, nDos)) {
                LogPrint("zeronode", "CZeronodeMan::CheckMnbAndUpdateZeronodeList -- Update() failed, zeronode=%s\n", mnb.vin.prevout.ToStringShort());
                return false;
            }
            if (hash != mnbOld.GetHash()) {
                mapSeenZeronodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } // end of LOCK(cs);

    if(mnb.CheckOutpoint(nDos)) {
        Add(mnb);
        zeronodeSync.AddedZeronodeList();
        // if it matches our Zeronode privkey...
        if(fZNode && mnb.pubKeyZeronode == activeZeronode.pubKeyZeronode) {
            mnb.nPoSeBanScore = -ZERONODE_POSE_BAN_MAX_SCORE;
            if(mnb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CZeronodeMan::CheckMnbAndUpdateZeronodeList -- Got NEW Zeronode entry: zeronode=%s  sigTime=%lld  addr=%s\n",
                            mnb.vin.prevout.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                activeZeronode.ManageState();
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CZeronodeMan::CheckMnbAndUpdateZeronodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        mnb.RelayZNode();
    } else {
        LogPrintf("CZeronodeMan::CheckMnbAndUpdateZeronodeList -- Rejected Zeronode entry: %s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());
        return false;
    }

    return true;
}

void CZeronodeMan::UpdateLastPaid()
{
    LOCK(cs);
    if(fLiteMode) return;
    if(!pCurrentBlockIndex) {
        // LogPrintf("CZeronodeMan::UpdateLastPaid, pCurrentBlockIndex=NULL\n");
        return;
    }

    static bool IsFirstRun = true;
    // Do full scan on first run or if we are not a zeronode
    // (MNs should update this info on every block, so limited scan should be enough for them)
    int nMaxBlocksToScanBack = (IsFirstRun || !fZNode) ? mnpayments.GetStorageLimit() : LAST_PAID_SCAN_BLOCKS;

    LogPrint("mnpayments", "CZeronodeMan::UpdateLastPaid -- nHeight=%d, nMaxBlocksToScanBack=%d, IsFirstRun=%s\n",
                             pCurrentBlockIndex->nHeight, nMaxBlocksToScanBack, IsFirstRun ? "true" : "false");

    BOOST_FOREACH(CZeronode& mn, vZeronodes) {
        mn.UpdateLastPaid(pCurrentBlockIndex, nMaxBlocksToScanBack);
    }

    // every time is like the first time if winners list is not synced
    IsFirstRun = !zeronodeSync.IsWinnersListSynced();
}

void CZeronodeMan::CheckAndRebuildZeronodeIndex()
{
    LOCK(cs);

    if(GetTime() - nLastIndexRebuildTime < MIN_INDEX_REBUILD_TIME) {
        return;
    }

    if(indexZeronodes.GetSize() <= MAX_EXPECTED_INDEX_SIZE) {
        return;
    }

    if(indexZeronodes.GetSize() <= int(vZeronodes.size())) {
        return;
    }

    indexZeronodesOld = indexZeronodes;
    indexZeronodes.Clear();
    for(size_t i = 0; i < vZeronodes.size(); ++i) {
        indexZeronodes.AddZeronodeVIN(vZeronodes[i].vin);
    }

    fIndexRebuilt = true;
    nLastIndexRebuildTime = GetTime();
}

void CZeronodeMan::UpdateWatchdogVoteTime(const CTxIn& vin)
{
    LOCK(cs);
    CZeronode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->UpdateWatchdogVoteTime();
    nLastWatchdogVoteTime = GetTime();
}

bool CZeronodeMan::IsWatchdogActive()
{
    LOCK(cs);
    // Check if any zeronodes have voted recently, otherwise return false
    return (GetTime() - nLastWatchdogVoteTime) <= ZERONODE_WATCHDOG_MAX_SECONDS;
}

void CZeronodeMan::CheckZeronode(const CTxIn& vin, bool fForce)
{
    LOCK(cs);
    CZeronode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

void CZeronodeMan::CheckZeronode(const CPubKey& pubKeyZeronode, bool fForce)
{
    LOCK(cs);
    CZeronode* pMN = Find(pubKeyZeronode);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

int CZeronodeMan::GetZeronodeState(const CTxIn& vin)
{
    LOCK(cs);
    CZeronode* pMN = Find(vin);
    if(!pMN)  {
        return CZeronode::ZERONODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

int CZeronodeMan::GetZeronodeState(const CPubKey& pubKeyZeronode)
{
    LOCK(cs);
    CZeronode* pMN = Find(pubKeyZeronode);
    if(!pMN)  {
        return CZeronode::ZERONODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

bool CZeronodeMan::IsZeronodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CZeronode* pMN = Find(vin);
    if(!pMN) {
        return false;
    }
    return pMN->IsPingedWithin(nSeconds, nTimeToCheckAt);
}

void CZeronodeMan::SetZeronodeLastPing(const CTxIn& vin, const CZeronodePing& mnp)
{
    LOCK(cs);
    CZeronode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->lastPing = mnp;
    mapSeenZeronodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CZeronodeBroadcast mnb(*pMN);
    uint256 hash = mnb.GetHash();
    if(mapSeenZeronodeBroadcast.count(hash)) {
        mapSeenZeronodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CZeronodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("zeronode", "CZeronodeMan::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);

    CheckSameAddr();

    if(fZNode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid();
    }
}

void CZeronodeMan::NotifyZeronodeUpdates()
{
    // Avoid double locking
    bool fZeronodesAddedLocal = false;
    bool fZeronodesRemovedLocal = false;
    {
        LOCK(cs);
        fZeronodesAddedLocal = fZeronodesAdded;
        fZeronodesRemovedLocal = fZeronodesRemoved;
    }

    if(fZeronodesAddedLocal) {
//        governance.CheckZeronodeOrphanObjects();
//        governance.CheckZeronodeOrphanVotes();
    }
    if(fZeronodesRemovedLocal) {
//        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fZeronodesAdded = false;
    fZeronodesRemoved = false;
}
