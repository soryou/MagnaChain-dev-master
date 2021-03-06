// Copyright (c) 2017-2017 The Bitcoin Core developers
// Copyright (c) 2016-2019 The MagnaChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "consensus/tx_verify.h"

#include "consensus/consensus.h"
#include "primitives/transaction.h"
#include "script/interpreter.h"
#include "consensus/validation.h"

// TODO remove the following dependencies
#include "chain/chain.h"
#include "transaction/coins.h"
#include "utils/utilmoneystr.h"
 
#include "chain/branchchain.h"
#include "chain/branchtxdb.h"
#include "io/core_io.h"
#include "chain/chainparams.h"
#include "rpc/branchchainrpc.h"
#include "utils/util.h"
#include "utils/utilstrencodings.h"
#include "transaction/txdb.h"
#include "coding/base58.h"
#include "smartcontract/smartcontract.h"

#include "chain/branchdb.h"

extern bool IsCoinBranchTranScript(const MCScript& script);

bool IsFinalTx(const MCTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    for (const auto& txin : tx.vin) {
        if (!(txin.nSequence == MCTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

std::pair<int, int64_t> CalculateSequenceLocks(const MCTransaction &tx, int flags, std::vector<int>* prevHeights, const MCBlockIndex& block)
{
    assert(prevHeights->size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2
                      && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const MCTxIn& txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & MCTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            (*prevHeights)[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = (*prevHeights)[txinIndex];

        if (txin.nSequence & MCTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight-1, 0))->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & MCTxIn::SEQUENCE_LOCKTIME_MASK) << MCTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        } else {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & MCTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

bool EvaluateSequenceLocks(const MCBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const MCTransaction &tx, int flags, std::vector<int>* prevHeights, const MCBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

unsigned int GetLegacySigOpCount(const MCTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto& txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const MCTransaction& tx, const MCCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const MCTxOut &prevout = coin.out;
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const MCTransaction& tx, const MCCoinsViewCache& inputs, int flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx) * WITNESS_SCALE_FACTOR;

    if (tx.IsCoinBase())
        return nSigOps;
	if (tx.IsBranchChainTransStep2())
		return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs) * WITNESS_SCALE_FACTOR;
    }

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const MCTxOut &prevout = coin.out;
        nSigOps += CountWitnessSigOps(tx.vin[i].scriptSig, prevout.scriptPubKey, &tx.vin[i].scriptWitness, flags);
    }
    return nSigOps;
}

bool CheckTransaction(const MCTransaction& tx, MCValidationState &state, bool fCheckDuplicateInputs, const MCBlock* pBlock, 
    const MCBlockIndex* pBlockIndex, const bool fVerifingDB, BranchCache *pBranchCache)
{
    if (tx.IsSyncBranchInfo())
    {
        if (!Params().IsMainChain())
            return state.DoS(100, false, REJECT_INVALID, "Branch chain can not accept branch head transaction");
        if (!CheckBranchBlockInfoTx(tx, state, pBranchCache))
            return false;
    }
    // Basic checks that don't depend on any context
    if (tx.vin.empty())
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vin-empty");
    if (tx.vout.empty() && !tx.IsSmartContract())
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vout-empty");
    // Size limits (this doesn't take the witness into account, as that hasn't been checked for malleability)
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) * WITNESS_SCALE_FACTOR > MAX_BLOCK_WEIGHT)
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-oversize");

    // Check for negative or overflow output values
    MCAmount nValueOut = 0;
    for (const auto& txout : tx.vout)
    {
        if (txout.nValue < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-vout-toolarge");
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-txouttotal-toolarge");
    }

    // Check for duplicate inputs - note that this check is slow so we skip it in CheckBlock
    if (fCheckDuplicateInputs) {
        std::set<MCOutPoint> vInOutPoints;
        for (const auto& txin : tx.vin)
        {
            if (!vInOutPoints.insert(txin.prevout).second)
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputs-duplicate");
        }
    }
	if (tx.nVersion == MCTransaction::PUBLISH_CONTRACT_VERSION)
    {
		if (GenerateContractAddressByTx(tx) != tx.pContractData->address)
		{
			return state.DoS(100, false, REJECT_INVALID, "bad-contract-address");
		}
	}
    // other transaction type extra check 
    if (tx.IsBranchCreate())//branch create check
    {
        if (!Params().IsMainChain())
        {
            return state.DoS(100, false, REJECT_INVALID, "only main chain can create branch chain.");
        }
        if (GetBranchChainCreateTxOut(tx) < GetCreateBranchMortgage(pBlock, pBlockIndex))
        {
            LogPrintf("check transaction for create branch mortgage fail, txid %s\n", tx.GetHash().ToString());
            return state.DoS(10, false, REJECT_INVALID, std::string("create branch mortgage coin error"));
        }
    }
    if (tx.IsPregnantTx()) //对输入输出的数量进行判断
    {
        MCAmount nTransChainOut = GetBranchChainOut(tx);
		MCMutableTransaction mtxTrans2;
		if (!DecodeHexTx(mtxTrans2, tx.sendToTxHexData))
		{
			return state.DoS(100, false, REJECT_INVALID, "bad-chain-transaction-step1");
		}
		if (mtxTrans2.IsBranchChainTransStep2() == false)
		{
			return state.DoS(100, false, REJECT_INVALID, "bad-transaction-step2-data");
		}
		MCAmount nAmountOut2 = 0;
		for (const auto& txout : mtxTrans2.vout)
		{
			if (!MoneyRange(txout.nValue))
				return state.DoS(100, false, REJECT_INVALID, "bad-tx2-txout-out-of-range");
			nAmountOut2 += txout.nValue;
			if (!MoneyRange(nAmountOut2))
				return state.DoS(100, false, REJECT_INVALID, "bad-tx2-txout-total-out-of-range");
		}

        if (nAmountOut2 >= nTransChainOut || mtxTrans2.inAmount != nTransChainOut)
        {
            return state.DoS(100, false, REJECT_INVALID, "bad-amount-not-correct-t1-t2");
        }
        // IsMortgage 抵押币out,主侧链的数量、大小需要一致
        if (tx.IsMortgage())
        {
            if (!Params().IsMainChain()){
                return state.DoS(100, false, REJECT_INVALID, "Only main chain can mortgage.");
            }
            if (tx.vout.size() > 3){
                return state.DoS(100, false, REJECT_INVALID, "Mortgage mine coin vout size invalid.");
            }
            uint256 branchHash;
            MCKeyID keyid;
            int64_t coinheight;
            if (GetMortgageMineData(tx.vout[0].scriptPubKey, &branchHash, &keyid, &coinheight) == false){
                return state.DoS(100, false, REJECT_INVALID, "Mortgage mine coin vout script invalid.");
            }
            if (branchHash.GetHex() != tx.sendToBranchid){
                return state.DoS(100, false, REJECT_INVALID, "Mortgage tx sendToBranchid and mortgage branchid not match.");
            }
            if (coinheight < 0)
                return state.DoS(100, false, REJECT_INVALID, "Mortgage coin height invalid.");
            if (mtxTrans2.vout.size() != 1 || !mtxTrans2.vout[0].scriptPubKey.empty()){
                return state.DoS(100, false, REJECT_INVALID, "Mortgage tx invalid trans2 vout");
            }
            if (GetMortgageMineOut(tx, false) != mtxTrans2.vout[0].nValue) {//GetMortgageCoinOut(mtxTrans2, false)
                return state.DoS(100, false, REJECT_INVALID, "Mortgage mine coin main chain and branch chain is not same.");
            }
        }
    }
    if (tx.IsRedeemMortgageStatement())
    {
        if(Params().IsMainChain())
            return state.DoS(100, false, REJECT_INVALID, "Redeem mortgage statement tx only in branch chain.");
    }
    if (tx.IsRedeemMortgage()) {
        if (!Params().IsMainChain())
            return state.DoS(100, false, REJECT_INVALID, "Redeem mortgage tx only in main chain.");
    }
    MCTransactionRef pFromTx;
    if (tx.IsBranchChainTransStep2() || tx.IsRedeemMortgage())
    {
        MCDataStream cds(tx.fromTx, SER_NETWORK, INIT_PROTO_VERSION);
        cds >> (pFromTx);
        if (tx.fromBranchId != MCBaseChainParams::MAIN) {
            //spv check
            uint256 frombranchid = uint256S(tx.fromBranchId);
            if (!pBranchCache->HasBranchData(frombranchid))
                return state.DoS(0, false, REJECT_INVALID, strprintf("CheckTransaction branchid error. %s", tx.fromBranchId));
            BranchData branchdata = pBranchCache->GetBranchData(frombranchid);

            MCSpvProof spvProof(*tx.pPMT);
            BranchBlockData* pBlockData = branchdata.GetBranchBlockData(spvProof.blockhash);
            if (pBlockData == nullptr)
                return false;
            if (CheckSpvProof(pBlockData->header.hashMerkleRoot, spvProof.pmt, pFromTx->GetHash()) < 0)
                return false;

            // best chain check
            if (!pBranchCache->IsBlockInActiveChain(frombranchid, tx.pPMT->blockhash))
                return state.DoS(1, false, REJECT_INVALID, "Branch-tx-not in best chain");
            int minedHeight = pBranchCache->GetBranchBlockMinedHeight(frombranchid, tx.pPMT->blockhash);
            if (minedHeight < BRANCH_CHAIN_MATURITY)
                return state.DoS(1, false, REJECT_INVALID, strprintf("branch-tx minedHeight %d is lessthan %d", minedHeight, BRANCH_CHAIN_MATURITY));
        }
    }
    if (tx.IsBranchChainTransStep2())
    {
        MCAmount nOrginalOut = nValueOut;
        if (tx.fromBranchId != MCBaseChainParams::MAIN){
            nOrginalOut = 0;// recalc exclude branch tran recharge
            for (const auto& txout : tx.vout){
                if (!IsCoinBranchTranScript(txout.scriptPubKey)){
                    nOrginalOut += txout.nValue;
                }
            }
        }
        if (nOrginalOut >= tx.inAmount || !MoneyRange(tx.inAmount)){
            return state.DoS(100, false, REJECT_INVALID, "bad-amount-not-correct-t1-t2");
        }
        
        //挖矿币产生输出判断
        if (!Params().IsMainChain() && QuickGetBranchScriptType(tx.vout[0].scriptPubKey) == BST_MORTGAGE_COIN)
        {
            uint256 coinfromtxid;
            MCKeyID coinkeyid;
            if (GetMortgageCoinData(tx.vout[0].scriptPubKey, &coinfromtxid, &coinkeyid) == false)
            {
                return state.DoS(100, false, REJECT_INVALID, "Invalid mortgage coin out");
            }
            if (pFromTx == nullptr || coinfromtxid != pFromTx->GetHash())
            {
                return state.DoS(100, false, REJECT_INVALID, "Invalid mortgage coin out,from txid");
            }
        }
        if (CheckBranchTransaction(tx, state, fVerifingDB, pFromTx) == false) {//OP: this call may be optimization point
            return false;
        }
        //IsMortgage child
        if (tx.vout.size() == 1 && QuickGetBranchScriptType(tx.vout[0].scriptPubKey) == BST_MORTGAGE_MINE)
        {
            if (tx.vout[0].nValue < MIN_MINE_BRANCH_MORTGAGE)
            {
                return state.Invalid(false, REJECT_INVALID, "mortgage vout nValue is not satisfy min mortgage");
            }
        }
    }
    if (tx.IsReport()){
        if (!Params().IsMainChain())
            return state.DoS(100, false, REJECT_INVALID, "Report tx must in main chain.");
        if (!CheckReportCheatTx(tx, state, pBranchCache)){
            return false;
        }
    }
    if (tx.IsProve()){
        if (!Params().IsMainChain())
            return state.DoS(100, false, REJECT_INVALID, "Prove tx must in main chain.");
        if (!CheckProveTx(tx, state, pBranchCache)){
            return false;
        }
    }
    if (tx.IsLockMortgageMineCoin()){
        if (!CheckLockMortgageMineCoinTx(tx, state))
            return false;
    }
    if (tx.IsUnLockMortgageMineCoin()) {
        if (!CheckUnlockMortgageMineCoinTx(tx, state))
            return false;
    }


    if (tx.IsCoinBase())
    {
        if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 200)
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-length");
    }
    else
    {
        for (const auto& txin : tx.vin)
            if (txin.prevout.IsNull())
                return state.DoS(10, false, REJECT_INVALID, "bad-txns-prevout-null");
    }

    return true;
}

extern bool CheckTranBranchScript(uint256 branchid, const MCScript& scriptPubKey);

bool Consensus::CheckTxInputs(const MCTransaction& tx, MCValidationState& state, const MCCoinsViewCache& inputs, int nSpendHeight)
{
        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
        if (!inputs.HaveInputs(tx))
            return state.Invalid(false, 0, "", "Inputs unavailable");

        MCAmount nMortgageCoin = 0;
        int nCountMortgageCoin = 0;
        uint256 mortgageFromTxid;

        MCAmount nValueIn = 0;
        MCAmount nFees = 0;
        MCAmount nContractAmountIn = 0;
        MCScript contractScript;
        if (tx.nVersion == MCTransaction::PUBLISH_CONTRACT_VERSION || tx.nVersion == MCTransaction::CALL_CONTRACT_VERSION)
            contractScript = GetScriptForDestination(tx.pContractData->address);

        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            const MCOutPoint &prevout = tx.vin[i].prevout;
            const Coin& coin = inputs.AccessCoin(prevout);
            assert(!coin.IsSpent());

            // If prev is coinbase, check that it's matured
            if (coin.IsCoinBase()) {
                if (nSpendHeight - coin.nHeight < COINBASE_MATURITY)
                    return state.Invalid(false,
                        REJECT_INVALID, "bad-txns-premature-spend-of-coinbase",
                        strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
            }
            if (coin.IsCoinCreateBranch()){
                if (nSpendHeight - coin.nHeight < BRANCH_CHAIN_CREATE_COIN_MATURITY){
                    return state.Invalid(false,
                        REJECT_INVALID, "bad-txns-premature-spend-of-coincreatebranch",
                        strprintf("tried to spend coincreatebranch at depth %d", nSpendHeight - coin.nHeight));
                }
            }

            // Check for negative or overflow input values
            nValueIn += coin.out.nValue;
            if (!MoneyRange(coin.out.nValue) || !MoneyRange(nValueIn))
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-outofrange");

            if (tx.vin[i].scriptSig.IsContract()) {
                if (tx.vin[i].scriptSig != contractScript)
                    return state.DoS(100, false, REJECT_INVALID, "bad_contract_pub_key");
                nContractAmountIn += coin.out.nValue;
            }

            // Check input types
            branch_script_type bst = QuickGetBranchScriptType(coin.out.scriptPubKey);
            if ((bst == BST_MORTGAGE_MINE && !tx.IsRedeemMortgage() && !tx.IsBranchChainTransStep2())) // 除了赎回抵押币
                return state.DoS(100, error("Can not use mortgage coin in not-stake transaction"));
            if ((bst == BST_MORTGAGE_COIN && !tx.IsRedeemMortgageStatement() && !tx.IsStake())) // 除了声明赎回挖矿币,挖矿时可以使用挖矿币
                return state.DoS(100, error("Can not use mortgage coin in not-stake transaction"));

            if (tx.IsRedeemMortgageStatement()){
                if (GetMortgageCoinData(coin.out.scriptPubKey, &mortgageFromTxid)){
                    nMortgageCoin += coin.out.nValue;
                    nCountMortgageCoin++;
                    if (nCountMortgageCoin > 1)
                        return state.DoS(100, false, REJECT_INVALID, "more-than-one-mortgage-coin", false, "Just one mortgage coin in, one redeem coin out");

                    if (nSpendHeight - coin.nHeight < REDEEM_SAFE_HEIGHT) // 挖矿币需要满足一定高度后才能赎回
                        return state.Invalid(false,
                            REJECT_INVALID, "bad-txns-premature-redeem-of-mortgage",
                            strprintf("tried to redeem mortgage at depth %d", nSpendHeight - coin.nHeight));
                }
            }
        }

        if (tx.nVersion == MCTransaction::CALL_CONTRACT_VERSION && nContractAmountIn == 0 && tx.pContractData->amountOut > 0) {
            nValueIn += tx.pContractData->amountOut;
            if (!MoneyRange(tx.pContractData->amountOut) || !MoneyRange(nValueIn))
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-outofrange");
            nContractAmountIn += tx.pContractData->amountOut;
        }

        MCAmount nValueOut = 0;
        MCAmount nContractAmountChange = 0;
        for (const auto& tx_out : tx.vout) {
            nValueOut += tx_out.nValue;
            if (!MoneyRange(tx_out.nValue) || !MoneyRange(nValueOut))
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-outofrange");

            if (tx_out.scriptPubKey.IsContract()) {
                MCContractID contractId;
                if (!tx_out.scriptPubKey.GetContractAddr(contractId) || contractId != tx.pContractData->address)
                    return state.DoS(100, false, REJECT_INVALID, "bad_contract_pub_key");
                if (tx_out.scriptPubKey.IsContractChange()) {
                    // 合约输出只能有一个找零
                    if (nContractAmountChange > 0)
                        return state.DoS(100, false, REJECT_INVALID, "bad_contract_amount_change");
                    nContractAmountChange += tx_out.nValue;
                }
            }
        }

        if (nValueIn < nValueOut)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-belowout", false,
                strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(tx.GetValueOut())));

        if (tx.IsBranchChainTransStep2() && tx.fromBranchId != MCBaseChainParams::MAIN)
        {
            MCAmount nInValueBranch = 0;
            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                const MCOutPoint &prevout = tx.vin[i].prevout;
                const Coin& coin = inputs.AccessCoin(prevout);// coin type has check in 
                nInValueBranch += coin.out.nValue;
            }
            uint256 frombranchid;
            frombranchid.SetHex(tx.fromBranchId);
            MCAmount nOrginalOut = 0;
            MCAmount nBranchRecharge = 0;
            for (const auto& txout : tx.vout) {
                if (!IsCoinBranchTranScript(txout.scriptPubKey)) {
                    nOrginalOut += txout.nValue;
                }
                else{
                    nBranchRecharge += txout.nValue;
                    if (!CheckTranBranchScript(frombranchid, txout.scriptPubKey)) {
                        return state.DoS(100, false, REJECT_INVALID, "branch-step-2-recharge-script-invalid");
                    }
                }
            }
            if (nBranchRecharge > nInValueBranch || nInValueBranch - nBranchRecharge != tx.inAmount){
                return state.DoS(100, false, REJECT_INVALID, "Invalid branch nInValueBranch");
            }
        }

        if (tx.nVersion == MCTransaction::CALL_CONTRACT_VERSION && tx.pContractData->amountOut > 0) {
            if (nContractAmountIn - nContractAmountChange != tx.pContractData->amountOut)
                return state.DoS(100, false, REJECT_INVALID, "contract amount not match");
        }

        //check vout
        if (tx.IsRedeemMortgageStatement()){
            MCAmount nMortgageCoinOut = 0;
            int nCountMortgageCoinOut = 0;
            for (const auto& tx_out : tx.vout) {
                uint256 fromtxid;
                if (GetRedeemSriptData(tx_out.scriptPubKey, &fromtxid)) {
                    nMortgageCoinOut += tx_out.nValue;
                    nCountMortgageCoinOut++;
                    if (nCountMortgageCoinOut > 1)
                        return state.DoS(100, false, REJECT_INVALID, "more-than-one-redeem-out", false, "Just one mortgage coin in, one redeem coin out");
                    //check redeem out from txid.
                    if (fromtxid != mortgageFromTxid)
                        return state.DoS(100, false, REJECT_INVALID, "redeem-out-script-error", false, "Redeem out script fromtxid not eq");
                }
            }
            if (nMortgageCoinOut != nMortgageCoin) {
                return state.DoS(100, false, REJECT_INVALID, "Invalid-redeem-tx", false, "mortgage coin nValue not eq redeem coin out");
            }
        }
        else{
            for (const auto& tx_out : tx.vout) {
                branch_script_type obst = QuickGetBranchScriptType(tx_out.scriptPubKey);
                if ((obst == BST_MORTGAGE_MINE && !tx.IsMortgage())){
                    return state.DoS(100, false, REJECT_INVALID, "Mortgage-mine-out-tx-invalid");
                }
                if ((obst == BST_MORTGAGE_COIN && !tx.IsBranchChainTransStep2() && !tx.IsStake())){
                    return state.DoS(100, false, REJECT_INVALID, "Mortgage-coin-out-tx-invalid");
                }
            }
        }

        // Tally transaction fees
        MCAmount nTxFee = nValueIn - tx.GetValueOut();
        if (nTxFee < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-negative");
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-outofrange");
    return true;
}
