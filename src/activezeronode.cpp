// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activezeronode.h"
#include "zeronode.h"
#include "zeronode-sync.h"
#include "zeronodeman.h"
#include "protocol.h"

extern CWallet *pwalletMain;

// Keep track of the active Zeronode
CActiveZeronode activeZeronode;

void CActiveZeronode::ManageState() {
    LogPrint("zeronode", "CActiveZeronode::ManageState -- Start\n");
    if (!fZNode) {
        LogPrint("zeronode", "CActiveZeronode::ManageState -- Not a zeronode, returning\n");
        return;
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && !zeronodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_ZERONODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveZeronode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if (nState == ACTIVE_ZERONODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_ZERONODE_INITIAL;
    }

    LogPrint("zeronode", "CActiveZeronode::ManageState -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    if (eType == ZERONODE_UNKNOWN) {
        ManageStateInitial();
    }

    if (eType == ZERONODE_REMOTE) {
        ManageStateRemote();
    } else if (eType == ZERONODE_LOCAL) {
        // Try Remote Start first so the started local zeronode can be restarted without recreate zeronode broadcast.
        ManageStateRemote();
        if (nState != ACTIVE_ZERONODE_STARTED)
            ManageStateLocal();
    }

    SendZeronodePing();
}

std::string CActiveZeronode::GetStateString() const {
    switch (nState) {
        case ACTIVE_ZERONODE_INITIAL:
            return "INITIAL";
        case ACTIVE_ZERONODE_SYNC_IN_PROCESS:
            return "SYNC_IN_PROCESS";
        case ACTIVE_ZERONODE_INPUT_TOO_NEW:
            return "INPUT_TOO_NEW";
        case ACTIVE_ZERONODE_NOT_CAPABLE:
            return "NOT_CAPABLE";
        case ACTIVE_ZERONODE_STARTED:
            return "STARTED";
        default:
            return "UNKNOWN";
    }
}

std::string CActiveZeronode::GetStatus() const {
    switch (nState) {
        case ACTIVE_ZERONODE_INITIAL:
            return "Node just started, not yet activated";
        case ACTIVE_ZERONODE_SYNC_IN_PROCESS:
            return "Sync in progress. Must wait until sync is complete to start Zeronode";
        case ACTIVE_ZERONODE_INPUT_TOO_NEW:
            return strprintf("Zeronode input must have at least %d confirmations",
                             Params().GetConsensus().nZeronodeMinimumConfirmations);
        case ACTIVE_ZERONODE_NOT_CAPABLE:
            return "Not capable zeronode: " + strNotCapableReason;
        case ACTIVE_ZERONODE_STARTED:
            return "Zeronode successfully started";
        default:
            return "Unknown";
    }
}

std::string CActiveZeronode::GetTypeString() const {
    std::string strType;
    switch (eType) {
        case ZERONODE_UNKNOWN:
            strType = "UNKNOWN";
            break;
        case ZERONODE_REMOTE:
            strType = "REMOTE";
            break;
        case ZERONODE_LOCAL:
            strType = "LOCAL";
            break;
        default:
            strType = "UNKNOWN";
            break;
    }
    return strType;
}

bool CActiveZeronode::SendZeronodePing() {
    if (!fPingerEnabled) {
        LogPrint("zeronode",
                 "CActiveZeronode::SendZeronodePing -- %s: zeronode ping service is disabled, skipping...\n",
                 GetStateString());
        return false;
    }

    if (!mnodeman.Has(vin)) {
        strNotCapableReason = "Zeronode not in zeronode list";
        nState = ACTIVE_ZERONODE_NOT_CAPABLE;
        LogPrintf("CActiveZeronode::SendZeronodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CZeronodePing mnp(vin);
    if (!mnp.Sign(keyZeronode, pubKeyZeronode)) {
        LogPrintf("CActiveZeronode::SendZeronodePing -- ERROR: Couldn't sign Zeronode Ping\n");
        return false;
    }

    // Update lastPing for our zeronode in Zeronode list
    if (mnodeman.IsZeronodePingedWithin(vin, ZERONODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveZeronode::SendZeronodePing -- Too early to send Zeronode Ping\n");
        return false;
    }

    mnodeman.SetZeronodeLastPing(vin, mnp);

    LogPrintf("CActiveZeronode::SendZeronodePing -- Relaying ping, collateral=%s\n", vin.ToString());
    mnp.Relay();

    return true;
}

void CActiveZeronode::ManageStateInitial() {
    LogPrint("zeronode", "CActiveZeronode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_ZERONODE_NOT_CAPABLE;
        strNotCapableReason = "Zeronode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveZeronode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    bool fFoundLocal = false;
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CZeronode::IsValidNetAddr(service);
        if (!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                nState = ACTIVE_ZERONODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveZeronode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode * pnode, vNodes)
            {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CZeronode::IsValidNetAddr(service);
                    if (fFoundLocal) break;
                }
            }
        }
    }

    if (!fFoundLocal) {
        nState = ACTIVE_ZERONODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveZeronode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_ZERONODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(),
                                            mainnetDefaultPort);
            LogPrintf("CActiveZeronode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_ZERONODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(),
                                        mainnetDefaultPort);
        LogPrintf("CActiveZeronode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveZeronode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());
    //TODO
    if (!ConnectNode(CAddress(service, NODE_NETWORK), NULL, false, true)) {
        nState = ACTIVE_ZERONODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveZeronode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = ZERONODE_REMOTE;

    // Check if wallet funds are available
    if (!pwalletMain) {
        LogPrintf("CActiveZeronode::ManageStateInitial -- %s: Wallet not available\n", GetStateString());
        return;
    }

    if (pwalletMain->IsLocked()) {
        LogPrintf("CActiveZeronode::ManageStateInitial -- %s: Wallet is locked\n", GetStateString());
        return;
    }

    if (pwalletMain->GetBalance() < ZERONODE_COIN_REQUIRED * COIN) {
        LogPrintf("CActiveZeronode::ManageStateInitial -- %s: Wallet balance is < 1000 ZRO\n", GetStateString());
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    // If collateral is found switch to LOCAL mode
    if (pwalletMain->GetZeronodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        eType = ZERONODE_LOCAL;
    }

    LogPrint("zeronode", "CActiveZeronode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveZeronode::ManageStateRemote() {
    LogPrint("zeronode",
             "CActiveZeronode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyZeronode.GetID() = %s\n",
             GetStatus(), fPingerEnabled, GetTypeString(), pubKeyZeronode.GetID().ToString());

    mnodeman.CheckZeronode(pubKeyZeronode);
    zeronode_info_t infoMn = mnodeman.GetZeronodeInfo(pubKeyZeronode);
    if (infoMn.fInfoValid) {
        if (infoMn.nProtocolVersion != PROTOCOL_VERSION) {
            nState = ACTIVE_ZERONODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveZeronode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (service != infoMn.addr) {
            nState = ACTIVE_ZERONODE_NOT_CAPABLE;
            // LogPrintf("service: %s\n", service.ToString());
            // LogPrintf("infoMn.addr: %s\n", infoMn.addr.ToString());
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this zeronode changed recently.";
            LogPrintf("CActiveZeronode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (!CZeronode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            nState = ACTIVE_ZERONODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Zeronode in %s state", CZeronode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveZeronode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (nState != ACTIVE_ZERONODE_STARTED) {
            LogPrintf("CActiveZeronode::ManageStateRemote -- STARTED!\n");
            vin = infoMn.vin;
            service = infoMn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_ZERONODE_STARTED;
        }
    } else {
        nState = ACTIVE_ZERONODE_NOT_CAPABLE;
        strNotCapableReason = "Zeronode not in zeronode list";
        LogPrintf("CActiveZeronode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}

void CActiveZeronode::ManageStateLocal() {
    LogPrint("zeronode", "CActiveZeronode::ManageStateLocal -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
    if (nState == ACTIVE_ZERONODE_STARTED) {
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    if (pwalletMain->GetZeronodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge < Params().GetConsensus().nZeronodeMinimumConfirmations) {
            nState = ACTIVE_ZERONODE_INPUT_TOO_NEW;
            strNotCapableReason = strprintf(_("%s - %d confirmations"), GetStatus(), nInputAge);
            LogPrintf("CActiveZeronode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->LockCoin(vin.prevout);
        }

        CZeronodeBroadcast mnb;
        std::string strError;
        if (!CZeronodeBroadcast::Create(vin, service, keyCollateral, pubKeyCollateral, keyZeronode,
                                     pubKeyZeronode, strError, mnb)) {
            nState = ACTIVE_ZERONODE_NOT_CAPABLE;
            strNotCapableReason = "Error creating mastenode broadcast: " + strError;
            LogPrintf("CActiveZeronode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        fPingerEnabled = true;
        nState = ACTIVE_ZERONODE_STARTED;

        //update to zeronode list
        LogPrintf("CActiveZeronode::ManageStateLocal -- Update Zeronode List\n");
        mnodeman.UpdateZeronodeList(mnb);
        mnodeman.NotifyZeronodeUpdates();

        //send to all peers
        LogPrintf("CActiveZeronode::ManageStateLocal -- Relay broadcast, vin=%s\n", vin.ToString());
        mnb.RelayZNode();
    }
}
