/* Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "xa.h"

#include "hash.h"               // HASH
#include "sql_class.h"          // THD
#include "transaction.h"        // trans_begin, trans_rollback
#include "debug_sync.h"         // DEBUG_SYNC
#include "log.h"                // tc_log
#include "sql_plugin.h"         // plugin_foreach
#include <pfs_transaction_provider.h>
#include <mysql/psi/mysql_transaction.h>
#include "binlog.h"

const char *XID_STATE::xa_state_names[]={
  "NON-EXISTING", "ACTIVE", "IDLE", "PREPARED", "ROLLBACK ONLY"
};

/* for recover() handlerton call */
static const int MIN_XID_LIST_SIZE= 128;
static const int MAX_XID_LIST_SIZE= 1024*128;

static mysql_mutex_t LOCK_transaction_cache;
static HASH transaction_cache;

static my_bool xacommit_handlerton(THD *unused1, plugin_ref plugin,
                                   void *arg)
{
  handlerton *hton= plugin_data<handlerton*>(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->recover)
    hton->commit_by_xid(hton, (XID *)arg);

  return FALSE;
}


static my_bool xarollback_handlerton(THD *unused1, plugin_ref plugin,
                                     void *arg)
{
  handlerton *hton= plugin_data<handlerton*>(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->recover)
    hton->rollback_by_xid(hton, (XID *)arg);

  return FALSE;
}


static void ha_commit_or_rollback_by_xid(THD *thd, XID *xid, bool commit)
{
  plugin_foreach(NULL, commit ? xacommit_handlerton : xarollback_handlerton,
                 MYSQL_STORAGE_ENGINE_PLUGIN, xid);
}


struct xarecover_st
{
  int len, found_foreign_xids, found_my_xids;
  XID *list;
  HASH *commit_list;
  bool dry_run;
};


static my_bool xarecover_handlerton(THD *unused, plugin_ref plugin,
                                    void *arg)
{
  handlerton *hton= plugin_data<handlerton*>(plugin);
  struct xarecover_st *info= (struct xarecover_st *) arg;
  int got;

  if (hton->state == SHOW_OPTION_YES && hton->recover)
  {
    while ((got= hton->recover(hton, info->list, info->len)) > 0)
    {
      sql_print_information("Found %d prepared transaction(s) in %s",
                            got, ha_resolve_storage_engine_name(hton));
      for (int i= 0; i < got; i++)
      {
        my_xid x= info->list[i].get_my_xid();
        if (!x) // not "mine" - that is generated by external TM
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE * 4 + 6]; // see xid_to_str
          XID *xid= info->list + i;
          sql_print_information("ignore xid %s", xid->xid_to_str(buf));
#endif
          transaction_cache_insert_recovery(info->list + i);
          info->found_foreign_xids++;
          continue;
        }
        if (info->dry_run)
        {
          info->found_my_xids++;
          continue;
        }
        // recovery mode
        if (info->commit_list ?
            my_hash_search(info->commit_list, (uchar *)&x, sizeof(x)) != 0 :
            tc_heuristic_recover == TC_HEURISTIC_RECOVER_COMMIT)
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE * 4 + 6]; // see xid_to_str
          XID *xid= info->list + i;
          sql_print_information("commit xid %s", xid->xid_to_str(buf));
#endif
          hton->commit_by_xid(hton, info->list + i);
        }
        else
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE * 4 + 6]; // see xid_to_str
          XID *xid= info->list + i;
          sql_print_information("rollback xid %s", xid->xid_to_str(buf));
#endif
          hton->rollback_by_xid(hton, info->list + i);
        }
      }
      if (got < info->len)
        break;
    }
  }
  return false;
}


int ha_recover(HASH *commit_list)
{
  struct xarecover_st info;
  DBUG_ENTER("ha_recover");
  info.found_foreign_xids= info.found_my_xids= 0;
  info.commit_list= commit_list;
  info.dry_run= (info.commit_list == 0 && tc_heuristic_recover == 0);
  info.list= NULL;

  /* commit_list and tc_heuristic_recover cannot be set both */
  DBUG_ASSERT(info.commit_list == 0 || tc_heuristic_recover == 0);
  /* if either is set, total_ha_2pc must be set too */
  DBUG_ASSERT(info.dry_run || total_ha_2pc>(ulong)opt_bin_log);

  if (total_ha_2pc <= (ulong)opt_bin_log)
    DBUG_RETURN(0);

  if (info.commit_list)
    sql_print_information("Starting crash recovery...");

  if (total_ha_2pc > (ulong)opt_bin_log + 1)
  {
    if (tc_heuristic_recover == TC_HEURISTIC_RECOVER_ROLLBACK)
    {
      sql_print_error("--tc-heuristic-recover rollback strategy is not safe "
                      "on systems with more than one 2-phase-commit-capable "
                      "storage engine. Aborting crash recovery.");
      DBUG_RETURN(1);
    }
  }
  else
  {
    /*
      If there is only one 2pc capable storage engine it is always safe
      to rollback. This setting will be ignored if we are in automatic
      recovery mode.
    */
    tc_heuristic_recover= TC_HEURISTIC_RECOVER_ROLLBACK; // forcing ROLLBACK
    info.dry_run= false;
  }

  for (info.len= MAX_XID_LIST_SIZE ;
       info.list == 0 && info.len > MIN_XID_LIST_SIZE; info.len/= 2)
  {
    info.list= (XID *)my_malloc(key_memory_XID,
                                info.len * sizeof(XID), MYF(0));
  }
  if (!info.list)
  {
    sql_print_error(ER(ER_OUTOFMEMORY),
                    static_cast<int>(info.len * sizeof(XID)));
    DBUG_RETURN(1);
  }

  plugin_foreach(NULL, xarecover_handlerton,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &info);

  my_free(info.list);
  if (info.found_foreign_xids)
    sql_print_warning("Found %d prepared XA transactions",
                      info.found_foreign_xids);
  if (info.dry_run && info.found_my_xids)
  {
    sql_print_error("Found %d prepared transactions! It means that mysqld was "
                    "not shut down properly last time and critical recovery "
                    "information (last binlog or %s file) was manually deleted"
                    " after a crash. You have to start mysqld with "
                    "--tc-heuristic-recover switch to commit or rollback "
                    "pending transactions.",
                    info.found_my_xids, opt_tc_log_file);
    DBUG_RETURN(1);
  }
  if (info.commit_list)
    sql_print_information("Crash recovery finished.");
  DBUG_RETURN(0);
}


/**
  Rollback the active XA transaction.

  @note Resets rm_error before calling ha_rollback(), so
        the thd->transaction.xid structure gets reset
        by ha_rollback() / THD::transaction::cleanup().

  @return true if the rollback failed, false otherwise.
*/

static bool xa_trans_force_rollback(THD *thd)
{
  /*
    We must reset rm_error before calling ha_rollback(),
    so thd->transaction.xid structure gets reset
    by ha_rollback()/THD::transaction::cleanup().
  */
  thd->get_transaction()->xid_state()->reset_error();
  if (ha_rollback_trans(thd, true))
  {
    my_error(ER_XAER_RMERR, MYF(0));
    return true;
  }
  return false;
}


/**
  Commit and terminate a XA transaction.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_commit::trans_xa_commit(THD *thd)
{
  bool res= true;
  XID_STATE *xid_state= thd->get_transaction()->xid_state();
  DBUG_ENTER("trans_xa_commit");

  DBUG_ASSERT(!thd->slave_thread || xid_state->get_xid()->is_null() ||
              m_xa_opt == XA_ONE_PHASE);

  if (!xid_state->has_same_xid(m_xid))
  {
    if (!xid_state->has_state(XID_STATE::XA_NOTR))
    {
      my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());

      DBUG_RETURN(true);
    }
    /*
      Note, that there is no race condition here between
      transaction_cache_search and transaction_cache_delete,
      since we always delete our own XID
      (m_xid == thd->transaction().xid_state().m_xid).
      The only case when m_xid != thd->transaction.xid_state.m_xid
      and xid_state->in_thd == 0 is in the function
      transaction_cache_insert_recovery(XID), which is called before starting
      client connections, and thus is always single-threaded.
    */
    Transaction_ctx *transaction= transaction_cache_search(m_xid);
    XID_STATE *xs= (transaction ? transaction->xid_state() : NULL);
    res= !xs || !xs->is_in_recovery();
    if (res)  // todo: fix transaction cleanup, BUG#20451386
      my_error(ER_XAER_NOTA, MYF(0));
    else
    {
      DBUG_ASSERT(xs->is_in_recovery());
      /*
        Resumed transaction XA-commit.
        The case deals with the "external" XA-commit by either a slave applier
        or a different than XA-prepared transaction session.
      */
      res= xs->xa_trans_rolled_back();

      /*
        xs' is_binlogged() is passed through xid_state's member to low-level
        logging routines for deciding how to log.  The same applies to
        Rollback case.
      */
      if (xs->is_binlogged())
        xid_state->set_binlogged();
      else
        xid_state->unset_binlogged();
      // todo xa framework: return an error
      ha_commit_or_rollback_by_xid(thd, m_xid, !res);
      xid_state->unset_binlogged();

      transaction_cache_delete(transaction);
    }
    DBUG_RETURN(res);
  }

  if (xid_state->xa_trans_rolled_back())
  {
    xa_trans_force_rollback(thd);
    res= thd->is_error();
  }
  else if (xid_state->has_state(XID_STATE::XA_IDLE) &&
           m_xa_opt == XA_ONE_PHASE)
  {
    int r= ha_commit_trans(thd, true);
    if ((res= MY_TEST(r)))
      my_error(r == 1 ? ER_XA_RBROLLBACK : ER_XAER_RMERR, MYF(0));
  }
  else if (xid_state->has_state(XID_STATE::XA_PREPARED) &&
           m_xa_opt == XA_NONE)
  {
    MDL_request mdl_request;

    /*
      Acquire metadata lock which will ensure that COMMIT is blocked
      by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
      progress blocks FTWRL).

      We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
    */
    MDL_REQUEST_INIT(&mdl_request,
                     MDL_key::COMMIT, "", "", MDL_INTENTION_EXCLUSIVE,
                     MDL_TRANSACTION);

    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout))
    {
      ha_rollback_trans(thd, true);
      my_error(ER_XAER_RMERR, MYF(0));
    }
    else
    {
      DEBUG_SYNC(thd, "trans_xa_commit_after_acquire_commit_lock");

      if (tc_log)
        res= MY_TEST(tc_log->commit(thd, /* all */ true));
      else
        res= MY_TEST(ha_commit_low(thd, /* all */ true));

      if (res)
        my_error(ER_XAER_RMERR, MYF(0));
#ifdef HAVE_PSI_TRANSACTION_INTERFACE
      else
      {
        /*
          Since we don't call ha_commit_trans() for prepared transactions,
          we need to explicitly mark the transaction as committed.
        */
        MYSQL_COMMIT_TRANSACTION(thd->m_transaction_psi);
        thd->m_transaction_psi= NULL;
      }
#endif
    }
  }
  else
  {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    DBUG_RETURN(true);
  }

  thd->variables.option_bits&= ~OPTION_BEGIN;
  thd->get_transaction()->reset_unsafe_rollback_flags(
    Transaction_ctx::SESSION);
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  transaction_cache_delete(thd->get_transaction());
  xid_state->set_state(XID_STATE::XA_NOTR);
  xid_state->unset_binlogged();
  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == NULL || res);
  DBUG_RETURN(res);
}


bool Sql_cmd_xa_commit::execute(THD *thd)
{
  bool st= trans_xa_commit(thd);

  if (!st)
  {
    thd->mdl_context.release_transactional_locks();
    /*
        We've just done a commit, reset transaction
        isolation level and access mode to the session default.
     */
    thd->tx_isolation= (enum_tx_isolation) thd->variables.tx_isolation;
    thd->tx_read_only= thd->variables.tx_read_only;

    my_ok(thd);
  }
  return st;
}


/**
  Roll back and terminate a XA transaction.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_rollback::trans_xa_rollback(THD *thd)
{
  XID_STATE *xid_state= thd->get_transaction()->xid_state();
  DBUG_ENTER("trans_xa_rollback");

  if (!xid_state->has_same_xid(m_xid))
  {
    if (!xid_state->has_state(XID_STATE::XA_NOTR))
    {
      my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());

      DBUG_RETURN(true);
    }

    Transaction_ctx *transaction= transaction_cache_search(m_xid);
    XID_STATE *xs= (transaction ? transaction->xid_state() : NULL);
    if (!xs || !xs->is_in_recovery())
      my_error(ER_XAER_NOTA, MYF(0));
    else
    {
      DBUG_ASSERT(xs->is_in_recovery());

      xs->xa_trans_rolled_back();
      if (xs->is_binlogged())
        xid_state->set_binlogged();
      else
        xid_state->unset_binlogged();
      ha_commit_or_rollback_by_xid(thd, m_xid, false);
      xid_state->unset_binlogged();
      transaction_cache_delete(transaction);
    }
    DBUG_RETURN(thd->is_error());
  }

  if (xid_state->has_state(XID_STATE::XA_NOTR) ||
      xid_state->has_state(XID_STATE::XA_ACTIVE))
  {
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
    DBUG_RETURN(true);
  }

  bool res= xa_trans_force_rollback(thd);

  thd->variables.option_bits&= ~OPTION_BEGIN;
  thd->get_transaction()->reset_unsafe_rollback_flags(
    Transaction_ctx::SESSION);
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  transaction_cache_delete(thd->get_transaction());
  xid_state->set_state(XID_STATE::XA_NOTR);
  xid_state->unset_binlogged();
  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == NULL);
  DBUG_RETURN(res);
}


bool Sql_cmd_xa_rollback::execute(THD *thd)
{
  bool st= trans_xa_rollback(thd);

  if (!st)
  {
    thd->mdl_context.release_transactional_locks();
    /*
      We've just done a rollback, reset transaction
      isolation level and access mode to the session default.
    */
    thd->tx_isolation= (enum_tx_isolation) thd->variables.tx_isolation;
    thd->tx_read_only= thd->variables.tx_read_only;
    my_ok(thd);
  }

  DBUG_EXECUTE_IF("crash_after_xa_rollback", DBUG_SUICIDE(););

  return st;
}


/**
  Start a XA transaction with the given xid value.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_start::trans_xa_start(THD *thd)
{
  XID_STATE *xid_state= thd->get_transaction()->xid_state();
  DBUG_ENTER("trans_xa_start");

  if (xid_state->has_state(XID_STATE::XA_IDLE) &&
      m_xa_opt == XA_RESUME)
  {
    bool not_equal= !xid_state->has_same_xid(m_xid);
    if (not_equal)
      my_error(ER_XAER_NOTA, MYF(0));
    else
    {
      xid_state->set_state(XID_STATE::XA_ACTIVE);
      MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi,
                                  (int)thd->get_transaction()->xid_state()->get_state());
    }
    DBUG_RETURN(not_equal);
  }

  /* TODO: JOIN is not supported yet. */
  if (m_xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!xid_state->has_state(XID_STATE::XA_NOTR))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (thd->locked_tables_mode || thd->in_active_multi_stmt_transaction())
    my_error(ER_XAER_OUTSIDE, MYF(0));
  else if (!trans_begin(thd))
  {
    xid_state->start_normal_xa(m_xid);
    MYSQL_SET_TRANSACTION_XID(thd->m_transaction_psi,
                              (const void *)xid_state->get_xid(),
                              (int)xid_state->get_state());
    if (transaction_cache_insert(m_xid, thd->get_transaction()))
    {
      xid_state->reset();
      trans_rollback(thd);
    }
  }

  DBUG_RETURN(thd->is_error() ||
              !xid_state->has_state(XID_STATE::XA_ACTIVE));
}


bool Sql_cmd_xa_start::execute(THD *thd)
{
  bool st= trans_xa_start(thd);

  if (!st)
  {
    if (thd->variables.pseudo_slave_mode)
    {
      /*
        In case of slave thread applier or processing binlog by client,
        detach the "native" thd's trx in favor of dynamically created.
      */
      plugin_foreach(thd, detach_native_trx,
                     MYSQL_STORAGE_ENGINE_PLUGIN, NULL);
    }
    my_ok(thd);
  }

  return st;
}


/**
  Put a XA transaction in the IDLE state.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_end::trans_xa_end(THD *thd)
{
  XID_STATE *xid_state= thd->get_transaction()->xid_state();
  DBUG_ENTER("trans_xa_end");

  /* TODO: SUSPEND and FOR MIGRATE are not supported yet. */
  if (m_xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!xid_state->has_state(XID_STATE::XA_ACTIVE))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (!xid_state->has_same_xid(m_xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else if (!xid_state->xa_trans_rolled_back())
  {
    xid_state->set_state(XID_STATE::XA_IDLE);
    MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi,
                                   (int)xid_state->get_state());
  }
  else
  {
    MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi,
                                   (int)xid_state->get_state());
  }

  DBUG_RETURN(thd->is_error() ||
              !xid_state->has_state(XID_STATE::XA_IDLE));
}


bool Sql_cmd_xa_end::execute(THD *thd)
{
  bool st= trans_xa_end(thd);

  if (!st)
    my_ok(thd);

  return st;
}


/**
  Put a XA transaction in the PREPARED state.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure
*/

bool Sql_cmd_xa_prepare::trans_xa_prepare(THD *thd)
{
  XID_STATE *xid_state= thd->get_transaction()->xid_state();
  DBUG_ENTER("trans_xa_prepare");

  if (!xid_state->has_state(XID_STATE::XA_IDLE))
    my_error(ER_XAER_RMFAIL, MYF(0), xid_state->state_name());
  else if (!xid_state->has_same_xid(m_xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else if (ha_prepare(thd))
  {
    /*
      todo: simulate a failure that sustains
      Bug 20538956.
    */
    transaction_cache_delete(thd->get_transaction());
    xid_state->set_state(XID_STATE::XA_NOTR);
    MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi,
                                   (int)xid_state->get_state());
    my_error(ER_XA_RBROLLBACK, MYF(0));
  }
  else
  {
    xid_state->set_state(XID_STATE::XA_PREPARED);
    MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi,
                                   (int)xid_state->get_state());
    if (thd->rpl_thd_ctx.session_gtids_ctx().notify_after_xa_prepare(thd))
      sql_print_warning("Failed to collect GTID to send in the response packet!");
  }

  DBUG_RETURN(thd->is_error() ||
              !xid_state->has_state(XID_STATE::XA_PREPARED));
}


bool Sql_cmd_xa_prepare::execute(THD *thd)
{
  bool st= trans_xa_prepare(thd);

  if (!st)
  {
    if (!thd->variables.pseudo_slave_mode ||
        !(st= applier_reset_xa_trans(thd)))
      my_ok(thd);
  }

  return st;
}


/**
  Return the list of XID's to a client, the same way SHOW commands do.

  @param thd    Current thread

  @retval false  Success
  @retval true   Failure

  @note
    I didn't find in XA specs that an RM cannot return the same XID twice,
    so trans_xa_recover does not filter XID's to ensure uniqueness.
    It can be easily fixed later, if necessary.
*/

bool Sql_cmd_xa_recover::trans_xa_recover(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->get_protocol();
  int i= 0;
  Transaction_ctx *transaction;

  DBUG_ENTER("trans_xa_recover");

  field_list.push_back(new Item_int(NAME_STRING("formatID"), 0,
                                    MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_int(NAME_STRING("gtrid_length"), 0,
                                    MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_int(NAME_STRING("bqual_length"), 0,
                                    MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_empty_string("data", XIDDATASIZE*2+2));

  if (thd->send_result_metadata(&field_list,
                                Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(true);

  mysql_mutex_lock(&LOCK_transaction_cache);

  while ((transaction= (Transaction_ctx*) my_hash_element(&transaction_cache,
                                                         i++)))
  {
    XID_STATE *xs= transaction->xid_state();
    if (xs->has_state(XID_STATE::XA_PREPARED))
    {
      protocol->start_row();
      xs->store_xid_info(protocol, m_print_xid_as_hex);

      if (protocol->end_row())
      {
        mysql_mutex_unlock(&LOCK_transaction_cache);
        DBUG_RETURN(true);
      }
    }
  }

  mysql_mutex_unlock(&LOCK_transaction_cache);
  my_eof(thd);
  DBUG_RETURN(false);
}


bool Sql_cmd_xa_recover::execute(THD *thd)
{
  bool st= trans_xa_recover(thd);
  DBUG_EXECUTE_IF("crash_after_xa_recover", {DBUG_SUICIDE();});

  return st;
}


bool XID_STATE::xa_trans_rolled_back()
{
  if (rm_error)
  {
    switch (rm_error)
    {
    case ER_LOCK_WAIT_TIMEOUT:
      my_error(ER_XA_RBTIMEOUT, MYF(0));
      break;
    case ER_LOCK_DEADLOCK:
      my_error(ER_XA_RBDEADLOCK, MYF(0));
      break;
    default:
      my_error(ER_XA_RBROLLBACK, MYF(0));
    }
    xa_state= XID_STATE::XA_ROLLBACK_ONLY;
  }

  return (xa_state == XID_STATE::XA_ROLLBACK_ONLY);
}


bool XID_STATE::check_xa_idle_or_prepared(bool report_error) const
{
  if (xa_state == XA_IDLE ||
      xa_state == XA_PREPARED)
  {
    if (report_error)
      my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);

    return true;
  }

  return false;
}


bool XID_STATE::check_has_uncommitted_xa() const
{
  if (xa_state == XA_IDLE ||
      xa_state == XA_PREPARED ||
      xa_state == XA_ROLLBACK_ONLY)
  {
    my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
    return true;
  }

  return false;
}


bool XID_STATE::check_in_xa(bool report_error) const
{
  if (xa_state != XA_NOTR)
  {
    if (report_error)
      my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
    return true;
  }

  return false;
}


void XID_STATE::set_error(THD *thd)
{
  if (xa_state != XA_NOTR)
    rm_error= thd->get_stmt_da()->mysql_errno();
}


void XID_STATE::store_xid_info(Protocol *protocol, bool print_xid_as_hex) const
{
  protocol->store_longlong(static_cast<longlong>(m_xid.formatID), false);
  protocol->store_longlong(static_cast<longlong>(m_xid.gtrid_length), false);
  protocol->store_longlong(static_cast<longlong>(m_xid.bqual_length), false);

  if (print_xid_as_hex)
  {
    /*
      xid_buf contains enough space for 0x followed by HEX representation
      of the binary XID data and one null termination character.
    */
    char xid_buf[XIDDATASIZE * 2 + 2 + 1];

    xid_buf[0]= '0';
    xid_buf[1]= 'x';

    size_t xid_str_len= bin_to_hex_str(xid_buf + 2, sizeof(xid_buf) - 2,
                                       const_cast<char*>(m_xid.data),
                                       m_xid.gtrid_length +
                                       m_xid.bqual_length) + 2;
    protocol->store(xid_buf, xid_str_len, &my_charset_bin);
  }
  else
  {
    protocol->store(m_xid.data, m_xid.gtrid_length + m_xid.bqual_length,
                    &my_charset_bin);
  }


}


#ifndef DBUG_OFF
char* XID::xid_to_str(char *buf) const
{
  char *s= buf;
  *s++= '\'';

  for (int i= 0; i < gtrid_length + bqual_length; i++)
  {
    /* is_next_dig is set if next character is a number */
    bool is_next_dig= false;
    if (i < XIDDATASIZE)
    {
      char ch= data[i+1];
      is_next_dig= (ch >= '0' && ch <='9');
    }
    if (i == gtrid_length)
    {
      *s++= '\'';
      if (bqual_length)
      {
        *s++= '.';
        *s++= '\'';
      }
    }
    uchar c= static_cast<uchar>(data[i]);
    if (c < 32 || c > 126)
    {
      *s++= '\\';
      /*
        If next character is a number, write current character with
        3 octal numbers to ensure that the next number is not seen
        as part of the octal number
      */
      if (c > 077 || is_next_dig)
        *s++= _dig_vec_lower[c >> 6];
      if (c > 007 || is_next_dig)
        *s++=_dig_vec_lower[(c >> 3) & 7];
      *s++= _dig_vec_lower[c & 7];
    }
    else
    {
      if (c == '\'' || c == '\\')
        *s++= '\\';
      *s++= c;
    }
  }
  *s++= '\'';
  *s= 0;
  return buf;
}
#endif


extern "C" uchar *transaction_get_hash_key(const uchar *, size_t *, my_bool);
extern "C" void transaction_free_hash(void *);


/**
  Callback that is called to get the key for a hash.

  @param ptr  pointer to the record
  @param length  length of the record

  @return  pointer to a record stored in cache
*/

extern "C" uchar *transaction_get_hash_key(const uchar *ptr, size_t *length,
                                           my_bool not_used __attribute__((unused)))
{
  *length= ((Transaction_ctx*)ptr)->xid_state()->get_xid()->key_length();
  return ((Transaction_ctx*)ptr)->xid_state()->get_xid()->key();
}


/**
  Callback that is called to do cleanup.

  @param ptr  pointer to free
*/

void transaction_free_hash(void *ptr)
{
  Transaction_ctx *transaction= (Transaction_ctx*)ptr;
  // Only time it's allocated is during recovery process.
  if (transaction->xid_state()->is_in_recovery())
    delete transaction;
}


#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_transaction_cache;

static PSI_mutex_info transaction_cache_mutexes[]=
{
  { &key_LOCK_transaction_cache, "LOCK_transaction_cache", PSI_FLAG_GLOBAL}
};

static void init_transaction_cache_psi_keys(void)
{
  const char* category= "sql";
  int count;

  count= array_elements(transaction_cache_mutexes);
  mysql_mutex_register(category, transaction_cache_mutexes, count);
}
#endif /* HAVE_PSI_INTERFACE */


bool transaction_cache_init()
{
#ifdef HAVE_PSI_INTERFACE
  init_transaction_cache_psi_keys();
#endif

  mysql_mutex_init(key_LOCK_transaction_cache, &LOCK_transaction_cache,
                   MY_MUTEX_INIT_FAST);
  return my_hash_init(&transaction_cache, &my_charset_bin, 100, 0, 0,
                      transaction_get_hash_key, transaction_free_hash, 0) != 0;
}

void transaction_cache_free()
{
  if (my_hash_inited(&transaction_cache))
  {
    my_hash_free(&transaction_cache);
    mysql_mutex_destroy(&LOCK_transaction_cache);
  }
}


Transaction_ctx *transaction_cache_search(XID *xid)
{
  mysql_mutex_lock(&LOCK_transaction_cache);

  Transaction_ctx *res=
      (Transaction_ctx *)my_hash_search(&transaction_cache,
                                        xid->key(),
                                        xid->key_length());
  mysql_mutex_unlock(&LOCK_transaction_cache);
  return res;
}


bool transaction_cache_insert(XID *xid, Transaction_ctx *transaction)
{
  mysql_mutex_lock(&LOCK_transaction_cache);
  if (my_hash_search(&transaction_cache, xid->key(),
                     xid->key_length()))
  {
    mysql_mutex_unlock(&LOCK_transaction_cache);
    my_error(ER_XAER_DUPID, MYF(0));
    return true;
  }
  bool res= my_hash_insert(&transaction_cache, (uchar*)transaction);
  mysql_mutex_unlock(&LOCK_transaction_cache);
  return res;
}


inline bool create_and_insert_new_transaction(XID *xid, bool is_binlogged_arg)
{
  Transaction_ctx *transaction= new (std::nothrow) Transaction_ctx();
  XID_STATE *xs;

  if (!transaction)
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), sizeof(Transaction_ctx));
    return true;
  }
  xs= transaction->xid_state();
  xs->start_recovery_xa(xid, is_binlogged_arg);

  return my_hash_insert(&transaction_cache, (uchar*)transaction);
}


bool transaction_cache_detach(Transaction_ctx *transaction)
{
  bool res= false;
  XID_STATE *xs= transaction->xid_state();
  XID xid= *(xs->get_xid());
  bool was_logged= xs->is_binlogged();


  DBUG_ASSERT(xs->has_state(XID_STATE::XA_PREPARED));

  mysql_mutex_lock(&LOCK_transaction_cache);

  DBUG_ASSERT(my_hash_search(&transaction_cache, xid.key(),
                             xid.key_length()));

  my_hash_delete(&transaction_cache, (uchar *)transaction);
  res= create_and_insert_new_transaction(&xid, was_logged);

  mysql_mutex_unlock(&LOCK_transaction_cache);

  return res;
}


bool transaction_cache_insert_recovery(XID *xid)
{
  mysql_mutex_lock(&LOCK_transaction_cache);

  if (my_hash_search(&transaction_cache, xid->key(),
                     xid->key_length()))
  {
    mysql_mutex_unlock(&LOCK_transaction_cache);
    return false;
  }

  /*
    It's assumed that XA transaction was binlogged before the server
    shutdown. If --log-bin has changed since that from OFF to ON, XA
    COMMIT or XA ROLLBACK of this transaction may be logged alone into
    the binary log.
  */
  bool res= create_and_insert_new_transaction(xid, true);

  mysql_mutex_unlock(&LOCK_transaction_cache);

  return res;
}


void transaction_cache_delete(Transaction_ctx *transaction)
{
  mysql_mutex_lock(&LOCK_transaction_cache);
  my_hash_delete(&transaction_cache, (uchar *)transaction);
  mysql_mutex_unlock(&LOCK_transaction_cache);
}


/**
  This is a specific to "slave" applier collection of standard cleanup
  actions to reset XA transaction states at the end of XA prepare rather than
  to do it at the transaction commit, see @c ha_commit_one_phase.
  THD of the slave applier is dissociated from a transaction object in engine
  that continues to exist there.

  @param  THD current thread
  @return the value of is_error()
*/

bool applier_reset_xa_trans(THD *thd)
{
  Transaction_ctx *trn_ctx= thd->get_transaction();
  XID_STATE *xid_state= trn_ctx->xid_state();
  /*
    In the following the server transaction state gets reset for
    a slave applier thread similarly to xa_commit logics
    except commit does not run.
  */
  thd->variables.option_bits&= ~OPTION_BEGIN;
  trn_ctx->reset_unsafe_rollback_flags(Transaction_ctx::STMT);
  thd->server_status&= ~SERVER_STATUS_IN_TRANS;
  /* Server transaction ctx is detached from THD */
  transaction_cache_detach(trn_ctx);
  xid_state->reset();
  /*
     The current engine transactions is detached from THD, and
     previously saved is restored.
  */
  attach_native_trx(thd);
  trn_ctx->set_ha_trx_info(Transaction_ctx::SESSION, NULL);
  trn_ctx->set_no_2pc(Transaction_ctx::SESSION, false);
  trn_ctx->cleanup();
#ifdef HAVE_PSI_TRANSACTION_INTERFACE
  MYSQL_COMMIT_TRANSACTION(thd->m_transaction_psi);
  thd->m_transaction_psi= NULL;
#endif
  thd->mdl_context.release_transactional_locks();

  return thd->is_error();
}


/**
  The function detaches existing storage engines transaction
  context from thd. Backup area to save it is provided to low level
  storage engine function.

  is invoked by plugin_foreach() after
  trans_xa_start() for each storage engine.

  @param[in,out]     thd     Thread context
  @param             plugin  Reference to handlerton

  @return    FALSE   on success, TRUE otherwise.
*/

my_bool detach_native_trx(THD *thd, plugin_ref plugin, void *unused)
{
  handlerton *hton= plugin_data<handlerton *>(plugin);

  if (hton->replace_native_transaction_in_thd)
    hton->replace_native_transaction_in_thd(thd, NULL,
                                            thd_ha_data_backup(thd, hton));

  return FALSE;
}

/**
  The function restores previously saved storage engine transaction context.

  @param     thd     Thread context
*/
void attach_native_trx(THD *thd)
{
  Ha_trx_info *ha_info=
    thd->get_transaction()->ha_trx_info(Transaction_ctx::SESSION);
  Ha_trx_info *ha_info_next;

  if (ha_info)
  {
    for (; ha_info; ha_info= ha_info_next)
    {
      handlerton *hton= ha_info->ht();
      if (hton->replace_native_transaction_in_thd)
      {
        /* restore the saved original engine transaction's link with thd */
        void **trx_backup= thd_ha_data_backup(thd, hton);

        hton->
          replace_native_transaction_in_thd(thd, *trx_backup, NULL);
        *trx_backup= NULL;
      }
      ha_info_next= ha_info->next();
      ha_info->reset();
    }
  }
}
