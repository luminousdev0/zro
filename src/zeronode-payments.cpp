// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activezeronode.h"
#include "darksend.h"
#include "zeronode-payments.h"
#include "zeronode-sync.h"
#include "zeronodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

#include <boost/lexical_cast.hpp>

/** Object for who's going to get paid on which blocks */
CZeronodePayments mnpayments;

CCriticalSection cs_vecPayees;
CCriticalSection cs_mapZeronodeBlocks;
CCriticalSection cs_mapZeronodePaymentVotes;

/**
* IsBlockValueValid
*
*   Determine if coinbase outgoing created money is the correct value
*
*   Why is this needed?
*   - In Dash some blocks are superblocks, which output much higher amounts of coins
*   - Otherblocks are 10% lower in outgoing value, so in total, no extra coins are created
*   - When non-superblocks are detected, the normal schedule should be maintained
*/

bool IsBlockValueValid(const CBlock &block, int nBlockHeight, CAmount blockReward, std::string &strErrorRet) {
    strErrorRet = "";

    bool isBlockRewardValueMet = (block.vtx[0].GetValueOut() <= blockReward);
    if (fDebug) LogPrintf("block.vtx[0].GetValueOut() %lld <= blockReward %lld\n", block.vtx[0].GetValueOut(), blockReward);

    // we are still using budgets, but we have no data about them anymore,
    // all we know is predefined budget cycle and window

//    const Consensus::Params &consensusParams = Params().GetConsensus();
//
////    if (nBlockHeight < consensusParams.nSuperblockStartBlock) {
//        int nOffset = nBlockHeight % consensusParams.nBudgetPaymentsCycleBlocks;
//        if (nBlockHeight >= consensusParams.nBudgetPaymentsStartBlock &&
//            nOffset < consensusParams.nBudgetPaymentsWindowBlocks) {
//            // NOTE: make sure SPORK_13_OLD_SUPERBLOCK_FLAG is disabled when 12.1 starts to go live
//            if (zeronodeSync.IsSynced() && !sporkManager.IsSporkActive(SPORK_13_OLD_SUPERBLOCK_FLAG)) {
//                // no budget blocks should be accepted here, if SPORK_13_OLD_SUPERBLOCK_FLAG is disabled
//                LogPrint("gobject", "IsBlockValueValid -- Client synced but budget spork is disabled, checking block value against block reward\n");
//                if (!isBlockRewardValueMet) {
//                    strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, budgets are disabled",
//                                            nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//                }
//                return isBlockRewardValueMet;
//            }
//            LogPrint("gobject", "IsBlockValueValid -- WARNING: Skipping budget block value checks, accepting block\n");
//            // TODO: reprocess blocks to make sure they are legit?
//            return true;
//        }
//        // LogPrint("gobject", "IsBlockValueValid -- Block is not in budget cycle window, checking block value against block reward\n");
//        if (!isBlockRewardValueMet) {
//            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, block is not in budget cycle window",
//                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//        }
//        return isBlockRewardValueMet;
//    }

    // superblocks started

//    CAmount nSuperblockMaxValue =  blockReward + CSuperblock::GetPaymentsLimit(nBlockHeight);
//    bool isSuperblockMaxValueMet = (block.vtx[0].GetValueOut() <= nSuperblockMaxValue);
//    bool isSuperblockMaxValueMet = false;

//    LogPrint("gobject", "block.vtx[0].GetValueOut() %lld <= nSuperblockMaxValue %lld\n", block.vtx[0].GetValueOut(), nSuperblockMaxValue);

    if (!zeronodeSync.IsSynced()) {
        // not enough data but at least it must NOT exceed superblock max value
//        if(CSuperblock::IsValidBlockHeight(nBlockHeight)) {
//            if(fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, checking superblock max bounds only\n");
//            if(!isSuperblockMaxValueMet) {
//                strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded superblock max value",
//                                        nBlockHeight, block.vtx[0].GetValueOut(), nSuperblockMaxValue);
//            }
//            return isSuperblockMaxValueMet;
//        }
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, only regular blocks are allowed at this height",
                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
        }
        // it MUST be a regular block otherwise
        return isBlockRewardValueMet;
    }

    // we are synced, let's try to check as much data as we can

    if (sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED)) {
////        if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
////            if(CSuperblockManager::IsValid(block.vtx[0], nBlockHeight, blockReward)) {
////                LogPrint("gobject", "IsBlockValueValid -- Valid superblock at height %d: %s", nBlockHeight, block.vtx[0].ToString());
////                // all checks are done in CSuperblock::IsValid, nothing to do here
////                return true;
////            }
////
////            // triggered but invalid? that's weird
////            LogPrintf("IsBlockValueValid -- ERROR: Invalid superblock detected at height %d: %s", nBlockHeight, block.vtx[0].ToString());
////            // should NOT allow invalid superblocks, when superblocks are enabled
////            strErrorRet = strprintf("invalid superblock detected at height %d", nBlockHeight);
////            return false;
////        }
//        LogPrint("gobject", "IsBlockValueValid -- No triggered superblock detected at height %d\n", nBlockHeight);
//        if(!isBlockRewardValueMet) {
//            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, no triggered superblock detected",
//                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//        }
    } else {
//        // should NOT allow superblocks at all, when superblocks are disabled
        LogPrint("gobject", "IsBlockValueValid -- Superblocks are disabled, no superblocks allowed\n");
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, superblocks are disabled",
                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
        }
    }

    // it MUST be a regular block
    return isBlockRewardValueMet;
}

bool IsBlockPayeeValid(const CTransaction &txNew, int nBlockHeight, CAmount blockReward) {
    // we can only check zeronode payment /
    const Consensus::Params &consensusParams = Params().GetConsensus();

    if (nBlockHeight < consensusParams.nZeronodePaymentsStartBlock) {
        //there is no budget data to use to check anything, let's just accept the longest chain
        if (fDebug) LogPrintf("IsBlockPayeeValid -- zeronode isn't start\n");
        return true;
    }
    if (!zeronodeSync.IsSynced()) {
        //there is no budget data to use to check anything, let's just accept the longest chain
        if (fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, skipping block payee checks\n");
        return true;
    }

    //check for zeronode payee
    if (mnpayments.IsTransactionValid(txNew, nBlockHeight)) {
        LogPrint("mnpayments", "IsBlockPayeeValid -- Valid zeronode payment at height %d: %s", nBlockHeight, txNew.ToString());
        return true;
    } else {
        if(sporkManager.IsSporkActive(SPORK_8_ZERONODE_PAYMENT_ENFORCEMENT)){
            return false;
        } else {
            LogPrintf("ZNode payment enforcement is disabled, accepting block\n");
            return true;
        }
    }
}

void FillBlockPayments(CMutableTransaction &txNew, int nBlockHeight, CAmount zeronodePayment, CTxOut &txoutZeronodeRet, std::vector <CTxOut> &voutSuperblockRet) {
    // only create superblocks if spork is enabled AND if superblock is actually triggered
    // (height should be validated inside)
//    if(sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED) &&
//        CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
//            LogPrint("gobject", "FillBlockPayments -- triggered superblock creation at height %d\n", nBlockHeight);
//            CSuperblockManager::CreateSuperblock(txNew, nBlockHeight, voutSuperblockRet);
//            return;
//    }

    // FILL BLOCK PAYEE WITH ZERONODE PAYMENT OTHERWISE
    mnpayments.FillBlockPayee(txNew, nBlockHeight, zeronodePayment, txoutZeronodeRet);
    LogPrint("mnpayments", "FillBlockPayments -- nBlockHeight %d zeronodePayment %lld txoutZeronodeRet %s txNew %s",
             nBlockHeight, zeronodePayment, txoutZeronodeRet.ToString(), txNew.ToString());
}

std::string GetRequiredPaymentsString(int nBlockHeight) {
    // IF WE HAVE A ACTIVATED TRIGGER FOR THIS HEIGHT - IT IS A SUPERBLOCK, GET THE REQUIRED PAYEES
//    if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
//        return CSuperblockManager::GetRequiredPaymentsString(nBlockHeight);
//    }

    // OTHERWISE, PAY ZERONODE
    return mnpayments.GetRequiredPaymentsString(nBlockHeight);
}

void CZeronodePayments::Clear() {
    LOCK2(cs_mapZeronodeBlocks, cs_mapZeronodePaymentVotes);
    mapZeronodeBlocks.clear();
    mapZeronodePaymentVotes.clear();
}

bool CZeronodePayments::CanVote(COutPoint outZeronode, int nBlockHeight) {
    LOCK(cs_mapZeronodePaymentVotes);

    if (mapZeronodesLastVote.count(outZeronode) && mapZeronodesLastVote[outZeronode] == nBlockHeight) {
        return false;
    }

    //record this zeronode voted
    mapZeronodesLastVote[outZeronode] = nBlockHeight;
    return true;
}

std::string CZeronodePayee::ToString() const {
    CTxDestination address1;
    ExtractDestination(scriptPubKey, address1);
    CBitcoinAddress address2(address1);
    std::string str;
    str += "(address: ";
    str += address2.ToString();
    str += ")\n";
    return str;
}

/**
*   FillBlockPayee
*
*   Fill Zeronode ONLY payment block
*/

void CZeronodePayments::FillBlockPayee(CMutableTransaction &txNew, int nBlockHeight, CAmount zeronodePayment, CTxOut &txoutZeronodeRet) {
    // make sure it's not filled yet
    txoutZeronodeRet = CTxOut();

    CScript payee;
    bool foundMaxVotedPayee = true;

    if (!mnpayments.GetBlockPayee(nBlockHeight, payee)) {
        // no zeronode detected...
        // LogPrintf("no zeronode detected...\n");
        foundMaxVotedPayee = false;
        int nCount = 0;
        CZeronode *winningNode = mnodeman.GetNextZeronodeInQueueForPayment(nBlockHeight, true, nCount);
        if (!winningNode) {
            // ...and we can't calculate it on our own
            LogPrintf("CZeronodePayments::FillBlockPayee -- Failed to detect zeronode to pay\n");
            return;
        }
        // fill payee with locally calculated winner and hope for the best
        payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
        LogPrintf("payee=%s\n", winningNode->ToString());
    }
    txoutZeronodeRet = CTxOut(zeronodePayment, payee);
    txNew.vout.push_back(txoutZeronodeRet);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);
    if (foundMaxVotedPayee) {
        LogPrintf("CZeronodePayments::FillBlockPayee::foundMaxVotedPayee -- Zeronode payment %lld to %s\n", zeronodePayment, address2.ToString());
    } else {
        LogPrintf("CZeronodePayments::FillBlockPayee -- Zeronode payment %lld to %s\n", zeronodePayment, address2.ToString());
    }

}

int CZeronodePayments::GetMinZeronodePaymentsProto() {
    return sporkManager.IsSporkActive(SPORK_10_ZERONODE_PAY_UPDATED_NODES)
           ? MIN_ZERONODE_PAYMENT_PROTO_VERSION_2
           : MIN_ZERONODE_PAYMENT_PROTO_VERSION_1;
}

void CZeronodePayments::ProcessMessage(CNode *pfrom, std::string &strCommand, CDataStream &vRecv) {

//    LogPrintf("CZeronodePayments::ProcessMessage strCommand=%s\n", strCommand);
    // Ignore any payments messages until zeronode list is synced
    if (!zeronodeSync.IsZeronodeListSynced()) return;

    if (fLiteMode) return; // disable all Dash specific functionality

    if (strCommand == NetMsgType::ZERONODEPAYMENTSYNC) { //Zeronode Payments Request Sync

        // Ignore such requests until we are fully synced.
        // We could start processing this after zeronode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!zeronodeSync.IsSynced()) return;

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (netfulfilledman.HasFulfilledRequest(pfrom->addr, NetMsgType::ZERONODEPAYMENTSYNC)) {
            // Asking for the payments list multiple times in a short period of time is no good
            LogPrintf("ZERONODEPAYMENTSYNC -- peer already asked me for the list, peer=%d\n", pfrom->id);
            Misbehaving(pfrom->GetId(), 20);
            return;
        }
        netfulfilledman.AddFulfilledRequest(pfrom->addr, NetMsgType::ZERONODEPAYMENTSYNC);

        Sync(pfrom);
        LogPrint("mnpayments", "ZERONODEPAYMENTSYNC -- Sent Zeronode payment votes to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::ZERONODEPAYMENTVOTE) { // Zeronode Payments Vote for the Winner

        CZeronodePaymentVote vote;
        vRecv >> vote;

        if (pfrom->nVersion < GetMinZeronodePaymentsProto()) return;

        if (!pCurrentBlockIndex) return;

        uint256 nHash = vote.GetHash();

        pfrom->setAskFor.erase(nHash);

        {
            LOCK(cs_mapZeronodePaymentVotes);
            if (mapZeronodePaymentVotes.count(nHash)) {
                LogPrint("mnpayments", "ZERONODEPAYMENTVOTE -- hash=%s, nHeight=%d seen\n", nHash.ToString(), pCurrentBlockIndex->nHeight);
                return;
            }

            // Avoid processing same vote multiple times
            mapZeronodePaymentVotes[nHash] = vote;
            // but first mark vote as non-verified,
            // AddPaymentVote() below should take care of it if vote is actually ok
            mapZeronodePaymentVotes[nHash].MarkAsNotVerified();
        }

        int nFirstBlock = pCurrentBlockIndex->nHeight - GetStorageLimit();
        if (vote.nBlockHeight < nFirstBlock || vote.nBlockHeight > pCurrentBlockIndex->nHeight + 20) {
            LogPrint("mnpayments", "ZERONODEPAYMENTVOTE -- vote out of range: nFirstBlock=%d, nBlockHeight=%d, nHeight=%d\n", nFirstBlock, vote.nBlockHeight, pCurrentBlockIndex->nHeight);
            return;
        }

        std::string strError = "";
        if (!vote.IsValid(pfrom, pCurrentBlockIndex->nHeight, strError)) {
            LogPrint("mnpayments", "ZERONODEPAYMENTVOTE -- invalid message, error: %s\n", strError);
            return;
        }

        if (!CanVote(vote.vinZeronode.prevout, vote.nBlockHeight)) {
            LogPrintf("ZERONODEPAYMENTVOTE -- zeronode already voted, zeronode=%s\n", vote.vinZeronode.prevout.ToStringShort());
            return;
        }

        zeronode_info_t mnInfo = mnodeman.GetZeronodeInfo(vote.vinZeronode);
        if (!mnInfo.fInfoValid) {
            // mn was not found, so we can't check vote, some info is probably missing
            LogPrintf("ZERONODEPAYMENTVOTE -- zeronode is missing %s\n", vote.vinZeronode.prevout.ToStringShort());
            mnodeman.AskForMN(pfrom, vote.vinZeronode);
            return;
        }

        int nDos = 0;
        if (!vote.CheckSignature(mnInfo.pubKeyZeronode, pCurrentBlockIndex->nHeight, nDos)) {
            if (nDos) {
                LogPrintf("ZERONODEPAYMENTVOTE -- ERROR: invalid signature\n");
                Misbehaving(pfrom->GetId(), nDos);
            } else {
                // only warn about anything non-critical (i.e. nDos == 0) in debug mode
                LogPrint("mnpayments", "ZERONODEPAYMENTVOTE -- WARNING: invalid signature\n");
            }
            // Either our info or vote info could be outdated.
            // In case our info is outdated, ask for an update,
            mnodeman.AskForMN(pfrom, vote.vinZeronode);
            // but there is nothing we can do if vote info itself is outdated
            // (i.e. it was signed by a mn which changed its key),
            // so just quit here.
            return;
        }

        CTxDestination address1;
        ExtractDestination(vote.payee, address1);
        CBitcoinAddress address2(address1);

        LogPrint("mnpayments", "ZERONODEPAYMENTVOTE -- vote: address=%s, nBlockHeight=%d, nHeight=%d, prevout=%s\n", address2.ToString(), vote.nBlockHeight, pCurrentBlockIndex->nHeight, vote.vinZeronode.prevout.ToStringShort());

        if (AddPaymentVote(vote)) {
            vote.Relay();
            zeronodeSync.AddedPaymentVote();
        }
    }
}

bool CZeronodePaymentVote::Sign() {
    std::string strError;
    std::string strMessage = vinZeronode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             ScriptToAsmStr(payee);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, activeZeronode.keyZeronode)) {
        LogPrintf("CZeronodePaymentVote::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(activeZeronode.pubKeyZeronode, vchSig, strMessage, strError)) {
        LogPrintf("CZeronodePaymentVote::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CZeronodePayments::GetBlockPayee(int nBlockHeight, CScript &payee) {
    if (mapZeronodeBlocks.count(nBlockHeight)) {
        return mapZeronodeBlocks[nBlockHeight].GetBestPayee(payee);
    }

    return false;
}

// Is this zeronode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 blocks of votes
bool CZeronodePayments::IsScheduled(CZeronode &mn, int nNotBlockHeight) {
    LOCK(cs_mapZeronodeBlocks);

    if (!pCurrentBlockIndex) return false;

    CScript mnpayee;
    mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = pCurrentBlockIndex->nHeight; h <= pCurrentBlockIndex->nHeight + 8; h++) {
        if (h == nNotBlockHeight) continue;
        if (mapZeronodeBlocks.count(h) && mapZeronodeBlocks[h].GetBestPayee(payee) && mnpayee == payee) {
            return true;
        }
    }

    return false;
}

bool CZeronodePayments::AddPaymentVote(const CZeronodePaymentVote &vote) {
    LogPrint("zeronode-payments", "CZeronodePayments::AddPaymentVote\n");
    uint256 blockHash = uint256();
    if (!GetBlockHash(blockHash, vote.nBlockHeight - 101)) return false;

    if (HasVerifiedPaymentVote(vote.GetHash())) return false;

    LOCK2(cs_mapZeronodeBlocks, cs_mapZeronodePaymentVotes);

    mapZeronodePaymentVotes[vote.GetHash()] = vote;

    if (!mapZeronodeBlocks.count(vote.nBlockHeight)) {
        CZeronodeBlockPayees blockPayees(vote.nBlockHeight);
        mapZeronodeBlocks[vote.nBlockHeight] = blockPayees;
    }

    mapZeronodeBlocks[vote.nBlockHeight].AddPayee(vote);

    return true;
}

bool CZeronodePayments::HasVerifiedPaymentVote(uint256 hashIn) {
    LOCK(cs_mapZeronodePaymentVotes);
    std::map<uint256, CZeronodePaymentVote>::iterator it = mapZeronodePaymentVotes.find(hashIn);
    return it != mapZeronodePaymentVotes.end() && it->second.IsVerified();
}

void CZeronodeBlockPayees::AddPayee(const CZeronodePaymentVote &vote) {
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CZeronodePayee & payee, vecPayees)
    {
        if (payee.GetPayee() == vote.payee) {
            payee.AddVoteHash(vote.GetHash());
            return;
        }
    }
    CZeronodePayee payeeNew(vote.payee, vote.GetHash());
    vecPayees.push_back(payeeNew);
}

bool CZeronodeBlockPayees::GetBestPayee(CScript &payeeRet) {
    LOCK(cs_vecPayees);
    LogPrint("mnpayments", "CZeronodeBlockPayees::GetBestPayee, vecPayees.size()=%s\n", vecPayees.size());
    if (!vecPayees.size()) {
        LogPrint("mnpayments", "CZeronodeBlockPayees::GetBestPayee -- ERROR: couldn't find any payee\n");
        return false;
    }

    int nVotes = -1;
    BOOST_FOREACH(CZeronodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() > nVotes) {
            payeeRet = payee.GetPayee();
            nVotes = payee.GetVoteCount();
        }
    }

    return (nVotes > -1);
}

bool CZeronodeBlockPayees::HasPayeeWithVotes(CScript payeeIn, int nVotesReq) {
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CZeronodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= nVotesReq && payee.GetPayee() == payeeIn) {
            return true;
        }
    }

//    LogPrint("mnpayments", "CZeronodeBlockPayees::HasPayeeWithVotes -- ERROR: couldn't find any payee with %d+ votes\n", nVotesReq);
    return false;
}

bool CZeronodeBlockPayees::IsTransactionValid(const CTransaction &txNew) {
    LOCK(cs_vecPayees);

    int nMaxSignatures = 0;
    std::string strPayeesPossible = "";

    CAmount nZeronodePayment = GetZeronodePayment(nBlockHeight, txNew.GetValueOut());

    //require at least MNPAYMENTS_SIGNATURES_REQUIRED signatures

    BOOST_FOREACH(CZeronodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= nMaxSignatures) {
            nMaxSignatures = payee.GetVoteCount();
        }
    }

    // if we don't have at least MNPAYMENTS_SIGNATURES_REQUIRED signatures on a payee, approve whichever is the longest chain
    if (nMaxSignatures < MNPAYMENTS_SIGNATURES_REQUIRED) return true;

    bool hasValidPayee = false;

    BOOST_FOREACH(CZeronodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= MNPAYMENTS_SIGNATURES_REQUIRED) {
            hasValidPayee = true;

            BOOST_FOREACH(CTxOut txout, txNew.vout) {
                if (payee.GetPayee() == txout.scriptPubKey && nZeronodePayment == txout.nValue) {
                    LogPrint("mnpayments", "CZeronodeBlockPayees::IsTransactionValid -- Found required payment\n");
                    return true;
                }
            }

            CTxDestination address1;
            ExtractDestination(payee.GetPayee(), address1);
            CBitcoinAddress address2(address1);

            if (strPayeesPossible == "") {
                strPayeesPossible = address2.ToString();
            } else {
                strPayeesPossible += "," + address2.ToString();
            }
        }
    }

    if (!hasValidPayee) return true;

    LogPrintf("CZeronodeBlockPayees::IsTransactionValid -- ERROR: Missing required payment, possible payees: '%s', amount: %f ZRO\n", strPayeesPossible, (float) nZeronodePayment / COIN);
    return false;
}

std::string CZeronodeBlockPayees::GetRequiredPaymentsString() {
    LOCK(cs_vecPayees);

    std::string strRequiredPayments = "Unknown";

    BOOST_FOREACH(CZeronodePayee & payee, vecPayees)
    {
        CTxDestination address1;
        ExtractDestination(payee.GetPayee(), address1);
        CBitcoinAddress address2(address1);

        if (strRequiredPayments != "Unknown") {
            strRequiredPayments += ", " + address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        } else {
            strRequiredPayments = address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        }
    }

    return strRequiredPayments;
}

std::string CZeronodePayments::GetRequiredPaymentsString(int nBlockHeight) {
    LOCK(cs_mapZeronodeBlocks);

    if (mapZeronodeBlocks.count(nBlockHeight)) {
        return mapZeronodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CZeronodePayments::IsTransactionValid(const CTransaction &txNew, int nBlockHeight) {
    LOCK(cs_mapZeronodeBlocks);

    if (mapZeronodeBlocks.count(nBlockHeight)) {
        return mapZeronodeBlocks[nBlockHeight].IsTransactionValid(txNew);
    }

    return true;
}

void CZeronodePayments::CheckAndRemove() {
    if (!pCurrentBlockIndex) return;

    LOCK2(cs_mapZeronodeBlocks, cs_mapZeronodePaymentVotes);

    int nLimit = GetStorageLimit();

    std::map<uint256, CZeronodePaymentVote>::iterator it = mapZeronodePaymentVotes.begin();
    while (it != mapZeronodePaymentVotes.end()) {
        CZeronodePaymentVote vote = (*it).second;

        if (pCurrentBlockIndex->nHeight - vote.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CZeronodePayments::CheckAndRemove -- Removing old Zeronode payment: nBlockHeight=%d\n", vote.nBlockHeight);
            mapZeronodePaymentVotes.erase(it++);
            mapZeronodeBlocks.erase(vote.nBlockHeight);
        } else {
            ++it;
        }
    }
    LogPrintf("CZeronodePayments::CheckAndRemove -- %s\n", ToString());
}

bool CZeronodePaymentVote::IsValid(CNode *pnode, int nValidationHeight, std::string &strError) {
    CZeronode *pmn = mnodeman.Find(vinZeronode);

    if (!pmn) {
        strError = strprintf("Unknown Zeronode: prevout=%s", vinZeronode.prevout.ToStringShort());
        // Only ask if we are already synced and still have no idea about that Zeronode
        if (zeronodeSync.IsZeronodeListSynced()) {
            mnodeman.AskForMN(pnode, vinZeronode);
        }

        return false;
    }

    int nMinRequiredProtocol;
    if (nBlockHeight >= nValidationHeight) {
        // new votes must comply SPORK_10_ZERONODE_PAY_UPDATED_NODES rules
        nMinRequiredProtocol = mnpayments.GetMinZeronodePaymentsProto();
    } else {
        // allow non-updated zeronodes for old blocks
        nMinRequiredProtocol = MIN_ZERONODE_PAYMENT_PROTO_VERSION_1;
    }

    if (pmn->nProtocolVersion < nMinRequiredProtocol) {
        strError = strprintf("Zeronode protocol is too old: nProtocolVersion=%d, nMinRequiredProtocol=%d", pmn->nProtocolVersion, nMinRequiredProtocol);
        return false;
    }

    // Only zeronodes should try to check zeronode rank for old votes - they need to pick the right winner for future blocks.
    // Regular clients (miners included) need to verify zeronode rank for future block votes only.
    if (!fZNode && nBlockHeight < nValidationHeight) return true;

    int nRank = mnodeman.GetZeronodeRank(vinZeronode, nBlockHeight - 101, nMinRequiredProtocol, false);

    if (nRank == -1) {
        LogPrint("mnpayments", "CZeronodePaymentVote::IsValid -- Can't calculate rank for zeronode %s\n",
                 vinZeronode.prevout.ToStringShort());
        return false;
    }

    if (nRank > MNPAYMENTS_SIGNATURES_TOTAL) {
        // It's common to have zeronodes mistakenly think they are in the top 10
        // We don't want to print all of these messages in normal mode, debug mode should print though
        strError = strprintf("Zeronode is not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL, nRank);
        // Only ban for new mnw which is out of bounds, for old mnw MN list itself might be way too much off
        if (nRank > MNPAYMENTS_SIGNATURES_TOTAL * 2 && nBlockHeight > nValidationHeight) {
            strError = strprintf("Zeronode is not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, nRank);
            LogPrintf("CZeronodePaymentVote::IsValid -- Error: %s\n", strError);
            Misbehaving(pnode->GetId(), 20);
        }
        // Still invalid however
        return false;
    }

    return true;
}

bool CZeronodePayments::ProcessBlock(int nBlockHeight) {

    // DETERMINE IF WE SHOULD BE VOTING FOR THE NEXT PAYEE

    if (fLiteMode || !fZNode) {
        return false;
    }

    // We have little chances to pick the right winner if winners list is out of sync
    // but we have no choice, so we'll try. However it doesn't make sense to even try to do so
    // if we have not enough data about zeronodes.
    if (!zeronodeSync.IsZeronodeListSynced()) {
        return false;
    }

    int nRank = mnodeman.GetZeronodeRank(activeZeronode.vin, nBlockHeight - 101, GetMinZeronodePaymentsProto(), false);

    if (nRank == -1) {
        LogPrint("mnpayments", "CZeronodePayments::ProcessBlock -- Unknown Zeronode\n");
        return false;
    }

    if (nRank > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("mnpayments", "CZeronodePayments::ProcessBlock -- Zeronode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, nRank);
        return false;
    }

    // LOCATE THE NEXT ZERONODE WHICH SHOULD BE PAID

    LogPrintf("CZeronodePayments::ProcessBlock -- Start: nBlockHeight=%d, zeronode=%s\n", nBlockHeight, activeZeronode.vin.prevout.ToStringShort());

    // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
    int nCount = 0;
    CZeronode *pmn = mnodeman.GetNextZeronodeInQueueForPayment(nBlockHeight, true, nCount);

    if (pmn == NULL) {
        LogPrintf("CZeronodePayments::ProcessBlock -- ERROR: Failed to find zeronode to pay\n");
        return false;
    }

    LogPrintf("CZeronodePayments::ProcessBlock -- Zeronode found by GetNextZeronodeInQueueForPayment(): %s\n", pmn->vin.prevout.ToStringShort());


    CScript payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());

    CZeronodePaymentVote voteNew(activeZeronode.vin, nBlockHeight, payee);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);

    // SIGN MESSAGE TO NETWORK WITH OUR ZERONODE KEYS

    if (voteNew.Sign()) {
        if (AddPaymentVote(voteNew)) {
            voteNew.Relay();
            return true;
        }
    }

    return false;
}

void CZeronodePaymentVote::Relay() {
    // do not relay until synced
    if (!zeronodeSync.IsWinnersListSynced()) {
        LogPrintf("CZeronodePaymentVote::Relay - zeronodeSync.IsWinnersListSynced() not sync\n");
        return;
    }
    CInv inv(MSG_ZERONODE_PAYMENT_VOTE, GetHash());
    RelayInv(inv);
}

bool CZeronodePaymentVote::CheckSignature(const CPubKey &pubKeyZeronode, int nValidationHeight, int &nDos) {
    // do not ban by default
    nDos = 0;

    std::string strMessage = vinZeronode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             ScriptToAsmStr(payee);

    std::string strError = "";
    if (!darkSendSigner.VerifyMessage(pubKeyZeronode, vchSig, strMessage, strError)) {
        // Only ban for future block vote when we are already synced.
        // Otherwise it could be the case when MN which signed this vote is using another key now
        // and we have no idea about the old one.
        if (zeronodeSync.IsZeronodeListSynced() && nBlockHeight > nValidationHeight) {
            nDos = 20;
        }
        return error("CZeronodePaymentVote::CheckSignature -- Got bad Zeronode payment signature, zeronode=%s, error: %s", vinZeronode.prevout.ToStringShort().c_str(), strError);
    }

    return true;
}

std::string CZeronodePaymentVote::ToString() const {
    std::ostringstream info;

    info << vinZeronode.prevout.ToStringShort() <<
         ", " << nBlockHeight <<
         ", " << ScriptToAsmStr(payee) <<
         ", " << (int) vchSig.size();

    return info.str();
}

// Send only votes for future blocks, node should request every other missing payment block individually
void CZeronodePayments::Sync(CNode *pnode) {
    LOCK(cs_mapZeronodeBlocks);

    if (!pCurrentBlockIndex) return;

    int nInvCount = 0;

    for (int h = pCurrentBlockIndex->nHeight; h < pCurrentBlockIndex->nHeight + 20; h++) {
        if (mapZeronodeBlocks.count(h)) {
            BOOST_FOREACH(CZeronodePayee & payee, mapZeronodeBlocks[h].vecPayees)
            {
                std::vector <uint256> vecVoteHashes = payee.GetVoteHashes();
                BOOST_FOREACH(uint256 & hash, vecVoteHashes)
                {
                    if (!HasVerifiedPaymentVote(hash)) continue;
                    pnode->PushInventory(CInv(MSG_ZERONODE_PAYMENT_VOTE, hash));
                    nInvCount++;
                }
            }
        }
    }

    LogPrintf("CZeronodePayments::Sync -- Sent %d votes to peer %d\n", nInvCount, pnode->id);
    pnode->PushMessage(NetMsgType::SYNCSTATUSCOUNT, ZERONODE_SYNC_MNW, nInvCount);
}

// Request low data/unknown payment blocks in batches directly from some node instead of/after preliminary Sync.
void CZeronodePayments::RequestLowDataPaymentBlocks(CNode *pnode) {
    if (!pCurrentBlockIndex) return;

    LOCK2(cs_main, cs_mapZeronodeBlocks);

    std::vector <CInv> vToFetch;
    int nLimit = GetStorageLimit();

    const CBlockIndex *pindex = pCurrentBlockIndex;

    while (pCurrentBlockIndex->nHeight - pindex->nHeight < nLimit) {
        if (!mapZeronodeBlocks.count(pindex->nHeight)) {
            // We have no idea about this block height, let's ask
            vToFetch.push_back(CInv(MSG_ZERONODE_PAYMENT_BLOCK, pindex->GetBlockHash()));
            // We should not violate GETDATA rules
            if (vToFetch.size() == MAX_INV_SZ) {
                LogPrintf("CZeronodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d blocks\n", pnode->id, MAX_INV_SZ);
                pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
                // Start filling new batch
                vToFetch.clear();
            }
        }
        if (!pindex->pprev) break;
        pindex = pindex->pprev;
    }

    std::map<int, CZeronodeBlockPayees>::iterator it = mapZeronodeBlocks.begin();

    while (it != mapZeronodeBlocks.end()) {
        int nTotalVotes = 0;
        bool fFound = false;
        BOOST_FOREACH(CZeronodePayee & payee, it->second.vecPayees)
        {
            if (payee.GetVoteCount() >= MNPAYMENTS_SIGNATURES_REQUIRED) {
                fFound = true;
                break;
            }
            nTotalVotes += payee.GetVoteCount();
        }
        // A clear winner (MNPAYMENTS_SIGNATURES_REQUIRED+ votes) was found
        // or no clear winner was found but there are at least avg number of votes
        if (fFound || nTotalVotes >= (MNPAYMENTS_SIGNATURES_TOTAL + MNPAYMENTS_SIGNATURES_REQUIRED) / 2) {
            // so just move to the next block
            ++it;
            continue;
        }
        // DEBUG
//        DBG (
//            // Let's see why this failed
//            BOOST_FOREACH(CZeronodePayee& payee, it->second.vecPayees) {
//                CTxDestination address1;
//                ExtractDestination(payee.GetPayee(), address1);
//                CBitcoinAddress address2(address1);
//                printf("payee %s votes %d\n", address2.ToString().c_str(), payee.GetVoteCount());
//            }
//            printf("block %d votes total %d\n", it->first, nTotalVotes);
//        )
        // END DEBUG
        // Low data block found, let's try to sync it
        uint256 hash;
        if (GetBlockHash(hash, it->first)) {
            vToFetch.push_back(CInv(MSG_ZERONODE_PAYMENT_BLOCK, hash));
        }
        // We should not violate GETDATA rules
        if (vToFetch.size() == MAX_INV_SZ) {
            LogPrintf("CZeronodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, MAX_INV_SZ);
            pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
            // Start filling new batch
            vToFetch.clear();
        }
        ++it;
    }
    // Ask for the rest of it
    if (!vToFetch.empty()) {
        LogPrintf("CZeronodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, vToFetch.size());
        pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
    }
}

std::string CZeronodePayments::ToString() const {
    std::ostringstream info;

    info << "Votes: " << (int) mapZeronodePaymentVotes.size() <<
         ", Blocks: " << (int) mapZeronodeBlocks.size();

    return info.str();
}

bool CZeronodePayments::IsEnoughData() {
    float nAverageVotes = (MNPAYMENTS_SIGNATURES_TOTAL + MNPAYMENTS_SIGNATURES_REQUIRED) / 2;
    int nStorageLimit = GetStorageLimit();
    return GetBlockCount() > nStorageLimit && GetVoteCount() > nStorageLimit * nAverageVotes;
}

int CZeronodePayments::GetStorageLimit() {
    return std::max(int(mnodeman.size() * nStorageCoeff), nMinBlocksToStore);
}

void CZeronodePayments::UpdatedBlockTip(const CBlockIndex *pindex) {
    pCurrentBlockIndex = pindex;
    LogPrint("mnpayments", "CZeronodePayments::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);
    
    ProcessBlock(pindex->nHeight + 5);
}
