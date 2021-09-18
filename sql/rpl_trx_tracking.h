#ifndef RPL_TRX_TRACKING_INCLUDED
/* Copyright (c) 2017, 2021, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#define RPL_TRX_TRACKING_INCLUDED

#include <assert.h>
#include <sys/types.h>
#include <atomic>
#include <map>

#include "libbinlogevents/include/binlog_event.h"

#include "my_inttypes.h"

// 感觉可以理解为一个 session 对应的 thread
class THD;

// 缩写：MTS -> Multi-Threaded Slaves

/**
  Logical timestamp generator for logical timestamping binlog transactions.
  A transaction is associated with two sequence numbers see
  @c Transaction_ctx::last_committed and @c Transaction_ctx::sequence_number.
  The class provides necessary interfaces including that of
  generating a next consecutive value for the latter.
*/
class Logical_clock {
private:
	std::atomic<int64> state;
	/*
		Offset is subtracted from the actual "absolute time" value at
		logging a replication event. That is the event holds logical
		timestamps in the "relative" format. They are meaningful only in
		the context of the current binlog.
		The member is updated (incremented) per binary log rotation.
	*/
	int64 offset;

public:
	Logical_clock();
	Logical_clock(const Logical_clock &other)
		: state(other.state.load()), offset(other.offset) {}

	int64 step();
	int64 set_if_greater(int64 new_val);
	int64 get_timestamp();
	int64 get_offset() { return offset; }
	/*
		Updates the offset.
		This operation is invoked when binlog rotates and at that time
		there can't any concurrent step() callers so no need to guard
		the assignement.
	*/
	void update_offset(int64 new_offset) {
		assert(offset <= new_offset);

		offset = new_offset;
	}
	~Logical_clock() = default;
};

/**
  Generate logical timestamps for MTS using COMMIT_ORDER
  in the binlog-transaction-dependency-tracking option.
*/
class Commit_order_trx_dependency_tracker {
public:
	/**
		Main function that gets the dependencies using the COMMIT_ORDER tracker.

		@param [in]     thd             THD of the caller.
		@param [in,out] sequence_number sequence_number initialized and returned.
		@param [in,out] commit_parent   commit_parent to be returned.
	*/
	void get_dependency(THD *thd, int64 &sequence_number, int64 &commit_parent);

	void update_max_committed(int64 sequence_number);

	Logical_clock get_max_committed_transaction() {
		return m_max_committed_transaction;
	}

	int64 step();
	void rotate();

private:
	/* Committed transactions timestamp */
	Logical_clock m_max_committed_transaction;

	/* "Prepared" transactions timestamp */
	Logical_clock m_transaction_counter;

	/*
		Stores the last sequence_number of the transaction which breaks the rule of
		lock based logical clock. commit_parent of the following transactions
		will be set to m_last_blocking_transaction if their last_committed is
		smaller than m_last_blocking_transaction.
	*/
	int64 m_last_blocking_transaction = SEQ_UNINIT;
};

/**
 * 使用 WRITESET 配置下为 MTS(Multi-Threaded Slaves) 生成逻辑时间戳。
 */
class Writeset_trx_dependency_tracker {
public:
	Writeset_trx_dependency_tracker(ulong max_history_size)
		: m_opt_max_history_size(max_history_size)
		, m_writeset_history_start(0) {}

	/**
	 * 使用 WRITESET tracker 获得依赖关系的 Main function。
	 * @param [in]     thd             调用者的 THD.
	 * @param [in,out] sequence_number sequence_number initialized and returned.
	 * @param [in,out] commit_parent   commit_parent to be returned.
	 */
	void get_dependency(THD *thd, int64 &sequence_number, int64 &commit_parent);

	void rotate(int64 start);

	/* Atomic variable - opt_binlog_transaction_dependency_history_size */
	std::atomic<ulong> m_opt_max_history_size;

private:
	/**
	 * 当逻辑时钟源为 WRITE_SET 时，
	 * 记录作为最小的 commit parent 的最后一个 write-set 事务。
	 * 即不在该 history 中的最新的事务，或者当 history 为空时为 0。
	 * 
	 * m_writeset_history_start 最初必须设置为 0。
	 * 每当 binlog_transaction_dependency_tracking（事务并发的依赖模式）
	 * 被改变或 history 被清除时，它就会更新为第一个被检查 dependencies 的事务。
 	 */
	int64 m_writeset_history_start;

	/**
	 * 记录改变数据库中每条记录的最后一个 transaction sequence number，
	 * 使用来自 Writeset 的 row hashes 作为索引。
	 */
	typedef std::map<uint64, int64> Writeset_history;
	Writeset_history m_writeset_history;
};

/**
 * 使用 WRITESET_SESSION 配置下为 MTS 生成逻辑时间戳
 */
class Writeset_session_trx_dependency_tracker {
public:
	/**
	 * 使用 WRITESET_SESSION tracker 获得依赖关系的 Main function。
	 * @param [in]     thd             THD of the caller.
	 * @param [in,out] sequence_number sequence_number initialized and returned.
	 * @param [in,out] commit_parent   commit_parent to be returned.
	 */
	void get_dependency(THD *thd, int64 &sequence_number, int64 &commit_parent);
};

/**
  Modes for parallel transaction dependency tracking
*/
enum enum_binlog_transaction_dependency_tracking {
	/**
	 * Track 基于事务的 commit order 的依赖关系。
	 * 追踪任何事务持有其所有锁的时间间隔（该间隔在存储引擎提交之前结束，此时锁被释放。对于一个自动提交的事务，它开始于存储引擎准备之前。对于BEGIN...COMMIT事务，它开始于COMMIT前的最后一条语句的结束）。) 如果两个事务的时间间隔重合，则被标记为不冲突。换句话说，如果trx1在binlog中出现在trx2之前，并且trx2在trx1释放其锁之前已经获得了所有的锁，那么trx2就被标记为，从属系统可以将其与trx1并行安排。
		Tracks dependencies based on the commit order of transactions.
		The time intervals during which any transaction holds all its locks are
		tracked (the interval ends just before storage engine commit, when locks
		are released. For an autocommit transaction it begins just before storage
		engine prepare. For BEGIN..COMMIT transactions it begins at the end of the
		last statement before COMMIT). Two transactions are marked as
		non-conflicting if their respective intervals overlap. In other words,
		if trx1 appears before trx2 in the binlog, and trx2 had acquired all its
		locks before trx1 released its locks, then trx2 is marked such that the
		slave can schedule it in parallel with trx1.
	*/
	DEPENDENCY_TRACKING_COMMIT_ORDER = 0,

	/**
	 * 根据 updated rows 的集合来判断依赖关系。
	 * 任何两个修改不相干的 row set 的事务都被认为是并发的、非冲突的。
	 */
	DEPENDENCY_TRACKING_WRITESET = 1,

	/**
	 * 根据 per session 的 updated rows 的集合来判断依赖关系。
	 * 任何两个来自不同 session 的，修改了不相干的 row set 的事务都被认为是并发的、非冲突的。
	 * 来自同一 session 的事务总认为是有依赖关系的，也就不会是可并发的、非冲突的。
	 */
	DEPENDENCY_TRACKING_WRITESET_SESSION = 2
};

/**
  Dependency tracker is a container singleton that dispatches between the three
  methods associated with the binlog-transaction-dependency-tracking option.
  There is a singleton instance of each of these classes.
*/
/**
 * Dependency tracker 是一个 container 单例，作为 MYSQL_BIN_LOG 的成员
 * 在与 binlog-transaction-dependency-tracking 选项相关的三种方法之间进行调度。
 * 这些方法中的每一个都有一个单例。
 */
class Transaction_dependency_tracker {
public:
    Transaction_dependency_tracker()
    	: m_opt_tracking_mode(DEPENDENCY_TRACKING_COMMIT_ORDER)	// 默认为 COMMIT_ORDER
      	, m_writeset(25000) {}

	// 在 sql/binlog.cc:write_transaction 中调用
  	void get_dependency(THD *thd, int64 &sequence_number, int64 &commit_parent);

	void tracking_mode_changed();

	void update_max_committed(THD *thd);
	int64 get_max_committed_timestamp();

	int64 step();
	void rotate();

public:
	/* option opt_binlog_transaction_dependency_tracking */
	long m_opt_tracking_mode;

  	Writeset_trx_dependency_tracker *get_writeset() { return &m_writeset; }

private:
	Writeset_trx_dependency_tracker m_writeset;
	Commit_order_trx_dependency_tracker m_commit_order;
	Writeset_session_trx_dependency_tracker m_writeset_session;
};

#endif /* RPL_TRX_TRACKING_INCLUDED */
