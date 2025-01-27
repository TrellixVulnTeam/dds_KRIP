/*
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/single_transaction_stats.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/shard_id.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

extern AtomicInt32 transactionLifetimeLimitSeconds;

class OperationContext;
class UpdateRequest;

enum class SpeculativeTransactionOpTime {
    kLastApplied,
    kAllCommitted,
};

/**
 * A write through cache for the state of a particular session. All modifications to the underlying
 * session transactions collection must be performed through an object of this class.
 *
 * The cache state can be 'up-to-date' (it is in sync with the persistent contents) or 'needs
 * refresh' (in which case refreshFromStorageIfNeeded needs to be called in order to make it
 * up-to-date).
 */
class Session {

public:
    using CommittedStatementTimestampMap = stdx::unordered_map<StmtId, repl::OpTime>;
    using CursorExistsFunction = std::function<bool(LogicalSessionId, TxnNumber)>;

    static const BSONObj kDeadEndSentinel;

    virtual ~Session() {}

    const LogicalSessionId& getSessionId() const {
        return _sessionId;
    }

    /**
     * Blocking method, which loads the transaction state from storage if it has been marked as
     * needing refresh.
     *
     * In order to avoid the possibility of deadlock, this method must not be called while holding a
     * lock.
     */
    virtual void refreshFromStorageIfNeeded(OperationContext* opCtx) = 0;

    /**
     * Starts a new transaction on the session, or continues an already active transaction. In this
     * context, a "transaction" is a sequence of operations associated with a transaction number.
     * This sequence of operations could be a retryable write or multi-statement transaction. Both
     * utilize this method.
     *
     * The 'autocommit' argument represents the value of the field given in the original client
     * request. If it is boost::none, no autocommit parameter was passed into the request. Every
     * operation that is part of a multi statement transaction must specify 'autocommit=false'.
     * 'startTransaction' represents the value of the field given in the original client request,
     * and indicates whether this operation is the beginning of a multi-statement transaction.
     *
     * Throws an exception if:
     *      - An attempt is made to start a transaction with number less than the latest
     *        transaction this session has seen.
     *      - The session has been invalidated.
     *      - The values of 'autocommit' and/or 'startTransaction' are inconsistent with the current
     *        state of the transaction.
     *
     * In order to avoid the possibility of deadlock, this method must not be called while holding a
     * lock. This method must also be called after refreshFromStorageIfNeeded has been called.
     */
    virtual void beginOrContinueTxn(OperationContext* opCtx,
                                    TxnNumber txnNumber,
                                    boost::optional<bool> autocommit,
                                    boost::optional<bool> startTransaction,
                                    StringData dbName,
                                    StringData cmdName) = 0;
    /**
     * Similar to beginOrContinueTxn except it is used specifically for shard migrations and does
     * not check or modify the autocommit parameter.
     */
    virtual void beginOrContinueTxnOnMigration(OperationContext* opCtx, TxnNumber txnNumber) = 0;

    /**
     * Called for speculative transactions to fix the optime of the snapshot to read from.
     */
    virtual void setSpeculativeTransactionOpTime(OperationContext* opCtx,
                                                 SpeculativeTransactionOpTime opTimeChoice) = 0;

    /**
     * Called after a write under the specified transaction completes while the node is a primary
     * and specifies the statement ids which were written. Must be called while the caller is still
     * in the write's WUOW. Updates the on-disk state of the session to match the specified
     * transaction/opTime and keeps the cached state in sync.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    virtual void onWriteOpCompletedOnPrimary(OperationContext* opCtx,
                                             TxnNumber txnNumber,
                                             std::vector<StmtId> stmtIdsWritten,
                                             const repl::OpTime& lastStmtIdWriteOpTime,
                                             Date_t lastStmtIdWriteDate) = 0;

    /**
     * Helper function to begin a migration on a primary node.
     *
     * Returns whether the specified statement should be migrated at all or skipped.
     */
    virtual bool onMigrateBeginOnPrimary(OperationContext* opCtx,
                                         TxnNumber txnNumber,
                                         StmtId stmtId) = 0;

    /**
     * Called after an entry for the specified session and transaction has been written to the oplog
     * during chunk migration, while the node is still primary. Must be called while the caller is
     * still in the oplog write's WUOW. Updates the on-disk state of the session to match the
     * specified transaction/opTime and keeps the cached state in sync.
     *
     * May be called concurrently with onWriteOpCompletedOnPrimary or onMigrateCompletedOnPrimary
     * and doesn't require the session to be checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number is newer than the
     * one specified.
     */
    virtual void onMigrateCompletedOnPrimary(OperationContext* opCtx,
                                             TxnNumber txnNumber,
                                             std::vector<StmtId> stmtIdsWritten,
                                             const repl::OpTime& lastStmtIdWriteOpTime,
                                             Date_t oplogLastStmtIdWriteDate) = 0;

    /**
     * Marks the session as requiring refresh. Used when the session state has been modified
     * externally, such as through a direct write to the transactions table.
     */
    virtual void invalidate() = 0;

    /**
     * Returns the op time of the last committed write for this session and transaction. If no write
     * has completed yet, returns an empty timestamp.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    virtual repl::OpTime getLastWriteOpTime(TxnNumber txnNumber) const = 0;

    /**
     * Checks whether the given statementId for the specified transaction has already executed and
     * if so, returns the oplog entry which was generated by that write. If the statementId hasn't
     * executed, returns boost::none.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    virtual boost::optional<repl::OplogEntry> checkStatementExecuted(OperationContext* opCtx,
                                                                     TxnNumber txnNumber,
                                                                     StmtId stmtId) const = 0;

    /**
     * Checks whether the given statementId for the specified transaction has already executed
     * without fetching the oplog entry which was generated by that write.
     *
     * Must only be called with the session checked-out.
     *
     * Throws if the session has been invalidated or the active transaction number doesn't match.
     */
    virtual bool checkStatementExecutedNoOplogEntryFetch(TxnNumber txnNumber,
                                                         StmtId stmtId) const = 0;

    /**
     * Transfers management of transaction resources from the OperationContext to the Session.
     */
    virtual void stashTransactionResources(OperationContext* opCtx) = 0;

    /**
     * Transfers management of transaction resources from the Session to the OperationContext.
     */
    virtual void unstashTransactionResources(OperationContext* opCtx,
                                             const std::string& cmdName) = 0;

    /**
     * Commits the transaction, including committing the write unit of work and updating
     * transaction state.
     */
    virtual void commitTransaction(OperationContext* opCtx) = 0;

    /**
     * Aborts the transaction outside the transaction, releasing transaction resources.
     */
    virtual void abortArbitraryTransaction(OperationContext* opCtx) = 0;

    /**
     * Same as abortArbitraryTransaction, except only executes if _transactionExpireDate indicates
     * that the transaction has expired.
     */
    virtual void abortArbitraryTransactionIfExpired(OperationContext* opCtx) = 0;

    /*
     * Aborts the transaction inside the transaction, releasing transaction resources.
     * We're inside the transaction when we have the Session checked out and 'opCtx' owns the
     * transaction resources.
     */
    virtual void abortActiveTransaction(OperationContext* opCtx) = 0;

    bool getAutocommit() const {
        return _autocommit;
    }

    /**
     * Returns whether we are in a multi-document transaction, which means we have an active
     * transaction which has autoCommit:false and has not been committed or aborted.
     */
    virtual bool inMultiDocumentTransaction() const = 0;

    virtual bool transactionIsCommitted() const = 0;

    virtual bool transactionIsAborted() const = 0;

    /**
     * Returns true if we are in an active multi-document transaction or if the transaction has
     * been aborted. This is used to cover the case where a transaction has been aborted, but the
     * OperationContext state has not been cleared yet.
     */
    virtual bool inActiveOrKilledMultiDocumentTransaction() const = 0;

    /**
     * Adds a stored operation to the list of stored operations for the current multi-document
     * (non-autocommit) transaction.  It is illegal to add operations when no multi-document
     * transaction is in progress.
     */
    virtual void addTransactionOperation(OperationContext* opCtx,
                                         const repl::ReplOperation& operation) = 0;

    /**
     * Returns and clears the stored operations for an multi-document (non-autocommit) transaction,
     * and marks the transaction as closed.  It is illegal to attempt to add operations to the
     * transaction after this is called.
     */
    virtual std::vector<repl::ReplOperation> endTransactionAndRetrieveOperations(
        OperationContext* opCtx) = 0;

    virtual const std::vector<repl::ReplOperation>& transactionOperationsForTest() = 0;

    virtual TxnNumber getActiveTxnNumberForTest() const = 0;

    virtual boost::optional<SingleTransactionStats> getSingleTransactionStats() const = 0;

    virtual repl::OpTime getSpeculativeTransactionReadOpTimeForTest() const = 0;

    virtual const Locker* getTxnResourceStashLockerForTest() const = 0;

    /**
     * If this session is holding stashed locks in _txnResourceStash, reports the current state of
     * the session using the provided builder. Locks the session object's mutex while running.
     */
    virtual void reportStashedState(BSONObjBuilder* builder) const = 0;

    /**
     * If this session is not holding stashed locks in _txnResourceStash (transaction is active),
     * reports the current state of the session using the provided builder. Locks the session
     * object's mutex while running.
     */
    virtual void reportUnstashedState(repl::ReadConcernArgs readConcernArgs,
                                      BSONObjBuilder* builder) const = 0;

    /**
     * Convenience method which creates and populates a BSONObj containing the stashed state.
     * Returns an empty BSONObj if this session has no stashed resources.
     */
    virtual BSONObj reportStashedState() const = 0;

    virtual std::string transactionInfoForLogForTest(const SingleThreadedLockStats* lockStats,
                                                     bool committed,
                                                     repl::ReadConcernArgs readConcernArgs) = 0;

    virtual void addMultikeyPathInfo(MultikeyPathInfo info) = 0;

    virtual const std::vector<MultikeyPathInfo>& getMultikeyPathInfo() const = 0;

    /**
      * Sets the current operation running on this Session.
      */
    virtual void setCurrentOperation(OperationContext* currentOperation) = 0;

    /**
     * Clears the current operation running on this Session.
     */
    virtual void clearCurrentOperation() = 0;

    /**
     * Append TxnInfo into command on mongos
     */
    virtual BSONObj appendTransactionInfo(OperationContext* opCtx,
                                          const ShardId& shardId,
                                          BSONObj obj) = 0;

    /**
     * Returns a new oplog entry if the given entry has transaction state embedded within in.
     * The new oplog entry will contain the operation needed to replicate the transaction
     * table.
     * Returns boost::none if the given oplog doesn't have any transaction state or does not
     * support update to the transaction table.
     */
    static boost::optional<repl::OplogEntry> createMatchingTransactionTableUpdate(
        const repl::OplogEntry& entry);

    static std::unique_ptr<Session> makeOwn(LogicalSessionId lsid);

    bool getStartTransaction() const {
        return _startTransaction;
    }

    std::string shardId;  // only used for non shard collection transaction in cluster

    // Set when a snapshot read / transaction begins. Alleviates cache pressure by limiting how long
    // a snapshot will remain open and available. Checked in combination with _txnState to determine
    // whether the transaction should be aborted.
    // This is unset until a transaction begins on the session, and then reset only when new
    // transactions begin.

protected:
    // Holds function which determines whether the CursorManager has client cursor references for a
    // given transaction.
    static CursorExistsFunction _cursorExistsFunction;

    virtual void _beginOrContinueTxn(OperationContext* opCtx,
                                     WithLock,
                                     TxnNumber txnNumber,
                                     boost::optional<bool> autocommit,
                                     boost::optional<bool> startTransaction) = 0;

    virtual void _beginOrContinueTxnOnMigration(OperationContext* opCtx,
                                                WithLock,
                                                TxnNumber txnNumber) = 0;

    // Checks if there is a conflicting operation on the current Session
    virtual void _checkValid(WithLock) const = 0;

    // Checks that a new txnNumber is higher than the activeTxnNumber so
    // we don't start a txn that is too old.
    virtual void _checkTxnValid(WithLock, TxnNumber txnNumber) const = 0;

    virtual void _setActiveTxn(OperationContext* opCtx, WithLock, TxnNumber txnNumber) = 0;

    virtual void _checkIsActiveTransaction(WithLock,
                                           TxnNumber txnNumber,
                                           bool checkAbort) const = 0;

    virtual boost::optional<repl::OpTime> _checkStatementExecuted(WithLock,
                                                                  TxnNumber txnNumber,
                                                                  StmtId stmtId) const = 0;
    ;

    // Returns the write date of the last committed write for this session and transaction. If no
    // write has completed yet, returns an empty date.
    //
    // Throws if the session has been invalidated or the active transaction number doesn't match.
    virtual Date_t _getLastWriteDate(WithLock, TxnNumber txnNumber) const = 0;

    virtual UpdateRequest _makeUpdateRequest(WithLock,
                                             TxnNumber newTxnNumber,
                                             const repl::OpTime& newLastWriteTs,
                                             Date_t newLastWriteDate) const = 0;

    virtual void _registerUpdateCacheOnCommit(OperationContext* opCtx,
                                              TxnNumber newTxnNumber,
                                              std::vector<StmtId> stmtIdsWritten,
                                              const repl::OpTime& lastStmtIdWriteTs) = 0;

    // Releases stashed transaction resources to abort the transaction.
    virtual void _abortTransaction(OperationContext* opCtx, WithLock) = 0;

    // Committing a transaction first changes its state to "Committing" and writes to the oplog,
    // then it changes the state to "Committed".
    //
    // When a transaction is in "Committing" state, it's not allowed for other threads to change its
    // state (i.e. abort the transaction), otherwise the on-disk state will diverge from the
    // in-memory state.
    // There are 3 cases where the transaction will be aborted.
    // 1) abortTransaction command. Session check-out mechanism only allows one client to access a
    // transaction.
    // 2) killSession, stepdown, transaction timeout and any thread that aborts the transaction
    // outside of session checkout. They can safely skip the committing transactions.
    // 3) Migration. Should be able to skip committing transactions.
    virtual void _commitTransaction(stdx::unique_lock<stdx::mutex> lk, OperationContext* opCtx) = 0;

    bool _startTransaction{false};

    LogicalSessionId _sessionId;

    // Protects the member variables below.
    mutable stdx::mutex _mutex;

    // A pointer back to the currently running operation on this Session, or nullptr if there
    // is no operation currently running for the Session.
    OperationContext* _currentOperation{nullptr};

    // Specifies whether the session information needs to be refreshed from storage
    bool _isValid{false};

    // Counter, incremented with each call to invalidate in order to discern invalidations, which
    // happen during refresh
    int _numInvalidations{0};

    // Set to true if incomplete history is detected. For example, when the oplog to a write was
    // truncated because it was too old.
    bool _hasIncompleteHistory{false};

    // Indicates the state of the current multi-document transaction or snapshot read, if any.  If
    // the transaction is in any state but kInProgress, no more operations can be collected.
    enum class MultiDocumentTransactionState {
        kNone,
        kInProgress,
        kCommitting,
        kCommitted,
        kAborted
    } _txnState = MultiDocumentTransactionState::kNone;

    // Logs the transaction information if it has run slower than the global parameter slowMS. The
    // transaction must be committed or aborted when this function is called.
    virtual void _logSlowTransaction(WithLock wl,
                                     const SingleThreadedLockStats* lockStats,
                                     MultiDocumentTransactionState terminationCause,
                                     repl::ReadConcernArgs readConcernArgs) = 0;


    // This method returns a string with information about a slow transaction. The format of the
    // logging string produced should match the format used for slow operation logging. A
    // transaction must be completed (committed or aborted) and a valid LockStats reference must be
    // passed in order for this method to be called.
    virtual std::string _transactionInfoForLog(const SingleThreadedLockStats* lockStats,
                                               MultiDocumentTransactionState terminationCause,
                                               repl::ReadConcernArgs readConcernArgs) = 0;

    // Reports transaction stats for both active and inactive transactions using the provided
    // builder.  The lock may be either a lock on _mutex or a lock on _statsMutex.
    virtual void _reportTransactionStats(WithLock wl,
                                         BSONObjBuilder* builder,
                                         repl::ReadConcernArgs readConcernArgs) const = 0;

    // Caches what is known to be the last written transaction record for the session
    boost::optional<SessionTxnRecord> _lastWrittenSessionRecord;

    // Tracks the last seen txn number for the session and is always >= to the transaction number in
    // the last written txn record. When it is > than that in the last written txn record, this
    // means a new transaction has begun on the session, but it hasn't yet performed any writes.
    TxnNumber _activeTxnNumber{kUninitializedTxnNumber};


    // Holds oplog data for operations which have been applied in the current multi-document
    // transaction.  Not used for retryable writes.
    std::vector<repl::ReplOperation> _transactionOperations;

    // Total size in bytes of all operations within the _transactionOperations vector.
    size_t _transactionOperationBytes = 0;

    // For the active txn, tracks which statement ids have been committed and at which oplog
    // opTime. Used for fast retryability check and retrieving the previous write's data without
    // having to scan through the oplog.
    CommittedStatementTimestampMap _activeTxnCommittedStatements;

    // Set in _beginOrContinueTxn and applies to the activeTxn on the session.
    bool _autocommit{true};

    // Set when a snapshot read / transaction begins. Alleviates cache pressure by limiting how long
    // a snapshot will remain open and available. Checked in combination with _txnState to determine
    // whether the transaction should be aborted.
    // This is unset until a transaction begins on the session, and then reset only when new
    // transactions begin.
    boost::optional<Date_t> _transactionExpireDate;

    // The OpTime a speculative transaction is reading from and also the earliest opTime it
    // should wait for write concern for on commit.
    repl::OpTime _speculativeTransactionReadOpTime;

    // This member is only applicable to operations running in a transaction. It is reset when a
    // transaction state resets.
    std::vector<MultikeyPathInfo> _multikeyPathInfo;

    // Protects _singleTransactionStats.  The concurrency rules are that _singleTransactionStats
    // may be read under either _mutex or _statsMutex, but to write both mutexes must be held,
    // with _mutex being taken before _statsMutex.  No other locks, particularly including the
    // Client lock, may be taken while holding _statsMutex.
    mutable stdx::mutex _statsMutex;

    // Tracks metrics for a single multi-document transaction.  Contains only txnNumber for
    // retryable writes.
    SingleTransactionStats _singleTransactionStats;
};

}  // namespace mongo
