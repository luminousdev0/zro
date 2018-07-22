// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ZERONODE_H
#define ZERONODE_H

#include "key.h"
#include "main.h"
#include "net.h"
#include "spork.h"
#include "timedata.h"
#include "utiltime.h"

class CZeronode;
class CZeronodeBroadcast;
class CZeronodePing;

static const int ZERONODE_CHECK_SECONDS               =   5;
static const int ZERONODE_MIN_MNB_SECONDS             =   5 * 60; //BROADCAST_TIME
static const int ZERONODE_MIN_MNP_SECONDS             =  10 * 60; //PRE_ENABLE_TIME
static const int ZERONODE_EXPIRATION_SECONDS          =  65 * 60;
static const int ZERONODE_WATCHDOG_MAX_SECONDS        = 120 * 60;
static const int ZERONODE_NEW_START_REQUIRED_SECONDS  = 180 * 60;
static const int ZERONODE_COIN_REQUIRED  = 1000;

static const int ZERONODE_POSE_BAN_MAX_SCORE          = 5;
//
// The Zeronode Ping Class : Contains a different serialize method for sending pings from zeronodes throughout the network
//

class CZeronodePing
{
public:
    CTxIn vin;
    uint256 blockHash;
    int64_t sigTime; //mnb message times
    std::vector<unsigned char> vchSig;
    //removed stop

    CZeronodePing() :
        vin(),
        blockHash(),
        sigTime(0),
        vchSig()
        {}

    CZeronodePing(CTxIn& vinNew);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(vin);
        READWRITE(blockHash);
        READWRITE(sigTime);
        READWRITE(vchSig);
    }

    void swap(CZeronodePing& first, CZeronodePing& second) // nothrow
    {
        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.vin, second.vin);
        swap(first.blockHash, second.blockHash);
        swap(first.sigTime, second.sigTime);
        swap(first.vchSig, second.vchSig);
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << vin;
        ss << sigTime;
        return ss.GetHash();
    }

    bool IsExpired() { return GetTime() - sigTime > ZERONODE_NEW_START_REQUIRED_SECONDS; }

    bool Sign(CKey& keyZeronode, CPubKey& pubKeyZeronode);
    bool CheckSignature(CPubKey& pubKeyZeronode, int &nDos);
    bool SimpleCheck(int& nDos);
    bool CheckAndUpdate(CZeronode* pmn, bool fFromNewBroadcast, int& nDos);
    void Relay();

    CZeronodePing& operator=(CZeronodePing from)
    {
        swap(*this, from);
        return *this;
    }
    friend bool operator==(const CZeronodePing& a, const CZeronodePing& b)
    {
        return a.vin == b.vin && a.blockHash == b.blockHash;
    }
    friend bool operator!=(const CZeronodePing& a, const CZeronodePing& b)
    {
        return !(a == b);
    }

};

struct zeronode_info_t
{
    zeronode_info_t()
        : vin(),
          addr(),
          pubKeyCollateralAddress(),
          pubKeyZeronode(),
          sigTime(0),
          nLastDsq(0),
          nTimeLastChecked(0),
          nTimeLastPaid(0),
          nTimeLastWatchdogVote(0),
          nTimeLastPing(0),
          nActiveState(0),
          nProtocolVersion(0),
          fInfoValid(false)
        {}

    CTxIn vin;
    CService addr;
    CPubKey pubKeyCollateralAddress;
    CPubKey pubKeyZeronode;
    int64_t sigTime; //mnb message time
    int64_t nLastDsq; //the dsq count from the last dsq broadcast of this node
    int64_t nTimeLastChecked;
    int64_t nTimeLastPaid;
    int64_t nTimeLastWatchdogVote;
    int64_t nTimeLastPing;
    int nActiveState;
    int nProtocolVersion;
    bool fInfoValid;
};

//
// The Zeronode Class. For managing the Darksend process. It contains the input of the 1000DRK, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CZeronode
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

public:
    enum state {
        ZERONODE_PRE_ENABLED,
        ZERONODE_ENABLED,
        ZERONODE_EXPIRED,
        ZERONODE_OUTPOINT_SPENT,
        ZERONODE_UPDATE_REQUIRED,
        ZERONODE_WATCHDOG_EXPIRED,
        ZERONODE_NEW_START_REQUIRED,
        ZERONODE_POSE_BAN
    };

    CTxIn vin;
    CService addr;
    CPubKey pubKeyCollateralAddress;
    CPubKey pubKeyZeronode;
    CZeronodePing lastPing;
    std::vector<unsigned char> vchSig;
    int64_t sigTime; //mnb message time
    int64_t nLastDsq; //the dsq count from the last dsq broadcast of this node
    int64_t nTimeLastChecked;
    int64_t nTimeLastPaid;
    int64_t nTimeLastWatchdogVote;
    int nActiveState;
    int nCacheCollateralBlock;
    int nBlockLastPaid;
    int nProtocolVersion;
    int nPoSeBanScore;
    int nPoSeBanHeight;
    bool fAllowMixingTx;
    bool fUnitTest;

    // KEEP TRACK OF GOVERNANCE ITEMS EACH ZERONODE HAS VOTE UPON FOR RECALCULATION
    std::map<uint256, int> mapGovernanceObjectsVotedOn;

    CZeronode();
    CZeronode(const CZeronode& other);
    CZeronode(const CZeronodeBroadcast& mnb);
    CZeronode(CService addrNew, CTxIn vinNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyZeronodeNew, int nProtocolVersionIn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        LOCK(cs);
        READWRITE(vin);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyZeronode);
        READWRITE(lastPing);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(nLastDsq);
        READWRITE(nTimeLastChecked);
        READWRITE(nTimeLastPaid);
        READWRITE(nTimeLastWatchdogVote);
        READWRITE(nActiveState);
        READWRITE(nCacheCollateralBlock);
        READWRITE(nBlockLastPaid);
        READWRITE(nProtocolVersion);
        READWRITE(nPoSeBanScore);
        READWRITE(nPoSeBanHeight);
        READWRITE(fAllowMixingTx);
        READWRITE(fUnitTest);
        READWRITE(mapGovernanceObjectsVotedOn);
    }

    void swap(CZeronode& first, CZeronode& second) // nothrow
    {
        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.vin, second.vin);
        swap(first.addr, second.addr);
        swap(first.pubKeyCollateralAddress, second.pubKeyCollateralAddress);
        swap(first.pubKeyZeronode, second.pubKeyZeronode);
        swap(first.lastPing, second.lastPing);
        swap(first.vchSig, second.vchSig);
        swap(first.sigTime, second.sigTime);
        swap(first.nLastDsq, second.nLastDsq);
        swap(first.nTimeLastChecked, second.nTimeLastChecked);
        swap(first.nTimeLastPaid, second.nTimeLastPaid);
        swap(first.nTimeLastWatchdogVote, second.nTimeLastWatchdogVote);
        swap(first.nActiveState, second.nActiveState);
        swap(first.nCacheCollateralBlock, second.nCacheCollateralBlock);
        swap(first.nBlockLastPaid, second.nBlockLastPaid);
        swap(first.nProtocolVersion, second.nProtocolVersion);
        swap(first.nPoSeBanScore, second.nPoSeBanScore);
        swap(first.nPoSeBanHeight, second.nPoSeBanHeight);
        swap(first.fAllowMixingTx, second.fAllowMixingTx);
        swap(first.fUnitTest, second.fUnitTest);
        swap(first.mapGovernanceObjectsVotedOn, second.mapGovernanceObjectsVotedOn);
    }

    // CALCULATE A RANK AGAINST OF GIVEN BLOCK
    arith_uint256 CalculateScore(const uint256& blockHash);

    bool UpdateFromNewBroadcast(CZeronodeBroadcast& mnb);

    void Check(bool fForce = false);

    bool IsBroadcastedWithin(int nSeconds) { return GetAdjustedTime() - sigTime < nSeconds; }

    bool IsPingedWithin(int nSeconds, int64_t nTimeToCheckAt = -1)
    {
        if(lastPing == CZeronodePing()) return false;

        if(nTimeToCheckAt == -1) {
            nTimeToCheckAt = GetAdjustedTime();
        }
        return nTimeToCheckAt - lastPing.sigTime < nSeconds;
    }

    bool IsEnabled() { return nActiveState == ZERONODE_ENABLED; }
    bool IsPreEnabled() { return nActiveState == ZERONODE_PRE_ENABLED; }
    bool IsPoSeBanned() { return nActiveState == ZERONODE_POSE_BAN; }
    // NOTE: this one relies on nPoSeBanScore, not on nActiveState as everything else here
    bool IsPoSeVerified() { return nPoSeBanScore <= -ZERONODE_POSE_BAN_MAX_SCORE; }
    bool IsExpired() { return nActiveState == ZERONODE_EXPIRED; }
    bool IsOutpointSpent() { return nActiveState == ZERONODE_OUTPOINT_SPENT; }
    bool IsUpdateRequired() { return nActiveState == ZERONODE_UPDATE_REQUIRED; }
    bool IsWatchdogExpired() { return nActiveState == ZERONODE_WATCHDOG_EXPIRED; }
    bool IsNewStartRequired() { return nActiveState == ZERONODE_NEW_START_REQUIRED; }

    static bool IsValidStateForAutoStart(int nActiveStateIn)
    {
        return  nActiveStateIn == ZERONODE_ENABLED ||
                nActiveStateIn == ZERONODE_PRE_ENABLED ||
                nActiveStateIn == ZERONODE_EXPIRED ||
                nActiveStateIn == ZERONODE_WATCHDOG_EXPIRED;
    }

    bool IsValidForPayment();

    bool IsValidNetAddr();
    static bool IsValidNetAddr(CService addrIn);

    void IncreasePoSeBanScore() { if(nPoSeBanScore < ZERONODE_POSE_BAN_MAX_SCORE) nPoSeBanScore++; }
    void DecreasePoSeBanScore() { if(nPoSeBanScore > -ZERONODE_POSE_BAN_MAX_SCORE) nPoSeBanScore--; }

    zeronode_info_t GetInfo();

    static std::string StateToString(int nStateIn);
    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string ToString() const;

    int GetCollateralAge();

    int GetLastPaidTime() { return nTimeLastPaid; }
    int GetLastPaidBlock() { return nBlockLastPaid; }
    void UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack);

    // KEEP TRACK OF EACH GOVERNANCE ITEM INCASE THIS NODE GOES OFFLINE, SO WE CAN RECALC THEIR STATUS
    void AddGovernanceVote(uint256 nGovernanceObjectHash);
    // RECALCULATE CACHED STATUS FLAGS FOR ALL AFFECTED OBJECTS
    void FlagGovernanceItemsAsDirty();

    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void UpdateWatchdogVoteTime();

    CZeronode& operator=(CZeronode from)
    {
        swap(*this, from);
        return *this;
    }
    friend bool operator==(const CZeronode& a, const CZeronode& b)
    {
        return a.vin == b.vin;
    }
    friend bool operator!=(const CZeronode& a, const CZeronode& b)
    {
        return !(a.vin == b.vin);
    }

};


//
// The Zeronode Broadcast Class : Contains a different serialize method for sending zeronodes through the network
//

class CZeronodeBroadcast : public CZeronode
{
public:

    bool fRecovery;

    CZeronodeBroadcast() : CZeronode(), fRecovery(false) {}
    CZeronodeBroadcast(const CZeronode& mn) : CZeronode(mn), fRecovery(false) {}
    CZeronodeBroadcast(CService addrNew, CTxIn vinNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyZeronodeNew, int nProtocolVersionIn) :
        CZeronode(addrNew, vinNew, pubKeyCollateralAddressNew, pubKeyZeronodeNew, nProtocolVersionIn), fRecovery(false) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(vin);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyZeronode);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(nProtocolVersion);
        READWRITE(lastPing);
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << vin;
        ss << pubKeyCollateralAddress;
        ss << sigTime;
        return ss.GetHash();
    }

    /// Create Zeronode broadcast, needs to be relayed manually after that
    static bool Create(CTxIn vin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyZeronodeNew, CPubKey pubKeyZeronodeNew, std::string &strErrorRet, CZeronodeBroadcast &mnbRet);
    static bool Create(std::string strService, std::string strKey, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CZeronodeBroadcast &mnbRet, bool fOffline = false);

    bool SimpleCheck(int& nDos);
    bool Update(CZeronode* pmn, int& nDos);
    bool CheckOutpoint(int& nDos);

    bool Sign(CKey& keyCollateralAddress);
    bool CheckSignature(int& nDos);
    void RelayZNode();
};

class CZeronodeVerification
{
public:
    CTxIn vin1;
    CTxIn vin2;
    CService addr;
    int nonce;
    int nBlockHeight;
    std::vector<unsigned char> vchSig1;
    std::vector<unsigned char> vchSig2;

    CZeronodeVerification() :
        vin1(),
        vin2(),
        addr(),
        nonce(0),
        nBlockHeight(0),
        vchSig1(),
        vchSig2()
        {}

    CZeronodeVerification(CService addr, int nonce, int nBlockHeight) :
        vin1(),
        vin2(),
        addr(addr),
        nonce(nonce),
        nBlockHeight(nBlockHeight),
        vchSig1(),
        vchSig2()
        {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(vin1);
        READWRITE(vin2);
        READWRITE(addr);
        READWRITE(nonce);
        READWRITE(nBlockHeight);
        READWRITE(vchSig1);
        READWRITE(vchSig2);
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << vin1;
        ss << vin2;
        ss << addr;
        ss << nonce;
        ss << nBlockHeight;
        return ss.GetHash();
    }

    void Relay() const
    {
        CInv inv(MSG_ZERONODE_VERIFY, GetHash());
        RelayInv(inv);
    }
};

#endif
