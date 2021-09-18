/* Copyright (c) 2018, 2021, Oracle and/or its affiliates.

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

#include "sql/rpl_trx_tracking.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "libbinlogevents/include/binlog_event.h"
#include "my_inttypes.h"
#include "my_sqlcommand.h"
#include "sql/binlog.h"
#include "sql/current_thd.h"
#include "sql/mysqld.h"
#include "sql/rpl_context.h"
#include "sql/rpl_transaction_write_set_ctx.h"
#include "sql/sql_alter.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/system_variables.h"
#include "sql/transaction_info.h"

Logical_clock::Logical_clock() : state(SEQ_UNINIT), offset(0) {}

/**
  Atomically fetch the current state.
  @return  not subtracted "absolute" value.
 */
inline int64 Logical_clock::get_timestamp() {
  int64 retval = 0;
  DBUG_TRACE;
  retval = state.load();
  return retval;
}

/**
  Steps the absolute value of the clock (state) to return
  an updated value.
  The caller must be sure to call the method in no concurrent
  execution context so either offset and state can't change.

  @return  incremented "absolute" value
 */
inline int64 Logical_clock::step() {
  static_assert(SEQ_UNINIT == 0, "");
  DBUG_EXECUTE_IF("logical_clock_step_2", ++state;);
  return ++state;
}

/**
  To try setting the clock *forward*.
  The clock does not change when the new value is in the past
  which is reflected by the new value and by offset.
  In other words the function main effects is described as
    state= max(state, new_value).
  Offset that exceeds the new value indicates the binary log rotation
  to render such new value useless.

  @param  new_val  a new value (offset included)
  @return a (new) value of state member regardless whether it's changed or not.
 */
inline int64 Logical_clock::set_if_greater(int64 new_val) {
  int64 old_val = new_val - 1;
  bool cas_rc;

  DBUG_TRACE;

  assert(new_val > 0);

  if (new_val <= offset) {
    /*
      This function's invocation can be separated from the
      transaction's flushing by few rotations. A late to log
      transaction does not change the clock, similarly to how
      its timestamps are handled at flushing.
    */
    return SEQ_UNINIT;
  }

  assert(new_val > 0);

  while (
      !(cas_rc = atomic_compare_exchange_strong(&state, &old_val, new_val)) &&
      old_val < new_val) {
  }

  assert(state >= new_val);  // setting can't be done to past

  assert(cas_rc || old_val >= new_val);

  return cas_rc ? new_val : old_val;
}

/*
  Admin statements release metadata lock too earlier. It breaks the rule of lock
  based logical clock. This function recognizes the statements.
 */
static bool is_trx_unsafe_for_parallel_slave(const THD *thd) {
  switch (thd->lex->sql_command) {
    case SQLCOM_ANALYZE:
    case SQLCOM_REPAIR:
    case SQLCOM_OPTIMIZE:
    case SQLCOM_CREATE_DB:
    case SQLCOM_ALTER_DB:
    case SQLCOM_DROP_DB:
      return true;
    case SQLCOM_ALTER_TABLE:
      return thd->lex->alter_info->flags & Alter_info::ALTER_ADMIN_PARTITION;
    default:
      return false;
  }
  return false;
}

/**
  Get the sequence_number for a transaction, and get the last_commit based
  on parallel committing transactions.

  @param[in]     thd             Current THD from which to extract trx context.
  @param[in,out] sequence_number Sequence number of current transaction.
  @param[in,out] commit_parent   Commit_parent of current transaction,
                                 pre-filled with the commit_parent calculated
                                 by the logical clock logic.
*/
void Commit_order_trx_dependency_tracker::get_dependency(THD *thd,
                                                         int64 &sequence_number,
                                                         int64 &commit_parent) {
  Transaction_ctx *trn_ctx = thd->get_transaction();

  assert(trn_ctx->sequence_number > m_max_committed_transaction.get_offset());
  /*
    Prepare sequence_number and commit_parent relative to the current
    binlog.  This is done by subtracting the binlog's clock offset
    from the values.

    A transaction that commits after the binlog is rotated, can have a
    commit parent in the previous binlog. In this case, subtracting
    the offset from the sequence number results in a negative
    number. The commit parent dependency gets lost in such
    case. Therefore, we log the value SEQ_UNINIT in this case.
  */
  sequence_number =
      trn_ctx->sequence_number - m_max_committed_transaction.get_offset();

  if (trn_ctx->last_committed <= m_max_committed_transaction.get_offset())
    commit_parent = SEQ_UNINIT;
  else
    commit_parent =
        std::max(trn_ctx->last_committed, m_last_blocking_transaction) -
        m_max_committed_transaction.get_offset();

  if (is_trx_unsafe_for_parallel_slave(thd))
    m_last_blocking_transaction = trn_ctx->sequence_number;
}

int64 Commit_order_trx_dependency_tracker::step() {
  return m_transaction_counter.step();
}

void Commit_order_trx_dependency_tracker::rotate() {
  m_max_committed_transaction.update_offset(
      m_transaction_counter.get_timestamp());

  m_transaction_counter.update_offset(m_transaction_counter.get_timestamp());
}

void Commit_order_trx_dependency_tracker::update_max_committed(
    int64 sequence_number) {
  mysql_mutex_assert_owner(&LOCK_replica_trans_dep_tracker);
  m_max_committed_transaction.set_if_greater(sequence_number);
}

/**
 * 获取一个事务的 writeset dependencies。
 * 这需要先使用 Commit_order_trx_dependency_tracker 得到的 commit_parent，
 * 使用每个事务的 writeset，使 commit_parent 尽可能的低。
 * 
 * 返回的 commit_parent 取决于 writeset_history 中存储了多少 row hases，
 * 一旦达到用户定义的最大值，就会被清除。
 * 
 * @param[in]     thd             当前的 THD，从中提取 trx context.
 * @param[in,out] sequence_number 当前事务的 Sequence number.
 * @param[in,out] commit_parent   当前事务的 Commit_parent,
 * 								  预填入了由 COMMIT_ORDER 的 get dependency 算出的 commit_parent
 * 								  （以便在 WRITESET commit_parent 无效时降级使用）
 */
void
Writeset_trx_dependency_tracker::get_dependency(THD *thd,
												int64 &sequence_number,
												int64 &commit_parent)
{
	// 获取当前 session 的 write_set
  	Rpl_transaction_write_set_ctx *write_set_ctx =
      	thd->get_transaction()->get_transaction_write_set_ctx();
  	std::vector<uint64> *writeset = write_set_ctx->get_write_set();

#ifndef NDEBUG
	/* The writeset of an empty transaction must be empty. */
	if (is_empty_transaction_in_binlog_cache(thd)) assert(writeset->size() == 0);
#endif

	/**
	 * 判断是否可以使用 write_set，主要关注：
	 * 	- 这个事务是否有 writeset（DDL 或类似场景）;
	 * 	- writeset 是否会溢出 history size;
	 * 	- 当前 session 的 transaction_write_set_extraction(hash algo) 是否与 global(history)一致;
	 * 	- 这个事务中引用的表所发生的修改是否是别的表的外键;
	 * 如果发生上述情况，使用 COMMIT_ORDER，并清除 history 以保持数据的一致。
 	 */
  	bool can_use_writesets =
		// empty writeset 意味着 DDL 或类似情况，也就不能并行，除非有 missing keys
		(writeset->size() != 0 || write_set_ctx->get_has_missing_keys()
		// empty transactions 不需要清除 writeset history，因为它们可以并行地执行
		|| is_empty_transaction_in_binlog_cache(thd))
		// session 的 hashing algorithm 必须与 history 中其他行所使用的算法相同
		&& (global_system_variables.transaction_write_set_extraction ==
       			thd->variables.transaction_write_set_extraction)
	    // 不可以使用外键
		&& !write_set_ctx->get_has_related_foreign_keys()
      	// 没有超出 capacity
      	&& !write_set_ctx->was_write_set_limit_reached();

	// write_history 的长度是否超过最大值
	bool exceeds_capacity = false;

  	if (can_use_writesets)
	{
		/**
		 * 检查如果添加该事务后，是否会超过了 writeset history 的容量。
		 * 如果会超过，m_writeset_history 将在当前事务使用完其信息后才被清除。
		 */
    	exceeds_capacity =
        	m_writeset_history.size() + writeset->size() > m_opt_max_history_size;

		// 计算所有冲突中的最大的 sequence_number，并将该事务的 row hases 添加到 history 中
		// 遍历 session 的 writeset，查找在 writeset_history 中的冲突行
        //    - 如果冲突，则更新 last_parent（last_parent 是临时变量，并不是 commit parent）
        //    - 如果没冲突，write_history 没找到最大值，则插入 write_history
    	int64 last_parent = m_writeset_history_start;
    	for (std::vector<uint64>::iterator it = writeset->begin();
         	 it != writeset->end();
			 ++it)
		{
      		Writeset_history::iterator hst = m_writeset_history.find(*it);
      		if (hst != m_writeset_history.end())	// writeset_history 中存在
			{
        		if (hst->second > last_parent && hst->second < sequence_number)
          			last_parent = hst->second;

        		hst->second = sequence_number;
      		}
			else	// writeset_history 中不存在
			{
        		if (!exceeds_capacity)	// 且当前没有超过 writeset history 的容量
          			m_writeset_history.insert(std::pair<uint64, int64>(*it, sequence_number));
      		}
   		}

		/**
		 * 如果事务引用了没有主键的表，则回退到 COMMIT_ORDER，
		 * 这里是更新而不是重置 writeset history，
		 * 因为任何引用这个表的事务也会恢复到 COMMIT_ORDER。
		 */
		if (!write_set_ctx->get_has_missing_keys())
		{
			/**
			 * WRITESET 的 commit_parent 为
			 * 	  - 事务所涉及的 row 的 hases 
			 * 	  - COMMIT_ORDER 计算得到的 commit parent
			 * 的最小值
			 */
			commit_parent = std::min(last_parent, commit_parent);
		}
  	}

	// 如果 writeset_history 已满，或者不可以使用 WriteSet，则清空 WriteSet
	if (exceeds_capacity || !can_use_writesets)
	{
		m_writeset_history_start = sequence_number;
		m_writeset_history.clear();
	}
}

void Writeset_trx_dependency_tracker::rotate(int64 start) {
	m_writeset_history_start = start;
	m_writeset_history.clear();
}

/**
 * 根据 session dependencies 获取事务的 writeset commit parent
 * @param[in]     thd             Current THD from which to extract trx context.
 * @param[in,out] sequence_number Sequence number of current transaction.
 * @param[in,out] commit_parent   Commit_parent of current transaction,
                                  pre-filled with the commit_parent calculated
                                  by the Write_set_trx_dependency_tracker as a
                                  fall-back.
 */
void
Writeset_session_trx_dependency_tracker::get_dependency(
    	THD *thd,
		int64 &sequence_number,
		int64 &commit_parent)
{
  	int64 session_parent = thd->rpl_thd_ctx
	  							.dependency_tracker_ctx()
                             	.get_last_session_sequence_number();

  	if (session_parent != 0 && session_parent < sequence_number)
    	commit_parent = std::max(commit_parent, session_parent);

  	thd->rpl_thd_ctx.dependency_tracker_ctx()
	  				.set_last_session_sequence_number(sequence_number);
}

/**
 * 获取事务中的依赖关系，这是 dependency tracking 的主要入口。
 */
void Transaction_dependency_tracker::get_dependency(THD *thd,
                                                    int64 &sequence_number,
                                                    int64 &commit_parent)
{
  	sequence_number = commit_parent = 0;

  	switch (m_opt_tracking_mode)
	{
    	case DEPENDENCY_TRACKING_COMMIT_ORDER:
			// COMMIT_ORDER 模式只调用 m_commit_order.get_dependency()
			m_commit_order.get_dependency(thd, sequence_number, commit_parent);
			break;
		case DEPENDENCY_TRACKING_WRITESET:
			m_commit_order.get_dependency(thd, sequence_number, commit_parent);
			m_writeset.get_dependency(thd, sequence_number, commit_parent);
			break;
		case DEPENDENCY_TRACKING_WRITESET_SESSION:
			m_commit_order.get_dependency(thd, sequence_number, commit_parent);
			m_writeset.get_dependency(thd, sequence_number, commit_parent);
			m_writeset_session.get_dependency(thd, sequence_number, commit_parent);
			break;
		default:
			assert(0);  // debug build 下直接崩溃
			
			// 在 produnction build 下回退到 COMMIT_ORDER 模式
			m_commit_order.get_dependency(thd, sequence_number, commit_parent);
  	}
}

void Transaction_dependency_tracker::tracking_mode_changed() {
  Logical_clock max_committed_transaction =
      m_commit_order.get_max_committed_transaction();
  int64 timestamp = max_committed_transaction.get_timestamp() -
                    max_committed_transaction.get_offset();

  m_writeset.rotate(timestamp);
}

/**
  The method is to be executed right before committing time.
  It must be invoked even if the transaction does not commit
  to engine being merely logged into the binary log.
  max_committed_transaction is updated with a greater timestamp
  value.
  As a side effect, the transaction context's sequence_number
  is reset.

  @param thd a pointer to THD instance
*/
void Transaction_dependency_tracker::update_max_committed(THD *thd) {
  Transaction_ctx *trn_ctx = thd->get_transaction();
  m_commit_order.update_max_committed(trn_ctx->sequence_number);
  /*
    sequence_number timestamp isn't needed anymore, so it's cleared off.
  */
  trn_ctx->sequence_number = SEQ_UNINIT;

  assert(trn_ctx->last_committed == SEQ_UNINIT ||
         thd->commit_error == THD::CE_FLUSH_ERROR ||
         thd->commit_error == THD::CE_FLUSH_GNO_EXHAUSTED_ERROR);
}

int64 Transaction_dependency_tracker::step() { return m_commit_order.step(); }

void Transaction_dependency_tracker::rotate() {
  m_commit_order.rotate();
  /*
    To make slave appliers be able to execute transactions in parallel
    after rotation, set the minimum commit_parent to 1 after rotation.
  */
  m_writeset.rotate(1);
  if (current_thd) current_thd->get_transaction()->sequence_number = 2;
}

int64 Transaction_dependency_tracker::get_max_committed_timestamp() {
  return m_commit_order.get_max_committed_transaction().get_timestamp();
}
