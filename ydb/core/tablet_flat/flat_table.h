#pragma once
#include "defs.h"
#include "flat_update_op.h"
#include "flat_dbase_scheme.h"
#include "flat_mem_warm.h"
#include "flat_iterator.h"
#include "flat_row_scheme.h"
#include "flat_row_versions.h"
#include "flat_part_laid.h"
#include "flat_part_slice.h"
#include "flat_table_committed.h"
#include "flat_table_part.h"
#include "flat_table_stats.h"
#include "flat_table_subset.h"
#include "flat_table_misc.h"
#include "flat_sausage_solid.h"
#include "util_basics.h"

#include <ydb/core/scheme/scheme_tablecell.h>
#include <library/cpp/containers/stack_vector/stack_vec.h>

#include <util/generic/deque.h>
#include <util/generic/set.h>
#include <util/generic/hash.h>
#include <util/generic/ptr.h>

namespace NKikimr {
namespace NTable {

class TTableEpochs;
class TKeyRangeCache;

class TTable: public TAtomicRefCount<TTable> {
public:
    using TOpsRef = TArrayRef<const TUpdateOp>;
    using TMemGlob = NPageCollection::TMemGlob;

    struct TStat {
        /*_ In memory (~memtable) data statistics   */

        ui64 FrozenWaste = 0;
        ui64 FrozenSize = 0;
        ui64 FrozenOps  = 0;
        ui64 FrozenRows = 0;

        /*_ Already flatten data statistics (parts) */

        TPartStats Parts;
        THashMap<ui64, TPartStats> PartsPerTablet;
    };

    struct TReady {
        EReady Ready = EReady::Page;

        /* Per part operation statictics on Charge(...) or Select(...)
            for ByKey bloom filter usage. The filter misses >= NoKey */

        ui64 Weeded = 0;
        ui64 Sieved = 0;
        ui64 NoKey = 0;         /* Examined TPart without the key */

        TIteratorStats Stats;
    };

    explicit TTable(TEpoch);
    ~TTable();

    void SetScheme(const TScheme::TTableInfo& tableScheme);

    TIntrusiveConstPtr<TRowScheme> GetScheme() const noexcept;

    TEpoch Snapshot() noexcept;

    TEpoch Head() const noexcept
    {
        return Epoch;
    }

    TAutoPtr<TSubset> Subset(TArrayRef<const TLogoBlobID> bundle, TEpoch edge);
    TAutoPtr<TSubset> Subset(TEpoch edge) const noexcept;
    TAutoPtr<TSubset> ScanSnapshot(TRowVersion snapshot = TRowVersion::Max()) noexcept;
    TAutoPtr<TSubset> Unwrap() noexcept; /* full Subset(..) + final Replace(..) */

    bool HasBorrowed(ui64 selfTabletId) const noexcept;

    /**
     * Returns current slices for bundles
     *
     * Map will only contain bundles that currently exist in the table
     */
    TBundleSlicesMap LookupSlices(TArrayRef<const TLogoBlobID> bundles) const noexcept;

    /**
     * Replaces slices for bundles in the slices map
     */
    void ReplaceSlices(TBundleSlicesMap slices) noexcept;

    /* Interface for redistributing data layout within the table. Take some
        subset with Subset(...) call, do some work and then return result
        with Replace(...) method. The result should hold the same set of rows
        as original subset. Replace(...) may produce some garbage that have to
        be displaced from table with Clean() method eventually.
    */

    void Replace(TArrayRef<const TPartView>, const TSubset&) noexcept;
    void ReplaceTxStatus(TArrayRef<const TIntrusiveConstPtr<TTxStatusPart>>, const TSubset&) noexcept;

    /*_ Special interface for clonig flatten part of table for outer usage.
        Cook some TPartView with Subset(...) method and/or TShrink tool first and
        then merge produced TPartView to outer table.
    */

    void Merge(TPartView partView) noexcept;
    void Merge(TIntrusiveConstPtr<TColdPart> part) noexcept;
    void Merge(TIntrusiveConstPtr<TTxStatusPart> txStatus) noexcept;
    void ProcessCheckTransactions() noexcept;

    /**
     * Returns constructed levels for slices
     */
    const TLevels& GetLevels() const noexcept;

    /**
     * Returns search height if there are no cold parts, 0 otherwise
     */
    ui64 GetSearchHeight() const noexcept;

    /* Hack for filling external blobs in TMemTable tables with data */

    TVector<TIntrusiveConstPtr<TMemTable>> GetMemTables() const noexcept;

    TAutoPtr<TTableIt> Iterate(TRawVals key, TTagsRef tags, IPages* env, ESeek,
            TRowVersion snapshot,
            const ITransactionMapPtr& visible = nullptr,
            const ITransactionObserverPtr& observer = nullptr) const noexcept;
    TAutoPtr<TTableReverseIt> IterateReverse(TRawVals key, TTagsRef tags, IPages* env, ESeek,
            TRowVersion snapshot,
            const ITransactionMapPtr& visible = nullptr,
            const ITransactionObserverPtr& observer = nullptr) const noexcept;
    EReady Select(TRawVals key, TTagsRef tags, IPages* env, TRowState& row,
                  ui64 flg, TRowVersion snapshot, TDeque<TPartSimpleIt>& tempIterators,
                  TSelectStats& stats,
                  const ITransactionMapPtr& visible = nullptr,
                  const ITransactionObserverPtr& observer = nullptr) const noexcept;

    EReady Precharge(TRawVals minKey, TRawVals maxKey, TTagsRef tags,
                     IPages* env, ui64 flg,
                     ui64 itemsLimit, ui64 bytesLimit,
                     EDirection direction, TRowVersion snapshot, TSelectStats& stats) const;

    void Update(ERowOp, TRawVals key, TOpsRef, TArrayRef<TMemGlob> apart, TRowVersion rowVersion);

    void UpdateTx(ERowOp, TRawVals key, TOpsRef, TArrayRef<TMemGlob> apart, ui64 txId);
    void CommitTx(ui64 txId, TRowVersion rowVersion);
    void RemoveTx(ui64 txId);

    TPartView GetPartView(const TLogoBlobID &bundle) const
    {
        auto *partView = Flatten.FindPtr(bundle);

        return partView ? *partView : TPartView{ };
    }

    TVector<TPartView> GetAllParts() const
    {
        TVector<TPartView> parts(Reserve(Flatten.size()));

        for (auto& x : Flatten) {
            parts.emplace_back(x.second);
        }

        return parts;
    }

    TVector<TIntrusiveConstPtr<TColdPart>> GetColdParts() const
    {
        TVector<TIntrusiveConstPtr<TColdPart>> parts(Reserve(ColdParts.size()));

        for (auto& x : ColdParts) {
            parts.emplace_back(x.second);
        }

        return parts;
    }

    void EnumerateParts(const std::function<void(const TPartView&)>& callback) const
    {
        for (auto& x : Flatten) {
            callback(x.second);
        }
    }

    void EnumerateColdParts(const std::function<void(const TIntrusiveConstPtr<TColdPart>&)>& callback) const
    {
        for (auto& x : ColdParts) {
            callback(x.second);
        }
    }

    void EnumerateTxStatusParts(const std::function<void(const TIntrusiveConstPtr<TTxStatusPart>&)>& callback) const
    {
        for (auto& x : TxStatus) {
            callback(x.second);
        }
    }

    const TStat& Stat() const noexcept
    {
        return Stat_;
    }

    ui64 GetMemSize(TEpoch epoch = TEpoch::Max()) const noexcept
    {
        if (Y_LIKELY(epoch == TEpoch::Max())) {
            return Stat_.FrozenSize + (Mutable ? Mutable->GetUsedMem() : 0);
        }

        ui64 size = 0;

        for (const auto& x : Frozen) {
            if (x->Epoch < epoch) {
                size += x->GetUsedMem();
            }
        }

        if (Mutable && Mutable->Epoch < epoch) {
            size += Mutable->GetUsedMem();
        }

        return size;
    }

    ui64 GetMemWaste() const noexcept
    {
        return Stat_.FrozenWaste + (Mutable ? Mutable->GetWastedMem() : 0);
    }

    ui64 GetMemRowCount() const noexcept
    {
        return Stat_.FrozenRows + (Mutable ? Mutable->GetRowCount() : 0);
    }

    ui64 GetOpsCount() const noexcept
    {
        return Stat_.FrozenOps + (Mutable ? Mutable->GetOpsCount() : 0);
    }

    ui64 GetPartsCount() const noexcept
    {
        return Flatten.size();
    }

    ui64 EstimateRowSize() const noexcept
    {
        ui64 size = Stat_.FrozenSize + (Mutable ? Mutable->GetUsedMem() : 0);
        ui64 rows = Stat_.FrozenRows + (Mutable ? Mutable->GetRowCount() : 0);

        for (const auto& flat : Flatten) {
            if (const TPartView &partView = flat.second) {
                size += partView->DataSize();
                rows += partView->Index.Rows();
            }
        }

        return rows ? (size / rows) : 0;
    }

    void DebugDump(IOutputStream& str, IPages *env, const NScheme::TTypeRegistry& typeRegistry) const;

    TKeyRangeCache* GetErasedKeysCache() const;

    bool RemoveRowVersions(const TRowVersion& lower, const TRowVersion& upper);

    const TRowVersionRanges& GetRemovedRowVersions() const {
        return RemovedRowVersions;
    }

    TCompactionStats GetCompactionStats() const;

    void FillTxStatusCache(THashMap<TLogoBlobID, TSharedData>& cache) const noexcept;

private:
    TMemTable& MemTable();
    void AddSafe(TPartView partView);

    void AddStat(const TPartView& partView);
    void RemoveStat(const TPartView& partView);

private:
    struct TOpenTransaction {
        THashSet<TIntrusiveConstPtr<TMemTable>> Mem;
        THashSet<TIntrusiveConstPtr<TPart>> Parts;
    };

private:
    TEpoch Epoch; /* Monotonic table change number, with holes */
    ui64 Annexed = 0; /* Monotonic serial of attached external blobs */
    TIntrusiveConstPtr<TRowScheme> Scheme;
    TIntrusivePtr<TMemTable> Mutable;
    TSet<TIntrusiveConstPtr<TMemTable>, TOrderByEpoch<TMemTable>> Frozen;
    THashMap<TLogoBlobID, TPartView> Flatten;
    THashMap<TLogoBlobID, TIntrusiveConstPtr<TColdPart>> ColdParts;
    THashMap<TLogoBlobID, TIntrusiveConstPtr<TTxStatusPart>> TxStatus;
    TEpoch FlattenEpoch = TEpoch::Min(); /* Current maximum flatten epoch */
    TStat Stat_;
    mutable THolder<TLevels> Levels;
    mutable TIntrusivePtr<TKeyRangeCache> ErasedKeysCache;

    bool EraseCacheEnabled = false;
    TKeyRangeCacheConfig EraseCacheConfig;

    TRowVersionRanges RemovedRowVersions;

    THashSet<ui64> CheckTransactions;
    THashMap<ui64, TOpenTransaction> OpenTransactions;
    TTransactionMap CommittedTransactions;
    TTransactionSet RemovedTransactions;
};

}
}
