// Copyright (c) 2012, Cloudera, inc.
#ifndef KUDU_TABLET_TABLET_H
#define KUDU_TABLET_TABLET_H

#include <iosfwd>
#include <string>
#include <vector>

#include "kudu/common/iterator.h"
#include "kudu/common/predicate_encoder.h"
#include "kudu/common/schema.h"
#include "kudu/gutil/atomicops.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/macros.h"
#include "kudu/server/metadata.h"
#include "kudu/tablet/lock_manager.h"
#include "kudu/tablet/mvcc.h"
#include "kudu/tablet/rowset.h"
#include "kudu/util/locks.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"

namespace kudu {

class MemTracker;
class MetricContext;
class RowChangeList;
class UnionIterator;

namespace log {
class OpIdAnchorRegistry;
}

namespace server {
class Clock;
}

class MaintenanceManager;
class MaintenanceOp;
struct MaintenanceOpStats;

namespace tablet {

using std::string;
using std::tr1::shared_ptr;

class AlterSchemaTransactionState;
class CompactionPolicy;
class MemRowSet;
class MvccSnapshot;
class PreparedRowWrite;
class RowSetsInCompaction;
class RowSetTree;
struct TabletComponents;
struct TabletMetrics;
class WriteTransactionState;

class Tablet {
 public:
  friend class CompactRowSetsOp;
  friend class FlushMRSOp;

  class CompactionFaultHooks;
  class FlushCompactCommonHooks;
  class FlushFaultHooks;
  class Iterator;

  // Create a new tablet.
  //
  // If 'parent_metrics_context' is non-NULL, then this tablet will store
  // metrics in a sub-context of this context. Otherwise, no metrics are collected.
  //
  // TODO allow passing in a server-wide parent MemTracker.
  Tablet(const scoped_refptr<metadata::TabletMetadata>& metadata,
         const scoped_refptr<server::Clock>& clock,
         const MetricContext* parent_metric_context,
         log::OpIdAnchorRegistry* opid_anchor_registry);

  ~Tablet();

  // Open the tablet.
  Status Open();

  // Actually start a write transaction.
  //
  // Starts an MVCC transaction and assigns a timestamp for the transaction.
  // This also snapshots the current set of tablet components into the transaction
  // state.
  //
  // This should always be done _after_ any relevant row locks are acquired
  // (using CreatePreparedInsert/CreatePreparedMutate). This ensures that,
  // within each row, timestamps only move forward. If we took a timestamp before
  // getting the row lock, we could have the following situation:
  //
  //   Thread 1         |  Thread 2
  //   ----------------------
  //   Start tx 1       |
  //                    |  Start tx 2
  //                    |  Obtain row lock
  //                    |  Update row
  //                    |  Commit tx 2
  //   Obtain row lock  |
  //   Delete row       |
  //   Commit tx 1
  //
  // This would cause the mutation list to look like: @t1: DELETE, @t2: UPDATE
  // which is invalid, since we expect to be able to be able to replay mutations
  // in increasing timestamp order on a given row.
  //
  // This requirement is basically two-phase-locking: the order in which row locks
  // are acquired for transactions determines their serialization order. If/when
  // we support multi-node serializable transactions, we'll have to acquire _all_
  // row locks (across all nodes) before obtaining a timestamp.
  void StartTransaction(WriteTransactionState* tx_state);

  // Same as the method above, but starts the transaction at a specified timestamp
  // instead of acquiring one from the clock.
  void StartTransactionAtTimestamp(WriteTransactionState* tx_state,
                                   Timestamp timestamp);

  // TODO update tests so that we can remove Insert() and Mutate()
  // and use only InsertUnlocked() and MutateUnlocked().

  // Creates a PreparedRowWrite with write_type() INSERT, acquires the row lock
  // for the row and creates a probe for later use. 'row_write' is set to the
  // PreparedRowWrite if this method returns OK.
  //
  // TODO when we get to remove the locked versions of Insert/Mutate we
  // can make the PreparedRowWrite own the row and can revert to passing just
  // the raw row data, but right now we need to pass the built ConstContinuousRow
  // as there are cases where row is passed as a reference (old API).
  Status CreatePreparedInsert(const WriteTransactionState* tx_state,
                              const ConstContiguousRow* row,
                              gscoped_ptr<PreparedRowWrite>* row_write);

  // Insert a new row into the tablet.
  //
  // The provided 'data' slice should have length equivalent to this
  // tablet's Schema.byte_size().
  //
  // After insert, the row and any referred-to memory (eg for strings)
  // have been copied into internal memory, and thus the provided memory
  // buffer may safely be re-used or freed.
  //
  // Returns Status::AlreadyPresent() if an entry with the same key is already
  // present in the tablet.
  // Returns Status::OK unless allocation fails.
  Status InsertForTesting(WriteTransactionState *tx_state, const ConstContiguousRow& row);

  // A version of Insert that does not acquire locks and instead assumes that
  // they were already acquired. Requires that handles for the relevant locks
  // and Mvcc transaction are present in the transaction context.
  Status InsertUnlocked(WriteTransactionState *tx_state,
                        const PreparedRowWrite* insert);

  // Creates a PreparedRowWrite with write_type() MUTATE, acquires the row lock
  // for the row and creates a probe for later use. 'row_write' is set to the
  // PreparedRowWrite if this method returns OK.
  //
  // TODO when we get to remove the locked versions of Insert/Mutate we
  // can make the PreparedRowWrite own the row and can revert to passing just
  // the raw row data, but right now we need to pass the built ConstContinuousRow
  // as there are cases where row is passed as a reference (old API).
  Status CreatePreparedMutate(const WriteTransactionState* tx_state,
                              const ConstContiguousRow* row_key,
                              const RowChangeList& changelist,
                              gscoped_ptr<PreparedRowWrite>* row_write);

  // Update a row in this tablet.
  // The specified schema is the full user schema necessary to decode
  // the update RowChangeList.
  //
  // If the row does not exist in this tablet, returns
  // Status::NotFound().
  Status MutateRowForTesting(WriteTransactionState *tx_state,
                             const ConstContiguousRow& row_key,
                             const Schema& update_schema,
                             const RowChangeList& update);

  // A version of MutateRow that does not acquire locks and instead assumes
  // they were already acquired. Requires that handles for the relevant locks
  // and Mvcc transaction are present in the transaction context.
  Status MutateRowUnlocked(WriteTransactionState *tx_state,
                           const PreparedRowWrite* mutate);

  // Create a new row iterator which yields the rows as of the current MVCC
  // state of this tablet.
  // The returned iterator is not initialized.
  Status NewRowIterator(const Schema &projection,
                        gscoped_ptr<RowwiseIterator> *iter) const;

  // Create a new row iterator for some historical snapshot.
  Status NewRowIterator(const Schema &projection,
                        const MvccSnapshot &snap,
                        gscoped_ptr<RowwiseIterator> *iter) const;

  // Flush the current MemRowSet for this tablet to disk. This swaps
  // in a new (initially empty) MemRowSet in its place.
  //
  // This doesn't flush any DeltaMemStores for any existing RowSets.
  // To do that, call FlushBiggestDMS() for example.
  Status Flush();

  // Prepares the transaction context for the alter schema operation.
  // An error will be returned if the specified schema is invalid (e.g.
  // key mismatch, or missing IDs)
  //
  // TODO: need to somehow prevent concurrent operations while an ALTER
  // is prepared (see KUDU-382).
  Status CreatePreparedAlterSchema(AlterSchemaTransactionState *tx_state,
                                   const Schema* schema);

  // Apply the Schema of the specified transaction.
  // This operation will trigger a flush on the current MemRowSet and on
  // all the DeltaMemStores.
  Status AlterSchema(AlterSchemaTransactionState* tx_state);

  // Prints current RowSet layout, taking a snapshot of the current RowSet interval
  // tree. Optionally prints XML header
  void PrintRSLayout(std::ostream* o, bool header = false);

  // Flags to change the behavior of compaction.
  enum CompactFlag {
    COMPACT_NO_FLAGS = 0,

    // Force the compaction to include all rowsets, regardless of the
    // configured compaction policy. This is currently only used in
    // tests.
    FORCE_COMPACT_ALL = 1 << 0
  };
  typedef int CompactFlags;

  Status Compact(CompactFlags flags);

  // Update the statistics for performing a compaction.
  void UpdateCompactionStats(MaintenanceOpStats* stats);

  // Returns the exact current size of the MRS, in bytes.
  // This method takes a read lock on component_lock_ and is thread-safe.
  size_t MemRowSetSize() const;

  // Estimate the total on-disk size of this tablet, in bytes.
  size_t EstimateOnDiskSize() const;

  // Get the total size of all the DMS
  size_t DeltaMemStoresSize() const;

  // Flush only the biggest DMS
  Status FlushBiggestDMS();

  // Finds the RowSet which has the most separate delta files and
  // issues a minor delta compaction.
  Status MinorCompactWorstDeltas();

  // Return the current number of rowsets in the tablet.
  size_t num_rowsets() const;

  // Attempt to count the total number of rows in the tablet.
  // This is not super-efficient since it must iterate over the
  // memrowset in the current implementation.
  Status CountRows(uint64_t *count) const;


  // Verbosely dump this entire tablet to the logs. This is only
  // really useful when debugging unit tests failures where the tablet
  // has a very small number of rows.
  Status DebugDump(vector<string> *lines = NULL);

  shared_ptr<Schema> schema_unlocked() const {
    // TODO: locking on the schema (KUDU-382)
    return schema_;
  }

  shared_ptr<Schema> schema() const {
    boost::shared_lock<rw_spinlock> lock(component_lock_);
    return schema_;
  }

  // Returns a reference to the key projection of the tablet schema.
  // The schema keys are immutable.
  const Schema& key_schema() const { return key_schema_; }

  // Return the MVCC manager for this tablet.
  MvccManager* mvcc_manager() { return &mvcc_; }

  // Return the Lock Manager for this tablet
  LockManager* lock_manager() { return &lock_manager_; }

  const metadata::TabletMetadata *metadata() const { return metadata_.get(); }
  metadata::TabletMetadata *metadata() { return metadata_.get(); }

  void SetCompactionHooksForTests(const shared_ptr<CompactionFaultHooks> &hooks);
  void SetFlushHooksForTests(const shared_ptr<FlushFaultHooks> &hooks);
  void SetFlushCompactCommonHooksForTests(const shared_ptr<FlushCompactCommonHooks> &hooks);

  // Returns the current MemRowSet id, for tests.
  // This method takes a read lock on component_lock_ and is thread-safe.
  int32_t CurrentMrsIdForTests() const;

  // Runs a major delta major compaction on columns at specified
  // indexes in 'input_rowset'; 'column_indexes' must be sorted.
  // NOTE: RowSet must presently be a DiskRowSet. (Perhaps the API should be
  // a shared_ptr API for now?)
  //
  // TODO: Handle MVCC to support MemRowSet and handle deltas in DeltaMemStore
  Status DoMajorDeltaCompaction(const metadata::ColumnIndexes& column_indexes,
                                shared_ptr<RowSet> input_rowset);

  // Method used by tests to retrieve all rowsets of this table. This
  // will be removed once code for selecting the appropriate RowSet is
  // finished and delta files is finished is part of Tablet class.
  void GetRowSetsForTests(vector<shared_ptr<RowSet> >* out);

  // Register the maintenance ops associated with this tablet
  void RegisterMaintenanceOps(MaintenanceManager* maintenance_manager);

  // Unregister the maintenance ops associated with this tablet.
  // This method is not thread safe.
  void UnregisterMaintenanceOps();

  const std::string& tablet_id() const { return metadata_->oid(); }

  // Return the metrics for this tablet.
  // May be NULL in unit tests, etc.
  TabletMetrics* metrics() { return metrics_.get(); }

  // Return handle to the metric context of this tablet.
  const MetricContext* GetMetricContext() const { return metric_context_.get(); }

  // Return true if 'fname' is a valid filename for a tablet.
  static bool IsTabletFileName(const std::string& fname);

  log::OpIdAnchorRegistry* opid_anchor_registry() {
    return opid_anchor_registry_;
  }

 private:
  friend class Iterator;

  Status FlushUnlocked();

  // Capture a set of iterators which, together, reflect all of the data in the tablet.
  //
  // These iterators are not true snapshot iterators, but they are safe against
  // concurrent modification. They will include all data that was present at the time
  // of creation, and potentially newer data.
  //
  // The returned iterators are not Init()ed.
  // 'projection' must remain valid and unchanged for the lifetime of the returned iterators.
  Status CaptureConsistentIterators(const Schema *projection,
                                    const MvccSnapshot &snap,
                                    const ScanSpec *spec,
                                    vector<shared_ptr<RowwiseIterator> > *iters) const;

  Status PickRowSetsToCompact(RowSetsInCompaction *picked,
                              CompactFlags flags) const;

  Status DoCompactionOrFlush(const Schema& schema,
                             const RowSetsInCompaction &input,
                             int64_t mrs_being_flushed);

  Status FlushMetadata(const RowSetVector& to_remove,
                       const metadata::RowSetMetadataVector& to_add,
                       int64_t mrs_being_flushed);

  static void ModifyRowSetTree(const RowSetTree& old_tree,
                               const RowSetVector& rowsets_to_remove,
                               const RowSetVector& rowsets_to_add,
                               RowSetTree* new_tree);

  // Swap out a set of rowsets, atomically replacing them with the new rowset
  // under the lock.
  void AtomicSwapRowSets(const RowSetVector &to_remove,
                         const RowSetVector &to_add);

  // Same as the above, but without taking the lock. This should only be used
  // in cases where the lock is already held.
  void AtomicSwapRowSetsUnlocked(const RowSetVector &to_remove,
                                 const RowSetVector &to_add);

  // Delete the underlying storage for the input layers in a compaction.
  Status DeleteCompactionInputs(const RowSetsInCompaction &input);

  void GetComponents(scoped_refptr<TabletComponents>* comps) const {
    boost::shared_lock<rw_spinlock> lock(component_lock_);
    *comps = components_;
  }

  // Create a new MemRowSet with the specified 'schema' and replace the current one.
  // The 'old_ms' pointer will be set to the current MemRowSet set before the replacement.
  // If the MemRowSet is not empty it will be added to the 'compaction' input
  // and the MemRowSet compaction lock will be taken to prevent the inclusion
  // in any concurrent compactions.
  Status ReplaceMemRowSetUnlocked(const Schema& schema,
                                  RowSetsInCompaction *compaction,
                                  shared_ptr<MemRowSet> *old_ms);

  // TODO: Document me.
  Status FlushInternal(const RowSetsInCompaction& input,
               const shared_ptr<MemRowSet>& old_ms,
               const Schema& schema);

  BloomFilterSizing bloom_sizing() const;

  // Convert the specified read client schema (without IDs) to a server schema (with IDs)
  // This method is used by NewRowIterator().
  Status GetMappedReadProjection(const Schema& projection,
                                 Schema *mapped_projection) const;

  Status CheckRowInTablet(const tablet::RowSetKeyProbe& probe) const;

  shared_ptr<Schema> schema_;
  const Schema key_schema_;
  scoped_refptr<metadata::TabletMetadata> metadata_;

  // The current components of the tablet. These should always be read
  // or swapped under the component_lock.
  scoped_refptr<TabletComponents> components_;
  log::OpIdAnchorRegistry* opid_anchor_registry_;
  std::tr1::shared_ptr<MemTracker> mem_tracker_;
  shared_ptr<MemRowSet> memrowset_;
  shared_ptr<RowSetTree> rowsets_;

  gscoped_ptr<MetricContext> metric_context_;
  gscoped_ptr<TabletMetrics> metrics_;

  int64_t next_mrs_id_;

  // A pointer to the server's clock.
  scoped_refptr<server::Clock> clock_;

  MvccManager mvcc_;
  LockManager lock_manager_;

  gscoped_ptr<CompactionPolicy> compaction_policy_;

  // Lock protecting access to the 'components_' member (i.e the rowsets in the tablet)
  //
  // Shared mode:
  // - Writers take this in shared mode at the same time as they obtain an MVCC timestamp
  //   and capture a reference to components_. This ensures that we can use the MVCC timestamp
  //   to determine which writers are writing to which components during compaction.
  // - Readers take this in shared mode while capturing their iterators. This ensures that
  //   they see a consistent view when racing against flush/compact.
  //
  // Exclusive mode:
  // - Flushes/compactions take this lock in order to lock out concurrent updates when
  //   swapping in a new memrowset.
  //
  // NOTE: callers should avoid taking this lock for a long time, even in shared mode.
  // This is because the lock has some concept of fairness -- if, while a long reader
  // is active, a writer comes along, then all future short readers will be blocked.
  // TODO: now that this is single-threaded again, we should change it to rw_spinlock
  mutable rw_spinlock component_lock_;

  // Lock protecting the selection of rowsets for compaction.
  // Only one thread may run the compaction selection algorithm at a time
  // so that they don't both try to select the same rowset.
  mutable boost::mutex compact_select_lock_;

  // We take this lock when flushing the tablet's rowsets in Tablet::Flush.  We
  // don't want to have two flushes in progress at once, in case the one which
  // started earlier completes after the one started later.
  mutable boost::mutex rowsets_flush_lock_;

  bool open_;

  // Fault hooks. In production code, these will always be NULL.
  shared_ptr<CompactionFaultHooks> compaction_hooks_;
  shared_ptr<FlushFaultHooks> flush_hooks_;
  shared_ptr<FlushCompactCommonHooks> common_hooks_;

  std::vector<MaintenanceOp*> maintenance_ops_;

  DISALLOW_COPY_AND_ASSIGN(Tablet);
};


// Hooks used in test code to inject faults or other code into interesting
// parts of the compaction code.
class Tablet::CompactionFaultHooks {
 public:
  virtual Status PostSelectIterators() { return Status::OK(); }
  virtual ~CompactionFaultHooks() {}
};

class Tablet::FlushCompactCommonHooks {
 public:
  virtual Status PostTakeMvccSnapshot() { return Status::OK(); }
  virtual Status PostWriteSnapshot() { return Status::OK(); }
  virtual Status PostSwapInDuplicatingRowSet() { return Status::OK(); }
  virtual Status PostReupdateMissedDeltas() { return Status::OK(); }
  virtual Status PostSwapNewRowSet() { return Status::OK(); }
  virtual ~FlushCompactCommonHooks() {}
};

// Hooks used in test code to inject faults or other code into interesting
// parts of the Flush() code.
class Tablet::FlushFaultHooks {
 public:
  virtual Status PostSwapNewMemRowSet() { return Status::OK(); }
  virtual ~FlushFaultHooks() {}
};

class Tablet::Iterator : public RowwiseIterator {
 public:
  virtual ~Iterator();

  virtual Status Init(ScanSpec *spec) OVERRIDE;

  virtual bool HasNext() const OVERRIDE;

  virtual Status NextBlock(RowBlock *dst) OVERRIDE;

  string ToString() const OVERRIDE;

  const Schema &schema() const OVERRIDE {
    return projection_;
  }

  virtual void GetIteratorStats(std::vector<IteratorStats>* stats) const OVERRIDE;

 private:
  friend class Tablet;

  DISALLOW_COPY_AND_ASSIGN(Iterator);

  Iterator(const Tablet *tablet,
           const Schema &projection,
           const MvccSnapshot &snap);

  const Tablet *tablet_;
  Schema projection_;
  const MvccSnapshot snap_;
  gscoped_ptr<UnionIterator> iter_;
  RangePredicateEncoder encoder_;
};

// Structure which represents the components of the tablet's storage.
// This structure is immutable -- a transaction can grab it and be sure
// that it won't change.
struct TabletComponents : public RefCountedThreadSafe<TabletComponents> {
  TabletComponents(const shared_ptr<MemRowSet>& mrs,
                   const shared_ptr<RowSetTree>& rs_tree);
  const shared_ptr<MemRowSet> memrowset;
  const shared_ptr<RowSetTree> rowsets;
};

} // namespace tablet
} // namespace kudu

#endif
