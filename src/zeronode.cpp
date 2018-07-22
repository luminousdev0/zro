// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activezeronode.h"
#include "consensus/validation.h"
#include "darksend.h"
#include "init.h"
//#include "governance.h"
#include "zeronode.h"
#include "zeronode-payments.h"
#include "zeronode-sync.h"
#include "zeronodeman.h"
#include "util.h"

#include <boost/lexical_cast.hpp>


CZeronode::CZeronode() :
        vin(),
        addr(),
        pubKeyCollateralAddress(),
        pubKeyZeronode(),
        lastPing(),
        vchSig(),
        sigTime(GetAdjustedTime()),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(0),
        nActiveState(ZERONODE_ENABLED),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(PROTOCOL_VERSION),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

CZeronode::CZeronode(CService addrNew, CTxIn vinNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyZeronodeNew, int nProtocolVersionIn) :
        vin(vinNew),
        addr(addrNew),
        pubKeyCollateralAddress(pubKeyCollateralAddressNew),
        pubKeyZeronode(pubKeyZeronodeNew),
        lastPing(),
        vchSig(),
        sigTime(GetAdjustedTime()),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(0),
        nActiveState(ZERONODE_ENABLED),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(nProtocolVersionIn),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

CZeronode::CZeronode(const CZeronode &other) :
        vin(other.vin),
        addr(other.addr),
        pubKeyCollateralAddress(other.pubKeyCollateralAddress),
        pubKeyZeronode(other.pubKeyZeronode),
        lastPing(other.lastPing),
        vchSig(other.vchSig),
        sigTime(other.sigTime),
        nLastDsq(other.nLastDsq),
        nTimeLastChecked(other.nTimeLastChecked),
        nTimeLastPaid(other.nTimeLastPaid),
        nTimeLastWatchdogVote(other.nTimeLastWatchdogVote),
        nActiveState(other.nActiveState),
        nCacheCollateralBlock(other.nCacheCollateralBlock),
        nBlockLastPaid(other.nBlockLastPaid),
        nProtocolVersion(other.nProtocolVersion),
        nPoSeBanScore(other.nPoSeBanScore),
        nPoSeBanHeight(other.nPoSeBanHeight),
        fAllowMixingTx(other.fAllowMixingTx),
        fUnitTest(other.fUnitTest) {}

CZeronode::CZeronode(const CZeronodeBroadcast &mnb) :
        vin(mnb.vin),
        addr(mnb.addr),
        pubKeyCollateralAddress(mnb.pubKeyCollateralAddress),
        pubKeyZeronode(mnb.pubKeyZeronode),
        lastPing(mnb.lastPing),
        vchSig(mnb.vchSig),
        sigTime(mnb.sigTime),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(mnb.sigTime),
        nActiveState(mnb.nActiveState),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(mnb.nProtocolVersion),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

//CSporkManager sporkManager;
//
// When a new zeronode broadcast is sent, update our information
//
bool CZeronode::UpdateFromNewBroadcast(CZeronodeBroadcast &mnb) {
    if (mnb.sigTime <= sigTime && !mnb.fRecovery) return false;

    pubKeyZeronode = mnb.pubKeyZeronode;
    sigTime = mnb.sigTime;
    vchSig = mnb.vchSig;
    nProtocolVersion = mnb.nProtocolVersion;
    addr = mnb.addr;
    nPoSeBanScore = 0;
    nPoSeBanHeight = 0;
    nTimeLastChecked = 0;
    int nDos = 0;
    if (mnb.lastPing == CZeronodePing() || (mnb.lastPing != CZeronodePing() && mnb.lastPing.CheckAndUpdate(this, true, nDos))) {
        lastPing = mnb.lastPing;
        mnodeman.mapSeenZeronodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
    }
    // if it matches our Zeronode privkey...
    if (fZNode && pubKeyZeronode == activeZeronode.pubKeyZeronode) {
        nPoSeBanScore = -ZERONODE_POSE_BAN_MAX_SCORE;
        if (nProtocolVersion == PROTOCOL_VERSION) {
            // ... and PROTOCOL_VERSION, then we've been remotely activated ...
            activeZeronode.ManageState();
        } else {
            // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
            // but also do not ban the node we get this message from
            LogPrintf("CZeronode::UpdateFromNewBroadcast -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", nProtocolVersion, PROTOCOL_VERSION);
            return false;
        }
    }
    return true;
}

//
// Deterministically calculate a given "score" for a Zeronode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CZeronode::CalculateScore(const uint256 &blockHash) {
    uint256 aux = ArithToUint256(UintToArith256(vin.prevout.hash) + vin.prevout.n);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << blockHash;
    arith_uint256 hash2 = UintToArith256(ss.GetHash());

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << blockHash;
    ss2 << aux;
    arith_uint256 hash3 = UintToArith256(ss2.GetHash());

    return (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
}

void CZeronode::Check(bool fForce) {
    LOCK(cs);

    if (ShutdownRequested()) return;

    if (!fForce && (GetTime() - nTimeLastChecked < ZERONODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    LogPrint("zeronode", "CZeronode::Check -- Zeronode %s is in %s state\n", vin.prevout.ToStringShort(), GetStateString());

    //once spent, stop doing the checks
    if (IsOutpointSpent()) return;

    int nHeight = 0;
    if (!fUnitTest) {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) return;

        CCoins coins;
        if (!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
            (unsigned int) vin.prevout.n >= coins.vout.size() ||
            coins.vout[vin.prevout.n].IsNull()) {
            nActiveState = ZERONODE_OUTPOINT_SPENT;
            LogPrint("zeronode", "CZeronode::Check -- Failed to find Zeronode UTXO, zeronode=%s\n", vin.prevout.ToStringShort());
            return;
        }

        nHeight = chainActive.Height();
    }

    if (IsPoSeBanned()) {
        if (nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Zeronode still will be on the edge and can be banned back easily if it keeps ignoring mnverify
        // or connect attempts. Will require few mnverify messages to strengthen its position in mn list.
        LogPrintf("CZeronode::Check -- Zeronode %s is unbanned and back in list now\n", vin.prevout.ToStringShort());
        DecreasePoSeBanScore();
    } else if (nPoSeBanScore >= ZERONODE_POSE_BAN_MAX_SCORE) {
        nActiveState = ZERONODE_POSE_BAN;
        // ban for the whole payment cycle
        nPoSeBanHeight = nHeight + mnodeman.size();
        LogPrintf("CZeronode::Check -- Zeronode %s is banned till block %d now\n", vin.prevout.ToStringShort(), nPoSeBanHeight);
        return;
    }

    int nActiveStatePrev = nActiveState;
    bool fOurZeronode = fZNode && activeZeronode.pubKeyZeronode == pubKeyZeronode;

    // zeronode doesn't meet payment protocol requirements ...
    bool fRequireUpdate = nProtocolVersion < mnpayments.GetMinZeronodePaymentsProto() ||
                          // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                          (fOurZeronode && nProtocolVersion < PROTOCOL_VERSION);

    if (fRequireUpdate) {
        nActiveState = ZERONODE_UPDATE_REQUIRED;
        if (nActiveStatePrev != nActiveState) {
            LogPrint("zeronode", "CZeronode::Check -- Zeronode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    // keep old zeronodes on start, give them a chance to receive updates...
    bool fWaitForPing = !zeronodeSync.IsZeronodeListSynced() && !IsPingedWithin(ZERONODE_MIN_MNP_SECONDS);

    if (fWaitForPing && !fOurZeronode) {
        // ...but if it was already expired before the initial check - return right away
        if (IsExpired() || IsWatchdogExpired() || IsNewStartRequired()) {
            LogPrint("zeronode", "CZeronode::Check -- Zeronode %s is in %s state, waiting for ping\n", vin.prevout.ToStringShort(), GetStateString());
            return;
        }
    }

    // don't expire if we are still in "waiting for ping" mode unless it's our own zeronode
    if (!fWaitForPing || fOurZeronode) {

        if (!IsPingedWithin(ZERONODE_NEW_START_REQUIRED_SECONDS)) {
            nActiveState = ZERONODE_NEW_START_REQUIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("zeronode", "CZeronode::Check -- Zeronode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        bool fWatchdogActive = zeronodeSync.IsSynced() && mnodeman.IsWatchdogActive();
        bool fWatchdogExpired = (fWatchdogActive && ((GetTime() - nTimeLastWatchdogVote) > ZERONODE_WATCHDOG_MAX_SECONDS));

//        LogPrint("zeronode", "CZeronode::Check -- outpoint=%s, nTimeLastWatchdogVote=%d, GetTime()=%d, fWatchdogExpired=%d\n",
//                vin.prevout.ToStringShort(), nTimeLastWatchdogVote, GetTime(), fWatchdogExpired);

        if (fWatchdogExpired) {
            nActiveState = ZERONODE_WATCHDOG_EXPIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("zeronode", "CZeronode::Check -- Zeronode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        if (!IsPingedWithin(ZERONODE_EXPIRATION_SECONDS)) {
            nActiveState = ZERONODE_EXPIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("zeronode", "CZeronode::Check -- Zeronode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }
    }

    if (lastPing.sigTime - sigTime < ZERONODE_MIN_MNP_SECONDS) {
        nActiveState = ZERONODE_PRE_ENABLED;
        if (nActiveStatePrev != nActiveState) {
            LogPrint("zeronode", "CZeronode::Check -- Zeronode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    nActiveState = ZERONODE_ENABLED; // OK
    if (nActiveStatePrev != nActiveState) {
        LogPrint("zeronode", "CZeronode::Check -- Zeronode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
    }
}

bool CZeronode::IsValidNetAddr() {
    return IsValidNetAddr(addr);
}

bool CZeronode::IsValidForPayment() {
    if (nActiveState == ZERONODE_ENABLED) {
        return true;
    }
//    if(!sporkManager.IsSporkActive(SPORK_14_REQUIRE_SENTINEL_FLAG) &&
//       (nActiveState == ZERONODE_WATCHDOG_EXPIRED)) {
//        return true;
//    }

    return false;
}

bool CZeronode::IsValidNetAddr(CService addrIn) {
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
           (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

zeronode_info_t CZeronode::GetInfo() {
    zeronode_info_t info;
    info.vin = vin;
    info.addr = addr;
    info.pubKeyCollateralAddress = pubKeyCollateralAddress;
    info.pubKeyZeronode = pubKeyZeronode;
    info.sigTime = sigTime;
    info.nLastDsq = nLastDsq;
    info.nTimeLastChecked = nTimeLastChecked;
    info.nTimeLastPaid = nTimeLastPaid;
    info.nTimeLastWatchdogVote = nTimeLastWatchdogVote;
    info.nTimeLastPing = lastPing.sigTime;
    info.nActiveState = nActiveState;
    info.nProtocolVersion = nProtocolVersion;
    info.fInfoValid = true;
    return info;
}

std::string CZeronode::StateToString(int nStateIn) {
    switch (nStateIn) {
        case ZERONODE_PRE_ENABLED:
            return "PRE_ENABLED";
        case ZERONODE_ENABLED:
            return "ENABLED";
        case ZERONODE_EXPIRED:
            return "EXPIRED";
        case ZERONODE_OUTPOINT_SPENT:
            return "OUTPOINT_SPENT";
        case ZERONODE_UPDATE_REQUIRED:
            return "UPDATE_REQUIRED";
        case ZERONODE_WATCHDOG_EXPIRED:
            return "WATCHDOG_EXPIRED";
        case ZERONODE_NEW_START_REQUIRED:
            return "NEW_START_REQUIRED";
        case ZERONODE_POSE_BAN:
            return "POSE_BAN";
        default:
            return "UNKNOWN";
    }
}

std::string CZeronode::GetStateString() const {
    return StateToString(nActiveState);
}

std::string CZeronode::GetStatus() const {
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

std::string CZeronode::ToString() const {
    std::string str;
    str += "zeronode{";
    str += addr.ToString();
    str += " ";
    str += std::to_string(nProtocolVersion);
    str += " ";
    str += vin.prevout.ToStringShort();
    str += " ";
    str += CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString();
    str += " ";
    str += std::to_string(lastPing == CZeronodePing() ? sigTime : lastPing.sigTime);
    str += " ";
    str += std::to_string(lastPing == CZeronodePing() ? 0 : lastPing.sigTime - sigTime);
    str += " ";
    str += std::to_string(nBlockLastPaid);
    str += "}\n";
    return str;
}

int CZeronode::GetCollateralAge() {
    int nHeight;
    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain || !chainActive.Tip()) return -1;
        nHeight = chainActive.Height();
    }

    if (nCacheCollateralBlock == 0) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge > 0) {
            nCacheCollateralBlock = nHeight - nInputAge;
        } else {
            return nInputAge;
        }
    }

    return nHeight - nCacheCollateralBlock;
}

void CZeronode::UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack) {
    if (!pindex) {
        LogPrintf("CZeronode::UpdateLastPaid pindex is NULL\n");
        return;
    }

    const CBlockIndex *BlockReading = pindex;

    CScript mnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());
    LogPrint("zeronode", "CZeronode::UpdateLastPaidBlock -- searching for block with payment to %s\n", vin.prevout.ToStringShort());

    LOCK(cs_mapZeronodeBlocks);

    for (int i = 0; BlockReading && BlockReading->nHeight > nBlockLastPaid && i < nMaxBlocksToScanBack; i++) {
//        LogPrintf("mnpayments.mapZeronodeBlocks.count(BlockReading->nHeight)=%s\n", mnpayments.mapZeronodeBlocks.count(BlockReading->nHeight));
//        LogPrintf("mnpayments.mapZeronodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)=%s\n", mnpayments.mapZeronodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2));
        if (mnpayments.mapZeronodeBlocks.count(BlockReading->nHeight) &&
            mnpayments.mapZeronodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)) {
            // LogPrintf("i=%s, BlockReading->nHeight=%s\n", i, BlockReading->nHeight);
            CBlock block;
            if (!ReadBlockFromDisk(block, BlockReading, Params().GetConsensus())) // shouldn't really happen
            {
                LogPrintf("ReadBlockFromDisk failed\n");
                continue;
            }

            CAmount nZeronodePayment = GetZeronodePayment(BlockReading->nHeight, block.vtx[0].GetValueOut());

            BOOST_FOREACH(CTxOut txout, block.vtx[0].vout)
            if (mnpayee == txout.scriptPubKey && nZeronodePayment == txout.nValue) {
                nBlockLastPaid = BlockReading->nHeight;
                nTimeLastPaid = BlockReading->nTime;
                LogPrint("zeronode", "CZeronode::UpdateLastPaidBlock -- searching for block with payment to %s -- found new %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
                return;
            }
        }

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    // Last payment for this zeronode wasn't found in latest mnpayments blocks
    // or it was found in mnpayments blocks but wasn't found in the blockchain.
    // LogPrint("zeronode", "CZeronode::UpdateLastPaidBlock -- searching for block with payment to %s -- keeping old %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
}

bool CZeronodeBroadcast::Create(std::string strService, std::string strKeyZeronode, std::string strTxHash, std::string strOutputIndex, std::string &strErrorRet, CZeronodeBroadcast &mnbRet, bool fOffline) {
    LogPrintf("CZeronodeBroadcast::Create\n");
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyZeronodeNew;
    CKey keyZeronodeNew;
    //need correct blocks to send ping
    if (!fOffline && !zeronodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Zeronode";
        LogPrintf("CZeronodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    //TODO
    if (!darkSendSigner.GetKeysFromSecret(strKeyZeronode, keyZeronodeNew, pubKeyZeronodeNew)) {
        strErrorRet = strprintf("Invalid zeronode key %s", strKeyZeronode);
        LogPrintf("CZeronodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!pwalletMain->GetZeronodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for zeronode %s", strTxHash, strOutputIndex, strService);
        LogPrintf("CZeronodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    CService service = CService(strService);
    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            strErrorRet = strprintf("Invalid port %u for zeronode %s, only %d is supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
            LogPrintf("CZeronodeBroadcast::Create -- %s\n", strErrorRet);
            return false;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for zeronode %s, %d is the only supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
        LogPrintf("CZeronodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    return Create(txin, CService(strService), keyCollateralAddressNew, pubKeyCollateralAddressNew, keyZeronodeNew, pubKeyZeronodeNew, strErrorRet, mnbRet);
}

bool CZeronodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyZeronodeNew, CPubKey pubKeyZeronodeNew, std::string &strErrorRet, CZeronodeBroadcast &mnbRet) {
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("zeronode", "CZeronodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyZeronodeNew.GetID() = %s\n",
             CBitcoinAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
             pubKeyZeronodeNew.GetID().ToString());


    CZeronodePing mnp(txin);
    if (!mnp.Sign(keyZeronodeNew, pubKeyZeronodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, zeronode=%s", txin.prevout.ToStringShort());
        LogPrintf("CZeronodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CZeronodeBroadcast();
        return false;
    }

    mnbRet = CZeronodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyZeronodeNew, PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address, zeronode=%s", txin.prevout.ToStringShort());
        LogPrintf("CZeronodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CZeronodeBroadcast();
        return false;
    }

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, zeronode=%s", txin.prevout.ToStringShort());
        LogPrintf("CZeronodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CZeronodeBroadcast();
        return false;
    }

    return true;
}

bool CZeronodeBroadcast::SimpleCheck(int &nDos) {
    nDos = 0;

    // make sure addr is valid
    if (!IsValidNetAddr()) {
        LogPrintf("CZeronodeBroadcast::SimpleCheck -- Invalid addr, rejected: zeronode=%s  addr=%s\n",
                  vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CZeronodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: zeronode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/unknown blockhash
    if (lastPing == CZeronodePing() || !lastPing.SimpleCheck(nDos)) {
        // one of us is probably forked or smth, just mark it as expired and check the rest of the rules
        nActiveState = ZERONODE_EXPIRED;
    }

    if (nProtocolVersion < mnpayments.GetMinZeronodePaymentsProto()) {
        LogPrintf("CZeronodeBroadcast::SimpleCheck -- ignoring outdated Zeronode: zeronode=%s  nProtocolVersion=%d\n", vin.prevout.ToStringShort(), nProtocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if (pubkeyScript.size() != 25) {
        LogPrintf("CZeronodeBroadcast::SimpleCheck -- pubKeyCollateralAddress has the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyZeronode.GetID());

    if (pubkeyScript2.size() != 25) {
        LogPrintf("CZeronodeBroadcast::SimpleCheck -- pubKeyZeronode has the wrong size\n");
        nDos = 100;
        return false;
    }

    if (!vin.scriptSig.empty()) {
        LogPrintf("CZeronodeBroadcast::SimpleCheck -- Ignore Not Empty ScriptSig %s\n", vin.ToString());
        nDos = 100;
        return false;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != mainnetDefaultPort) return false;
    } else if (addr.GetPort() == mainnetDefaultPort) return false;

    return true;
}

bool CZeronodeBroadcast::Update(CZeronode *pmn, int &nDos) {
    nDos = 0;

    if (pmn->sigTime == sigTime && !fRecovery) {
        // mapSeenZeronodeBroadcast in CZeronodeMan::CheckMnbAndUpdateZeronodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return false;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if (pmn->sigTime > sigTime) {
        LogPrintf("CZeronodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Zeronode %s %s\n",
                  sigTime, pmn->sigTime, vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    pmn->Check();

    // zeronode is banned by PoSe
    if (pmn->IsPoSeBanned()) {
        LogPrintf("CZeronodeBroadcast::Update -- Banned by PoSe, zeronode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // IsVnAssociatedWithPubkey is validated once in CheckOutpoint, after that they just need to match
    if (pmn->pubKeyCollateralAddress != pubKeyCollateralAddress) {
        LogPrintf("CZeronodeBroadcast::Update -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CZeronodeBroadcast::Update -- CheckSignature() failed, zeronode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // if ther was no zeronode broadcast recently or if it matches our Zeronode privkey...
    if (!pmn->IsBroadcastedWithin(ZERONODE_MIN_MNB_SECONDS) || (fZNode && pubKeyZeronode == activeZeronode.pubKeyZeronode)) {
        // take the newest entry
        LogPrintf("CZeronodeBroadcast::Update -- Got UPDATED Zeronode entry: addr=%s\n", addr.ToString());
        if (pmn->UpdateFromNewBroadcast((*this))) {
            pmn->Check();
            RelayZNode();
        }
        zeronodeSync.AddedZeronodeList();
    }

    return true;
}

bool CZeronodeBroadcast::CheckOutpoint(int &nDos) {
    // we are a zeronode with the same vin (i.e. already activated) and this mnb is ours (matches our Zeronode privkey)
    // so nothing to do here for us
    if (fZNode && vin.prevout == activeZeronode.vin.prevout && pubKeyZeronode == activeZeronode.pubKeyZeronode) {
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CZeronodeBroadcast::CheckOutpoint -- CheckSignature() failed, zeronode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            // not mnb fault, let it to be checked again later
            LogPrint("zeronode", "CZeronodeBroadcast::CheckOutpoint -- Failed to aquire lock, addr=%s", addr.ToString());
            mnodeman.mapSeenZeronodeBroadcast.erase(GetHash());
            return false;
        }

        CCoins coins;
        if (!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
            (unsigned int) vin.prevout.n >= coins.vout.size() ||
            coins.vout[vin.prevout.n].IsNull()) {
            LogPrint("zeronode", "CZeronodeBroadcast::CheckOutpoint -- Failed to find Zeronode UTXO, zeronode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if (coins.vout[vin.prevout.n].nValue != ZERONODE_COIN_REQUIRED * COIN) {
            LogPrint("zeronode", "CZeronodeBroadcast::CheckOutpoint -- Zeronode UTXO should have 1000 ZRO, zeronode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if (chainActive.Height() - coins.nHeight + 1 < Params().GetConsensus().nZeronodeMinimumConfirmations) {
            LogPrintf("CZeronodeBroadcast::CheckOutpoint -- Zeronode UTXO must have at least %d confirmations, zeronode=%s\n",
                      Params().GetConsensus().nZeronodeMinimumConfirmations, vin.prevout.ToStringShort());
            // maybe we miss few blocks, let this mnb to be checked again later
            mnodeman.mapSeenZeronodeBroadcast.erase(GetHash());
            return false;
        }
    }

    LogPrint("zeronode", "CZeronodeBroadcast::CheckOutpoint -- Zeronode UTXO verified\n");

    // make sure the vout that was signed is related to the transaction that spawned the Zeronode
    //  - this is expensive, so it's only done once per Zeronode
    if (!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubKeyCollateralAddress)) {
        LogPrintf("CZeronodeMan::CheckOutpoint -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 1000 ZRO tx got nZeronodeMinimumConfirmations
    uint256 hashBlock = uint256();
    CTransaction tx2;
    GetTransaction(vin.prevout.hash, tx2, Params().GetConsensus(), hashBlock, true);
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex *pMNIndex = (*mi).second; // block for 1000 ZRO tx -> 1 confirmation
            CBlockIndex *pConfIndex = chainActive[pMNIndex->nHeight + Params().GetConsensus().nZeronodeMinimumConfirmations - 1]; // block where tx got nZeronodeMinimumConfirmations
            if (pConfIndex->GetBlockTime() > sigTime) {
                LogPrintf("CZeronodeBroadcast::CheckOutpoint -- Bad sigTime %d (%d conf block is at %d) for Zeronode %s %s\n",
                          sigTime, Params().GetConsensus().nZeronodeMinimumConfirmations, pConfIndex->GetBlockTime(), vin.prevout.ToStringShort(), addr.ToString());
                return false;
            }
        }
    }

    return true;
}

bool CZeronodeBroadcast::Sign(CKey &keyCollateralAddress) {
    std::string strError;
    std::string strMessage;

    sigTime = GetAdjustedTime();

    strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                 pubKeyCollateralAddress.GetID().ToString() + pubKeyZeronode.GetID().ToString() +
                 boost::lexical_cast<std::string>(nProtocolVersion);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, keyCollateralAddress)) {
        LogPrintf("CZeronodeBroadcast::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
        LogPrintf("CZeronodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CZeronodeBroadcast::CheckSignature(int &nDos) {
    std::string strMessage;
    std::string strError = "";
    nDos = 0;

    strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                 pubKeyCollateralAddress.GetID().ToString() + pubKeyZeronode.GetID().ToString() +
                 boost::lexical_cast<std::string>(nProtocolVersion);

    LogPrint("zeronode", "CZeronodeBroadcast::CheckSignature -- strMessage: %s  pubKeyCollateralAddress address: %s  sig: %s\n", strMessage, CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString(), EncodeBase64(&vchSig[0], vchSig.size()));

    if (!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
        LogPrintf("CZeronodeBroadcast::CheckSignature -- Got bad Zeronode announce signature, error: %s\n", strError);
        nDos = 100;
        return false;
    }

    return true;
}

void CZeronodeBroadcast::RelayZNode() {
    LogPrintf("CZeronodeBroadcast::RelayZNode\n");
    CInv inv(MSG_ZERONODE_ANNOUNCE, GetHash());
    RelayInv(inv);
}

CZeronodePing::CZeronodePing(CTxIn &vinNew) {
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    vin = vinNew;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
    vchSig = std::vector < unsigned char > ();
}

bool CZeronodePing::Sign(CKey &keyZeronode, CPubKey &pubKeyZeronode) {
    std::string strError;
    std::string strZNodeSignMessage;

    sigTime = GetAdjustedTime();
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, keyZeronode)) {
        LogPrintf("CZeronodePing::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubKeyZeronode, vchSig, strMessage, strError)) {
        LogPrintf("CZeronodePing::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CZeronodePing::CheckSignature(CPubKey &pubKeyZeronode, int &nDos) {
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";
    nDos = 0;

    if (!darkSendSigner.VerifyMessage(pubKeyZeronode, vchSig, strMessage, strError)) {
        LogPrintf("CZeronodePing::CheckSignature -- Got bad Zeronode ping signature, zeronode=%s, error: %s\n", vin.prevout.ToStringShort(), strError);
        nDos = 33;
        return false;
    }
    return true;
}

bool CZeronodePing::SimpleCheck(int &nDos) {
    // don't ban by default
    nDos = 0;

    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CZeronodePing::SimpleCheck -- Signature rejected, too far into the future, zeronode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    {
//        LOCK(cs_main);
        AssertLockHeld(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint("zeronode", "CZeronodePing::SimpleCheck -- Zeronode ping is invalid, unknown block hash: zeronode=%s blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
    }
    LogPrint("zeronode", "CZeronodePing::SimpleCheck -- Zeronode ping verified: zeronode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);
    return true;
}

bool CZeronodePing::CheckAndUpdate(CZeronode *pmn, bool fFromNewBroadcast, int &nDos) {
    // don't ban by default
    nDos = 0;

    if (!SimpleCheck(nDos)) {
        return false;
    }

    if (pmn == NULL) {
        LogPrint("zeronode", "CZeronodePing::CheckAndUpdate -- Couldn't find Zeronode entry, zeronode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    if (!fFromNewBroadcast) {
        if (pmn->IsUpdateRequired()) {
            LogPrint("zeronode", "CZeronodePing::CheckAndUpdate -- zeronode protocol is outdated, zeronode=%s\n", vin.prevout.ToStringShort());
            return false;
        }

        if (pmn->IsNewStartRequired()) {
            LogPrint("zeronode", "CZeronodePing::CheckAndUpdate -- zeronode is completely expired, new start is required, zeronode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            LogPrintf("CZeronodePing::CheckAndUpdate -- Zeronode ping is invalid, block hash is too old: zeronode=%s  blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // nDos = 1;
            return false;
        }
    }

    LogPrint("zeronode", "CZeronodePing::CheckAndUpdate -- New ping: zeronode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);

    // LogPrintf("mnping - Found corresponding mn for vin: %s\n", vin.prevout.ToStringShort());
    // update only if there is no known ping for this zeronode or
    // last ping was more then ZERONODE_MIN_MNP_SECONDS-60 ago comparing to this one
    if (pmn->IsPingedWithin(ZERONODE_MIN_MNP_SECONDS - 60, sigTime)) {
        LogPrint("zeronode", "CZeronodePing::CheckAndUpdate -- Zeronode ping arrived too early, zeronode=%s\n", vin.prevout.ToStringShort());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pmn->pubKeyZeronode, nDos)) return false;

    // so, ping seems to be ok

    // if we are still syncing and there was no known ping for this mn for quite a while
    // (NOTE: assuming that ZERONODE_EXPIRATION_SECONDS/2 should be enough to finish mn list sync)
    if (!zeronodeSync.IsZeronodeListSynced() && !pmn->IsPingedWithin(ZERONODE_EXPIRATION_SECONDS / 2)) {
        // let's bump sync timeout
        LogPrint("zeronode", "CZeronodePing::CheckAndUpdate -- bumping sync timeout, zeronode=%s\n", vin.prevout.ToStringShort());
        zeronodeSync.AddedZeronodeList();
    }

    // let's store this ping as the last one
    LogPrint("zeronode", "CZeronodePing::CheckAndUpdate -- Zeronode ping accepted, zeronode=%s\n", vin.prevout.ToStringShort());
    pmn->lastPing = *this;

    // and update mnodeman.mapSeenZeronodeBroadcast.lastPing which is probably outdated
    CZeronodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if (mnodeman.mapSeenZeronodeBroadcast.count(hash)) {
        mnodeman.mapSeenZeronodeBroadcast[hash].second.lastPing = *this;
    }

    pmn->Check(true); // force update, ignoring cache
    if (!pmn->IsEnabled()) return false;

    LogPrint("zeronode", "CZeronodePing::CheckAndUpdate -- Zeronode ping acceepted and relayed, zeronode=%s\n", vin.prevout.ToStringShort());
    Relay();

    return true;
}

void CZeronodePing::Relay() {
    CInv inv(MSG_ZERONODE_PING, GetHash());
    RelayInv(inv);
}

//void CZeronode::AddGovernanceVote(uint256 nGovernanceObjectHash)
//{
//    if(mapGovernanceObjectsVotedOn.count(nGovernanceObjectHash)) {
//        mapGovernanceObjectsVotedOn[nGovernanceObjectHash]++;
//    } else {
//        mapGovernanceObjectsVotedOn.insert(std::make_pair(nGovernanceObjectHash, 1));
//    }
//}

//void CZeronode::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
//{
//    std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.find(nGovernanceObjectHash);
//    if(it == mapGovernanceObjectsVotedOn.end()) {
//        return;
//    }
//    mapGovernanceObjectsVotedOn.erase(it);
//}

void CZeronode::UpdateWatchdogVoteTime() {
    LOCK(cs);
    nTimeLastWatchdogVote = GetTime();
}

/**
*   FLAG GOVERNANCE ITEMS AS DIRTY
*
*   - When zeronode come and go on the network, we must flag the items they voted on to recalc it's cached flags
*
*/
//void CZeronode::FlagGovernanceItemsAsDirty()
//{
//    std::vector<uint256> vecDirty;
//    {
//        std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.begin();
//        while(it != mapGovernanceObjectsVotedOn.end()) {
//            vecDirty.push_back(it->first);
//            ++it;
//        }
//    }
//    for(size_t i = 0; i < vecDirty.size(); ++i) {
//        mnodeman.AddDirtyGovernanceObjectHash(vecDirty[i]);
//    }
//}
