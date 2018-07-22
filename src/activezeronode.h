// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEZERONODE_H
#define ACTIVEZERONODE_H

#include "net.h"
#include "key.h"
#include "wallet/wallet.h"

class CActiveZeronode;

static const int ACTIVE_ZERONODE_INITIAL          = 0; // initial state
static const int ACTIVE_ZERONODE_SYNC_IN_PROCESS  = 1;
static const int ACTIVE_ZERONODE_INPUT_TOO_NEW    = 2;
static const int ACTIVE_ZERONODE_NOT_CAPABLE      = 3;
static const int ACTIVE_ZERONODE_STARTED          = 4;

extern CActiveZeronode activeZeronode;

// Responsible for activating the Zeronode and pinging the network
class CActiveZeronode
{
public:
    enum zeronode_type_enum_t {
        ZERONODE_UNKNOWN = 0,
        ZERONODE_REMOTE  = 1,
        ZERONODE_LOCAL   = 2
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    zeronode_type_enum_t eType;

    bool fPingerEnabled;

    /// Ping Zeronode
    bool SendZeronodePing();

public:
    // Keys for the active Zeronode
    CPubKey pubKeyZeronode;
    CKey keyZeronode;

    // Initialized while registering Zeronode
    CTxIn vin;
    CService service;

    int nState; // should be one of ACTIVE_ZERONODE_XXXX
    std::string strNotCapableReason;

    CActiveZeronode()
        : eType(ZERONODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeyZeronode(),
          keyZeronode(),
          vin(),
          service(),
          nState(ACTIVE_ZERONODE_INITIAL)
    {}

    /// Manage state of active Zeronode
    void ManageState();

    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;

private:
    void ManageStateInitial();
    void ManageStateRemote();
    void ManageStateLocal();
};

#endif
