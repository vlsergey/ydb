#include "schemeshard_impl.h"

#include <util/string/join.h>

namespace NKikimr {
namespace NSchemeShard {

using namespace NTabletFlatExecutor;

struct TSchemeShard::TTxRunConditionalErase: public TSchemeShard::TRwTxBase {
    TTableInfo::TPtr TableInfo;
    THashMap<TTabletId, NKikimrTxDataShard::TEvConditionalEraseRowsRequest> RunOnTablets;

    TTxRunConditionalErase(TSelf *self, TEvPrivate::TEvRunConditionalErase::TPtr& ev)
        : TRwTxBase(self)
        , TableInfo(nullptr)
    {
        Y_UNUSED(ev);
    }

    TTxRunConditionalErase(TSelf *self, TTableInfo::TPtr tableInfo)
        : TRwTxBase(self)
        , TableInfo(tableInfo)
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_RUN_CONDITIONAL_ERASE;
    }

    void DoExecute(TTransactionContext&, const TActorContext& ctx) override {
        LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "TTxRunConditionalErase DoExecute"
            << ": at schemeshard: " << Self->TabletID());

        if (!Self->AllowConditionalEraseOperations) {
            LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Conditional erase operations are not allowed"
                << ", skip TTxRunConditionalErase"
                << ": at schemeshard: " << Self->TabletID());
            return;
        }

        if (!TableInfo) {
            for (const auto& [_, tableInfo] : Self->TTLEnabledTables) {
                DoExecuteOnTable(tableInfo, ctx);
            }
        } else {
            DoExecuteOnTable(TableInfo, ctx);
        }
    }

    void DoExecuteOnTable(TTableInfo::TPtr tableInfo, const TActorContext& ctx) {
        if (!tableInfo->IsTTLEnabled()) {
            LOG_ERROR_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "TTL is not enabled for table #P1"
                << ", at schemeshard: " << Self->TabletID());
            return;
        }

        const auto& partitions = tableInfo->GetPartitions();
        const auto& shardToPartition = tableInfo->GetShard2PartitionIdx();
        THashSet<TShardIdx> reschedule;

        for (const auto& shardIdx : tableInfo->GetInFlightCondErase()) {
            Y_VERIFY(shardToPartition.contains(shardIdx));
            const ui64 partitionIdx = shardToPartition.at(shardIdx);

            Y_VERIFY(partitionIdx < partitions.size());
            const auto& tableShardInfo = partitions.at(partitionIdx);

            if (!DoExecuteOnShard(tableInfo, tableShardInfo, ctx)) {
                reschedule.insert(shardIdx);
            }
        }

        for (const auto& shardIdx : reschedule) {
            tableInfo->RescheduleCondErase(shardIdx);
        }

        const auto& sysSettings = tableInfo->TTLSettings().GetEnabled().GetSysSettings();
        const auto maxInFlight = sysSettings.GetMaxShardsInFlight();

        while (true) {
            if (maxInFlight && tableInfo->GetInFlightCondErase().size() >= maxInFlight) {
                break;
            }

            const auto* tableShardInfo = tableInfo->GetScheduledCondEraseShard();
            if (!tableShardInfo) {
                break;
            }

            if (!DoExecuteOnShard(tableInfo, *tableShardInfo, ctx)) {
                break;
            }

            tableInfo->AddInFlightCondErase(tableShardInfo->ShardIdx);
        }
    }

    bool DoExecuteOnShard(TTableInfo::TPtr tableInfo, const TTableShardInfo& tableShardInfo, const TActorContext& ctx) {
        if (tableShardInfo.NextCondErase > ctx.Now()) {
            LOG_DEBUG_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Skip conditional erase"
                << ": shardIdx: " << tableShardInfo.ShardIdx
                << ", run at: " << tableShardInfo.NextCondErase
                << ", at schemeshard: " << Self->TabletID());
            return false;
        }

        if (!Self->ShardInfos.contains(tableShardInfo.ShardIdx)) {
            LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Unable to resolve shard info"
                << ": shardIdx: " << tableShardInfo.ShardIdx
                << ", at schemeshard: " << Self->TabletID());
            return false;
        }

        const TShardInfo& shardInfo = Self->ShardInfos.at(tableShardInfo.ShardIdx);
        if (!Self->PathsById.contains(shardInfo.PathId)) {
            LOG_ERROR_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Unable to resolve path"
                << ": shardIdx: " << tableShardInfo.ShardIdx
                << ": pathId: " << shardInfo.PathId
                << ", at schemeshard: " << Self->TabletID());
            return false;
        }

        auto path = Self->PathsById.at(shardInfo.PathId);
        if (path->Dropped()) {
            LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Table is dropped"
                << ": shardIdx: " << tableShardInfo.ShardIdx
                << ": pathId: " << shardInfo.PathId
                << ", at schemeshard: " << Self->TabletID());
            return false;
        }

        if (!Self->Tables.contains(shardInfo.PathId)) {
            LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Unable to resolve table"
                << ": shardIdx: " << tableShardInfo.ShardIdx
                << ": pathId: " << shardInfo.PathId
                << ", at schemeshard: " << Self->TabletID());
            return false;
        }

        const auto& settings = tableInfo->TTLSettings().GetEnabled();
        const TDuration expireAfter = TDuration::Seconds(settings.GetExpireAfterSeconds());
        const TInstant wallClock = ctx.Now() - expireAfter;

        NKikimrTxDataShard::TEvConditionalEraseRowsRequest request;
        request.SetTableId(shardInfo.PathId.LocalPathId);
        request.SetSchemaVersion(tableInfo->AlterVersion);

        for (const auto& [indexId, columnIds]: MakeIndexes(shardInfo.PathId)) {
            auto& index = *request.MutableIndexes()->Add();

            index.SetOwnerId(indexId.PathId.OwnerId);
            index.SetPathId(indexId.PathId.LocalPathId);
            index.SetSchemaVersion(indexId.SchemaVersion);

            for (const auto& [indexColumnId, mainColumnId] : columnIds) {
                auto& keyMap = *index.MutableKeyMap()->Add();
                keyMap.SetIndexColumnId(indexColumnId);
                keyMap.SetMainColumnId(mainColumnId);
            }
        }

        request.MutableExpiration()->SetColumnId(tableInfo->GetTTLColumnId());
        request.MutableExpiration()->SetWallClockTimestamp(wallClock.GetValue());
        request.MutableExpiration()->SetColumnUnit(settings.GetColumnUnit());

        const auto& sysSettings = settings.GetSysSettings();
        request.MutableLimits()->SetBatchMaxBytes(sysSettings.GetBatchMaxBytes());
        request.MutableLimits()->SetBatchMinKeys(sysSettings.GetBatchMinKeys());
        request.MutableLimits()->SetBatchMaxKeys(sysSettings.GetBatchMaxKeys());

        RunOnTablets.emplace(shardInfo.TabletID, std::move(request));

        return true;
    }

    void DoComplete(const TActorContext& ctx) override {
        LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "TTxRunConditionalErase DoComplete"
            << ": at schemeshard: " << Self->TabletID());

        for (auto& kv : RunOnTablets) {
            const auto& tabletId = kv.first;
            auto& request = kv.second;

            auto ev = MakeHolder<TEvDataShard::TEvConditionalEraseRowsRequest>();
            ev->Record = std::move(request);

            LOG_DEBUG_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Run conditional erase"
                << ", tabletId: " << tabletId
                << ", request: " << ev->Record.ShortDebugString()
                << ", at schemeshard: " << Self->TabletID());
            Self->PipeClientCache->Send(ctx, ui64(tabletId), ev.Release());
        }

        if (!TableInfo) {
            Self->ScheduleConditionalEraseRun(ctx);
        }
    }

private:
    THashMap<TTableId, TVector<std::pair<ui32, ui32>>> MakeIndexes(const TPathId& mainPathId) const {
        THashMap<TTableId, TVector<std::pair<ui32, ui32>>> result;

        Y_VERIFY(Self->PathsById.contains(mainPathId));
        auto mainPath = Self->PathsById.at(mainPathId);

        Y_VERIFY(Self->Tables.contains(mainPathId));
        auto mainTable = Self->Tables.at(mainPathId);

        for (const auto& [_, childPathId] : mainPath->GetChildren()) {
            Y_VERIFY(Self->PathsById.contains(childPathId));
            auto childPath = Self->PathsById.at(childPathId);

            if (!childPath->IsTableIndex() || childPath->Dropped()) {
                continue;
            }

            auto index = GetIndex(childPath);
            if (index->Type == NKikimrSchemeOp::EIndexTypeGlobalAsync) {
                continue;
            }

            auto [indexImplTableId, indexImplTable] = GetIndexImplTable(childPath);

            auto ret = result.emplace(
                TTableId(indexImplTableId, indexImplTable->AlterVersion),
                MakeColumnIds(mainTable, index, indexImplTable)
            );
            Y_VERIFY(ret.second);
        }

        return result;
    }

    TTableIndexInfo::TPtr GetIndex(TPathElement::TPtr indexPath) const {
        Y_VERIFY(Self->Indexes.contains(indexPath->PathId));
        return Self->Indexes.at(indexPath->PathId);
    }

    std::pair<TPathId, TTableInfo::TPtr> GetIndexImplTable(TPathElement::TPtr indexPath) const {
        Y_VERIFY(indexPath->GetChildren().size() == 1);

        for (const auto& [_, indexImplPathId] : indexPath->GetChildren()) {
            auto childPath = Self->PathsById.at(indexImplPathId);

            Y_VERIFY(!childPath->Dropped());

            Y_VERIFY(Self->Tables.contains(indexImplPathId));
            return std::make_pair(indexImplPathId, Self->Tables.at(indexImplPathId));
        }

        Y_FAIL("Unreachable");
    }

    static TVector<std::pair<ui32, ui32>> MakeColumnIds(TTableInfo::TPtr mainTable, TTableIndexInfo::TPtr index, TTableInfo::TPtr indexImplTable) {
        TVector<std::pair<ui32, ui32>> result;
        THashSet<TString> keys;

        const auto mainColumns = MakeColumnNameToId(mainTable->Columns);
        const auto indexImplColumns = MakeColumnNameToId(indexImplTable->Columns);

        for (const TString& indexKey : index->IndexKeys) {
            Y_VERIFY(mainColumns.contains(indexKey));
            Y_VERIFY(indexImplColumns.contains(indexKey));

            result.emplace_back(std::make_pair(indexImplColumns.at(indexKey), mainColumns.at(indexKey)));
            keys.insert(indexKey);
        }

        for (const ui32 mainColumnId : mainTable->KeyColumnIds) {
            Y_VERIFY(mainTable->Columns.contains(mainColumnId));
            const TString& mainKey = mainTable->Columns.at(mainColumnId).Name;

            if (keys.contains(mainKey)) {
                continue;
            }

            Y_VERIFY(indexImplColumns.contains(mainKey));
            result.emplace_back(std::make_pair(indexImplColumns.at(mainKey), mainColumnId));
        }

        return result;
    }

    static THashMap<TString, ui32> MakeColumnNameToId(const THashMap<ui32, TTableInfo::TColumn>& columns) {
        THashMap<TString, ui32> result;

        for (const auto& [id, column] : columns) {
            if (column.IsDropped()) {
                continue;
            }

            auto ret = result.emplace(column.Name, id);
            Y_VERIFY(ret.second);
        }

        return result;
    }

}; // TTxRunConditionalErase

struct TSchemeShard::TTxScheduleConditionalErase : public TTransactionBase<TSchemeShard> {
    TEvDataShard::TEvConditionalEraseRowsResponse::TPtr Ev;
    THolder<NSysView::TEvSysView::TEvUpdateTtlStats> StatsCollectorEv;
    TTableInfo::TPtr TableInfo;

    TTxScheduleConditionalErase(TSelf* self, TEvDataShard::TEvConditionalEraseRowsResponse::TPtr& ev)
        : TBase(self)
        , Ev(ev)
        , TableInfo(nullptr)
    {
    }

    virtual ~TTxScheduleConditionalErase() = default;

    TTxType GetTxType() const override {
        return TXTYPE_SCHEDULE_CONDITIONAL_ERASE;
    }

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "TTxScheduleConditionalErase Execute"
            << ": at schemeshard: " << Self->TabletID());

        if (!Self->AllowConditionalEraseOperations) {
            LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Conditional erase operations are not allowed"
                << ", skip TTxScheduleConditionalErase"
                << ": at schemeshard: " << Self->TabletID());
            return true;
        }

        const auto& record = Ev->Get()->Record;

        const TTabletId tabletId(record.GetTabletID());
        const TShardIdx shardIdx = Self->GetShardIdx(tabletId);

        if (!Self->ShardInfos.contains(shardIdx)) {
            LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Unable to resolve shard info"
                << ": tabletId: " << tabletId
                << ", at schemeshard: " << Self->TabletID());
            return true;
        }

        const TShardInfo& shardInfo = Self->ShardInfos.at(shardIdx);
        const TPathId& tableId = shardInfo.PathId;

        if (!Self->TTLEnabledTables.contains(tableId)) {
            LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "TTL is not enabled for table #P2"
                << ": tabletId: " << tabletId
                << ", tableId: " << tableId
                << ", at schemeshard: " << Self->TabletID());
            return true;
        }

        TTableInfo::TPtr tableInfo = Self->TTLEnabledTables.at(tableId);
        if (!tableInfo->IsTTLEnabled()) {
            LOG_ERROR_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "TTL is not enabled for table #P3"
                << ": tabletId: " << tabletId
                << ", tableId: " << tableId
                << ", at schemeshard: " << Self->TabletID());
            return true;
        }

        const auto& inFlight = tableInfo->GetInFlightCondErase();
        if (!inFlight.contains(shardIdx)) {
            LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Shard idx mismatch"
                << ", tableId: " << tableId
                << ", shardIdx: " << shardIdx
                << ", in-flight shards: [" << JoinSeq(",", inFlight) << "]"
                << ", at schemeshard: " << Self->TabletID());
            return true;
        }

        const auto& sysSettings = tableInfo->TTLSettings().GetEnabled().GetSysSettings();
        TDuration next = TDuration::FromValue(sysSettings.GetRunInterval());

        switch (record.GetStatus()) {
        case NKikimrTxDataShard::TEvConditionalEraseRowsResponse::PARTIAL:
            // TODO: remember progress
            return true;

        case NKikimrTxDataShard::TEvConditionalEraseRowsResponse::BAD_REQUEST:
        case NKikimrTxDataShard::TEvConditionalEraseRowsResponse::ABORTED:
        case NKikimrTxDataShard::TEvConditionalEraseRowsResponse::ERASE_ERROR:
        case NKikimrTxDataShard::TEvConditionalEraseRowsResponse::OVERLOADED:
        case NKikimrTxDataShard::TEvConditionalEraseRowsResponse::SCHEME_ERROR:
            next = TDuration::FromValue(sysSettings.GetRetryInterval());
            LOG_ERROR_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Unsuccessful conditional erase"
                << ": tabletId: " << tabletId
                << ", status: " << NKikimrTxDataShard::TEvConditionalEraseRowsResponse_EStatus_Name(record.GetStatus())
                << ", error: " << record.GetErrorDescription()
                << ", retry after: " << next
                << ", at schemeshard: " << Self->TabletID());
            break;

        case NKikimrTxDataShard::TEvConditionalEraseRowsResponse::OK:
            LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Successful conditional erase"
                << ": tabletId: " << tabletId
                << ", at schemeshard: " << Self->TabletID());
            break;

        default:
            LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "Unknown conditional erase status"
                << ": tabletId: " << tabletId
                << ", status: " << static_cast<ui32>(record.GetStatus())
                << ", error: " << record.GetErrorDescription()
                << ", at schemeshard: " << Self->TabletID());
            break;
        }

        const auto& shardToPartition = tableInfo->GetShard2PartitionIdx();
        Y_VERIFY(shardToPartition.contains(shardIdx));
        const ui64 partitionIdx = shardToPartition.at(shardIdx);

        const auto& partitions = tableInfo->GetPartitions();
        Y_VERIFY(partitionIdx < partitions.size());
        const auto& tableShardInfo = partitions.at(partitionIdx);

        const auto& lag = tableShardInfo.LastCondEraseLag;

        if (lag) {
            Self->TabletCounters->Percentile()[COUNTER_NUM_SHARDS_BY_TTL_LAG].DecrementFor(lag->Seconds());
        } else {
            Y_VERIFY_DEBUG(false);
        }

        const auto now = ctx.Now();

        NIceDb::TNiceDb db(txc.DB);
        tableInfo->ScheduleNextCondErase(shardIdx, now, next);
        Self->PersistTablePartitionCondErase(db, tableId, partitionIdx, tableInfo);

        if (AppData(ctx)->FeatureFlags.GetEnableSystemViews()) {
            StatsCollectorEv = MakeHolder<NSysView::TEvSysView::TEvUpdateTtlStats>(
                Self->GetDomainKey(tableId), tableId, std::make_pair(ui64(shardIdx.GetOwnerId()), ui64(shardIdx.GetLocalId()))
            );

            auto& stats = StatsCollectorEv->Stats;
            stats.SetLastRunTime(now.MilliSeconds());
            stats.SetLastRowsProcessed(record.GetStats().GetRowsProcessed());
            stats.SetLastRowsErased(record.GetStats().GetRowsErased());
        }

        Y_VERIFY(lag.Defined());
        Self->TabletCounters->Percentile()[COUNTER_NUM_SHARDS_BY_TTL_LAG].IncrementFor(lag->Seconds());

        TableInfo = tableInfo;
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "TTxScheduleConditionalErase Complete"
            << ": at schemeshard: " << Self->TabletID());

        if (StatsCollectorEv) {
            ctx.Send(Self->SysPartitionStatsCollector, StatsCollectorEv.Release());
        }

        if (TableInfo) {
            Self->Execute(new TTxRunConditionalErase(Self, TableInfo), ctx);
        }
    }

}; // TTxScheduleConditionalErase

ITransaction* TSchemeShard::CreateTxRunConditionalErase(TEvPrivate::TEvRunConditionalErase::TPtr& ev) {
    return new TTxRunConditionalErase(this, ev);
}

ITransaction* TSchemeShard::CreateTxScheduleConditionalErase(TEvDataShard::TEvConditionalEraseRowsResponse::TPtr& ev) {
    return new TTxScheduleConditionalErase(this, ev);
}

} // NSchemeShard
} // NKikimr
