// Copyright (c) 2016-2018 The CellLink developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BRANCHDB_H
#define BITCOIN_BRANCHDB_H

#include "chain.h"
#include "io/dbwrapper.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

//the state of report and prove
const uint16_t RP_FLAG_REPORTED = 1;//被举报了
const uint16_t RP_FLAG_PROVED = 2;//已证明

class BranchBlockData
{
public:
    BranchBlockData();

    typedef std::vector<uint256> VEC_HASH;
    typedef std::map<uint256, uint16_t> MAP_REPORT_PROVE;
    enum {//dead status
        eLive = 0,
        eDeadSelf = 1<<0,
        eDeadInherit = 1<<1,
    };

    CellBlockHeader header; // 侧链spv
    int32_t nHeight;     // 侧链块高度
    CellTransactionRef pStakeTx;

    // mBlockHash 和 txIndex 为了查找到原来交易
    uint256 mBlockHash; // 主链打包块的hash
    uint256 txHash; // 侧链向主链发送侧链块spv的交易hash
    int txIndex;    // 交易在主链打包块的index

    arith_uint256 nChainWork;// 包含全部祖先和自己的work
    VEC_HASH vecSonHashs;// 子block hash,make a block tree chain
    unsigned char deadstatus;
    MAP_REPORT_PROVE mapReportStatus;// deadstatus需要知道当前block的举报状态，BranchDb的mReortTxFlag差不了，或者需要改成多key容器
                                  // 而且放这里比mReortTxFlag更有优势吧，数据分散到每个block，而不是集中起来。集中会导致某个数据
                                  // 数据过大。
    bool IsDead();//是否在被举报状态

    void InitDataFromTx(const CellTransaction& tx);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(header);
        READWRITE(nHeight);
        READWRITE(pStakeTx);
        READWRITE(mBlockHash);
        READWRITE(txHash);
        READWRITE(txIndex);

        READWRITE(nChainWork);
        READWRITE(vecSonHashs);
        READWRITE(deadstatus);
        READWRITE(mapReportStatus);
    }

    enum {
        eADD,
        eDELETE,
    };
    unsigned char flags; // memory only
};

typedef std::vector<uint256> VBRANCH_CHAIN;
typedef std::map<uint256, BranchBlockData> MAPBRANCH_HEADERS;
typedef std::map<uint256, uint256> MAP_MAINBLOCK_BRANCHTIP;
//
class BranchData
{
public:
    MAPBRANCH_HEADERS mapHeads;
    VBRANCH_CHAIN vecChainActive;
    MAP_MAINBLOCK_BRANCHTIP mapSnapshotBlockTip; // record connected main block, each branch tip

    void AddNewBlockData(BranchBlockData& blockdata);
    void ActivateBestChain(const uint256 &bestTipHash);
    void RemoveBlock(const uint256& blockhash);

    //
    void UpdateDeadStatus(const uint256& blockId, bool &fStatusChange);
    void DeadTransmit(const uint256& blockId);
    //
    void UpdateRebornStatus(const uint256& blockId, bool &fStatusChange);
    void RebornTransmit(const uint256& blockId);
    // 
    uint256 FindBestTipBlock();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(mapHeads);
        READWRITE(vecChainActive);
        READWRITE(mapSnapshotBlockTip);
    }

    void InitBranchGenesisBlockData(const uint256 &branchid);
    void SnapshotBlockTip(const uint256& mainBlockHash);
    void RecoverTip(const uint256& mainBlockHash);

    uint256 TipHash(void);
    uint32_t Height(void);
    bool IsBlockInBestChain(const uint256& blockhash);
    int GetBlockMinedHeight(const uint256& blockhash);

private:
    void FindBestBlock(const uint256& blockhash, uint256& mostworkblock);
};

typedef std::map<uint256, BranchData> MAPBRANCHS_DATA;

class BranchCache
{
public:
    MAPBRANCHS_DATA mapBranchCache;
    std::map<uint256, uint16_t> mReortTxFlagCache;

    bool HasInCache(const CellTransaction& tx);
    void AddToCache(const CellTransaction& tx);
    
    void RemoveFromBlock(const std::vector<CellTransactionRef>& vtx);

    std::vector<uint256> GetAncestorsBlocksHash(const CellTransaction& tx);

    const BranchBlockData* GetBranchBlockData(const uint256 &branchhash, const uint256 &blockhash);

private:
    void RemoveFromCache(const CellTransaction& tx);
};

/*
 1、保证每个BranchData的mapHeads的BranchBlockData的preblock数据是存在的。
    也就是每个数据都有完整的到达创世块的链路径
 */

class BranchDb
{
public:
    BranchDb() = delete;
    BranchDb(const BranchDb&) = delete;
    BranchDb(const fs::path& path, size_t nCacheSize, bool fMemory, bool fWipe);
    BranchDb& operator=(const BranchDb&) = delete;

    // flush data in connectblock and disconnnectblock, add or remove data.
    void Flush(const std::shared_ptr<const CellBlock>& pblock, bool fConnect);
    void OnConnectBlock(const std::shared_ptr<const CellBlock>& pblock);
    void OnDisconnectBlock(const std::shared_ptr<const CellBlock>& pblock);

    void AddBlockInfoTxData(CellTransactionRef &transaction, const uint256 &mainBlockHash, const size_t iTxVtxIndex, std::set<uint256>& modifyBranch);
    void DelBlockInfoTxData(CellTransactionRef &transaction, const uint256 &mainBlockHash, const size_t iTxVtxIndex, std::set<uint256>& modifyBranch);

    void LoadData();

    uint256 GetBranchTipHash(const uint256& branchid);
    uint32_t GetBranchHeight(const uint256& branchid);
public:
    std::map<uint256, uint16_t> mReortTxFlag;

    bool HasBranchData(const uint256& branchHash) const
    {
        return mapBranchsData.count(branchHash) > 0;
    }
    BranchData GetBranchData(const uint256& branchHash);
    bool IsBlockInActiveChain(const uint256& branchHash, const uint256& blockHash);
    int GetBranchBlockMinedHeight(const uint256& branchHash, const uint256& blockHash);
private:
    bool WriteModifyToDB(const std::set<uint256>& modifyBranch);

    CellDBWrapper db;
    MAPBRANCHS_DATA mapBranchsData;
};

extern BranchDb* pBranchDb;

//branch mempool tx cache data
extern BranchCache branchDataMemCache;

#endif // BITCOIN_BRANCHDB_H