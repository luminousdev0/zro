// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ZERONODE_PAYMENTS_H
#define ZERONODE_PAYMENTS_H

#include "util.h"
#include "core_io.h"
#include "key.h"
#include "main.h"
#include "zeronode.h"
#include "utilstrencodings.h"

class CZeronodePayments;
class CZeronodePaymentVote;
class CZeronodeBlockPayees;

static const int MNPAYMENTS_SIGNATURES_REQUIRED         = 6;
static const int MNPAYMENTS_SIGNATURES_TOTAL            = 10;

//! minimum peer version that can receive and send zeronode payment messages,
//  vote for zeronode and be elected as a payment winner
// V1 - Last protocol version before update
// V2 - Newest protocol version
static const int MIN_ZERONODE_PAYMENT_PROTO_VERSION_1 = 90023;
static const int MIN_ZERONODE_PAYMENT_PROTO_VERSION_2 = 90024;

extern CCriticalSection cs_vecPayees;
extern CCriticalSection cs_mapZeronodeBlocks;
extern CCriticalSection cs_mapZeronodePayeeVotes;

extern CZeronodePayments mnpayments;

/// TODO: all 4 functions do not belong here really, they should be refactored/moved somewhere (main.cpp ?)
bool IsBlockValueValid(const CBlock& block, int nBlockHeight, CAmount blockReward, std::string &strErrorRet);
bool IsBlockPayeeValid(const CTransaction& txNew, int nBlockHeight, CAmount blockReward);
void FillBlockPayments(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutZeronodeRet, std::vector<CTxOut>& voutSuperblockRet);
std::string GetRequiredPaymentsString(int nBlockHeight);

class CZeronodePayee
{
private:
    CScript scriptPubKey;
    std::vector<uint256> vecVoteHashes;

public:
    CZeronodePayee() :
        scriptPubKey(),
        vecVoteHashes()
        {}

    CZeronodePayee(CScript payee, uint256 hashIn) :
        scriptPubKey(payee),
        vecVoteHashes()
    {
        vecVoteHashes.push_back(hashIn);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(*(CScriptBase*)(&scriptPubKey));
        READWRITE(vecVoteHashes);
    }

    CScript GetPayee() { return scriptPubKey; }

    void AddVoteHash(uint256 hashIn) { vecVoteHashes.push_back(hashIn); }
    std::vector<uint256> GetVoteHashes() { return vecVoteHashes; }
    int GetVoteCount() { return vecVoteHashes.size(); }
    std::string ToString() const;
};

// Keep track of votes for payees from zeronodes
class CZeronodeBlockPayees
{
public:
    int nBlockHeight;
    std::vector<CZeronodePayee> vecPayees;

    CZeronodeBlockPayees() :
        nBlockHeight(0),
        vecPayees()
        {}
    CZeronodeBlockPayees(int nBlockHeightIn) :
        nBlockHeight(nBlockHeightIn),
        vecPayees()
        {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(nBlockHeight);
        READWRITE(vecPayees);
    }

    void AddPayee(const CZeronodePaymentVote& vote);
    bool GetBestPayee(CScript& payeeRet);
    bool HasPayeeWithVotes(CScript payeeIn, int nVotesReq);

    bool IsTransactionValid(const CTransaction& txNew);

    std::string GetRequiredPaymentsString();
};

// vote for the winning payment
class CZeronodePaymentVote
{
public:
    CTxIn vinZeronode;

    int nBlockHeight;
    CScript payee;
    std::vector<unsigned char> vchSig;

    CZeronodePaymentVote() :
        vinZeronode(),
        nBlockHeight(0),
        payee(),
        vchSig()
        {}

    CZeronodePaymentVote(CTxIn vinZeronode, int nBlockHeight, CScript payee) :
        vinZeronode(vinZeronode),
        nBlockHeight(nBlockHeight),
        payee(payee),
        vchSig()
        {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(vinZeronode);
        READWRITE(nBlockHeight);
        READWRITE(*(CScriptBase*)(&payee));
        READWRITE(vchSig);
    }

    uint256 GetHash() const {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << *(CScriptBase*)(&payee);
        ss << nBlockHeight;
        ss << vinZeronode.prevout;
        return ss.GetHash();
    }

    bool Sign();
    bool CheckSignature(const CPubKey& pubKeyZeronode, int nValidationHeight, int &nDos);

    bool IsValid(CNode* pnode, int nValidationHeight, std::string& strError);
    void Relay();

    bool IsVerified() { return !vchSig.empty(); }
    void MarkAsNotVerified() { vchSig.clear(); }

    std::string ToString() const;
};

//
// Zeronode Payments Class
// Keeps track of who should get paid for which blocks
//

class CZeronodePayments
{
private:
    // zeronode count times nStorageCoeff payments blocks should be stored ...
    const float nStorageCoeff;
    // ... but at least nMinBlocksToStore (payments blocks)
    const int nMinBlocksToStore;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

public:
    std::map<uint256, CZeronodePaymentVote> mapZeronodePaymentVotes;
    std::map<int, CZeronodeBlockPayees> mapZeronodeBlocks;
    std::map<COutPoint, int> mapZeronodesLastVote;

    CZeronodePayments() : nStorageCoeff(1.25), nMinBlocksToStore(5000) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(mapZeronodePaymentVotes);
        READWRITE(mapZeronodeBlocks);
    }

    void Clear();

    bool AddPaymentVote(const CZeronodePaymentVote& vote);
    bool HasVerifiedPaymentVote(uint256 hashIn);
    bool ProcessBlock(int nBlockHeight);

    void Sync(CNode* node);
    void RequestLowDataPaymentBlocks(CNode* pnode);
    void CheckAndRemove();

    bool GetBlockPayee(int nBlockHeight, CScript& payee);
    bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight);
    bool IsScheduled(CZeronode& mn, int nNotBlockHeight);

    bool CanVote(COutPoint outZeronode, int nBlockHeight);

    int GetMinZeronodePaymentsProto();
    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    std::string GetRequiredPaymentsString(int nBlockHeight);
    void FillBlockPayee(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutZeronodeRet);
    std::string ToString() const;

    int GetBlockCount() { return mapZeronodeBlocks.size(); }
    int GetVoteCount() { return mapZeronodePaymentVotes.size(); }

    bool IsEnoughData();
    int GetStorageLimit();

    void UpdatedBlockTip(const CBlockIndex *pindex);
};

#endif
