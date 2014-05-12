// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "MDLog.h"
#include "MDS.h"
#include "MDCache.h"
#include "LogEvent.h"

#include "osdc/Journaler.h"
#include "mds/JournalPointer.h"

#include "common/entity_name.h"
#include "common/perf_counters.h"

#include "events/ESubtreeMap.h"

#include "common/config.h"
#include "common/errno.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_mds
#undef DOUT_COND
#define DOUT_COND(cct, l) l<=cct->_conf->debug_mds || l <= cct->_conf->debug_mds_log
#undef dout_prefix
#define dout_prefix *_dout << "mds." << mds->get_nodeid() << ".log "

// cons/des
MDLog::~MDLog()
{
  if (journaler) { delete journaler; journaler = 0; }
  if (logger) {
    g_ceph_context->get_perfcounters_collection()->remove(logger);
    delete logger;
    logger = 0;
  }
}


void MDLog::create_logger()
{
  PerfCountersBuilder plb(g_ceph_context, "mds_log", l_mdl_first, l_mdl_last);

  plb.add_u64_counter(l_mdl_evadd, "evadd");
  plb.add_u64_counter(l_mdl_evex, "evex");
  plb.add_u64_counter(l_mdl_evtrm, "evtrm");
  plb.add_u64(l_mdl_ev, "ev");
  plb.add_u64(l_mdl_evexg, "evexg");
  plb.add_u64(l_mdl_evexd, "evexd");

  plb.add_u64_counter(l_mdl_segadd, "segadd");
  plb.add_u64_counter(l_mdl_segex, "segex");
  plb.add_u64_counter(l_mdl_segtrm, "segtrm");
  plb.add_u64(l_mdl_seg, "seg");
  plb.add_u64(l_mdl_segexg, "segexg");
  plb.add_u64(l_mdl_segexd, "segexd");

  plb.add_u64(l_mdl_expos, "expos");
  plb.add_u64(l_mdl_wrpos, "wrpos");
  plb.add_u64(l_mdl_rdpos, "rdpos");
  plb.add_u64(l_mdl_jlat, "jlat");

  // logger
  logger = plb.create_perf_counters();
  g_ceph_context->get_perfcounters_collection()->add(logger);
}

void MDLog::init_journaler()
{
  // inode
  ino = MDS_INO_LOG_OFFSET + mds->get_nodeid();
  
  // log streamer
  if (journaler) delete journaler;
  journaler = new Journaler(ino, mds->mdsmap->get_metadata_pool(), CEPH_FS_ONDISK_MAGIC, mds->objecter,
			    logger, l_mdl_jlat,
			    &mds->timer);
  assert(journaler->is_readonly());
  journaler->set_write_error_handler(new C_MDL_WriteError(this));
}

void MDLog::handle_journaler_write_error(int r)
{
  if (r == -EBLACKLISTED) {
    derr << "we have been blacklisted (fenced), respawning..." << dendl;
    mds->respawn();
  } else {
    derr << "unhandled error " << cpp_strerror(r) << ", shutting down..." << dendl;
    mds->suicide();
  }
}

void MDLog::write_head(Context *c) 
{
  journaler->write_head(c);
}

uint64_t MDLog::get_read_pos()
{
  return journaler->get_read_pos(); 
}

uint64_t MDLog::get_write_pos()
{
  return journaler->get_write_pos(); 
}

uint64_t MDLog::get_safe_pos()
{
  return journaler->get_write_safe_pos(); 
}



void MDLog::create(Context *c)
{
  dout(5) << "create empty log" << dendl;
  init_journaler();
  journaler->set_writeable();
  journaler->create(&mds->mdcache->default_log_layout, g_conf->mds_journal_format);
  journaler->write_head(c);

  logger->set(l_mdl_expos, journaler->get_expire_pos());
  logger->set(l_mdl_wrpos, journaler->get_write_pos());
}

void MDLog::open(Context *c)
{
  dout(5) << "open discovering log bounds" << dendl;

  recovery_thread.set_completion(c);
  recovery_thread.create();
  recovery_thread.detach();
  // either append() or replay() will follow.
}

void MDLog::append()
{
  dout(5) << "append positioning at end and marking writeable" << dendl;
  journaler->set_read_pos(journaler->get_write_pos());
  journaler->set_expire_pos(journaler->get_write_pos());
  
  journaler->set_writeable();

  logger->set(l_mdl_expos, journaler->get_write_pos());
}



// -------------------------------------------------

void MDLog::start_entry(LogEvent *e)
{
  assert(cur_event == NULL);
  cur_event = e;
  e->set_start_off(get_write_pos());
}

void MDLog::submit_entry(LogEvent *le, Context *c) 
{
  assert(!mds->is_any_replay());
  assert(le == cur_event);
  cur_event = NULL;

  if (!g_conf->mds_log) {
    // hack: log is disabled.
    if (c) {
      c->complete(0);
    }
    return;
  }

  // let the event register itself in the segment
  assert(!segments.empty());
  le->_segment = segments.rbegin()->second;
  le->_segment->num_events++;
  le->update_segment();

  le->set_stamp(ceph_clock_now(g_ceph_context));
  
  num_events++;
  assert(!capped);
  
  // encode it, with event type
  {
    bufferlist bl;
    le->encode_with_header(bl);

    dout(5) << "submit_entry " << journaler->get_write_pos() << "~" << bl.length()
	    << " : " << *le << dendl;
      
    // journal it.
    journaler->append_entry(bl);  // bl is destroyed.
  }

  le->_segment->end = journaler->get_write_pos();

  if (logger) {
    logger->inc(l_mdl_evadd);
    logger->set(l_mdl_ev, num_events);
    logger->set(l_mdl_wrpos, journaler->get_write_pos());
  }

  unflushed++;

  if (c)
    journaler->wait_for_flush(c);
  
  // start a new segment?
  //  FIXME: should this go elsewhere?
  uint64_t last_seg = get_last_segment_offset();
  uint64_t period = journaler->get_layout_period();
  // start a new segment if there are none or if we reach end of last segment
  if (le->get_type() == EVENT_SUBTREEMAP ||
      (le->get_type() == EVENT_IMPORTFINISH && mds->is_resolve())) {
    // avoid infinite loop when ESubtreeMap is very large.
    // don not insert ESubtreeMap among EImportFinish events that finish
    // disambiguate imports. Because the ESubtreeMap reflects the subtree
    // state when all EImportFinish events are replayed.
  } else if (journaler->get_write_pos()/period != last_seg/period) {
    dout(10) << "submit_entry also starting new segment: last = " << last_seg
	     << ", cur pos = " << journaler->get_write_pos() << dendl;
    start_new_segment();
  } else if (g_conf->mds_debug_subtrees &&
	     le->get_type() != EVENT_SUBTREEMAP_TEST) {
    // debug: journal this every time to catch subtree replay bugs.
    // use a different event id so it doesn't get interpreted as a
    // LogSegment boundary on replay.
    LogEvent *sle = mds->mdcache->create_subtree_map();
    sle->set_type(EVENT_SUBTREEMAP_TEST);
    submit_entry(sle);
  }

  delete le;
}

void MDLog::wait_for_safe(Context *c)
{
  if (g_conf->mds_log) {
    // wait
    journaler->wait_for_flush(c);
  } else {
    // hack: bypass.
    c->complete(0);
  }
}

void MDLog::flush()
{
  if (unflushed)
    journaler->flush();
  unflushed = 0;
}

void MDLog::cap()
{ 
  dout(5) << "cap" << dendl;
  capped = true;
}


// -----------------------------
// segments

void MDLog::start_new_segment(Context *onsync)
{
  prepare_new_segment();
  journal_segment_subtree_map();
  if (onsync) {
    wait_for_safe(onsync);
    flush();
  }
}

void MDLog::prepare_new_segment()
{
  dout(7) << __func__ << " at " << journaler->get_write_pos() << dendl;

  segments[journaler->get_write_pos()] = new LogSegment(journaler->get_write_pos());

  logger->inc(l_mdl_segadd);
  logger->set(l_mdl_seg, segments.size());

  // Adjust to next stray dir
  dout(10) << "Advancing to next stray directory on mds " << mds->get_nodeid() 
	   << dendl;
  mds->mdcache->advance_stray();
}

void MDLog::journal_segment_subtree_map()
{
  dout(7) << __func__ << dendl;
  submit_entry(mds->mdcache->create_subtree_map());
}

void MDLog::trim(int m)
{
  int max_segments = g_conf->mds_log_max_segments;
  int max_events = g_conf->mds_log_max_events;
  if (m >= 0)
    max_events = m;

  // trim!
  dout(10) << "trim " 
	   << segments.size() << " / " << max_segments << " segments, " 
	   << num_events << " / " << max_events << " events"
	   << ", " << expiring_segments.size() << " (" << expiring_events << ") expiring"
	   << ", " << expired_segments.size() << " (" << expired_events << ") expired"
	   << dendl;

  if (segments.empty())
    return;

  // hack: only trim for a few seconds at a time
  utime_t stop = ceph_clock_now(g_ceph_context);
  stop += 2.0;

  map<uint64_t,LogSegment*>::iterator p = segments.begin();
  while (p != segments.end() && 
	 ((max_events >= 0 &&
	   num_events - expiring_events - expired_events > max_events) ||
	  (max_segments >= 0 &&
	   segments.size() - expiring_segments.size() - expired_segments.size() > (unsigned)max_segments))) {
    
    if (stop < ceph_clock_now(g_ceph_context))
      break;

    int num_expiring_segments = (int)expiring_segments.size();
    if (num_expiring_segments >= g_conf->mds_log_max_expiring)
      break;

    int op_prio = CEPH_MSG_PRIO_LOW +
		  (CEPH_MSG_PRIO_HIGH - CEPH_MSG_PRIO_LOW) *
		  num_expiring_segments / g_conf->mds_log_max_expiring;

    // look at first segment
    LogSegment *ls = p->second;
    assert(ls);
    ++p;
    
    if (ls->end > journaler->get_write_safe_pos()) {
      dout(5) << "trim segment " << ls->offset << ", not fully flushed yet, safe "
	      << journaler->get_write_safe_pos() << " < end " << ls->end << dendl;
      break;
    }
    if (expiring_segments.count(ls)) {
      dout(5) << "trim already expiring segment " << ls->offset << ", " << ls->num_events << " events" << dendl;
    } else if (expired_segments.count(ls)) {
      dout(5) << "trim already expired segment " << ls->offset << ", " << ls->num_events << " events" << dendl;
    } else {
      try_expire(ls, op_prio);
    }
  }

  // discard expired segments
  _trim_expired_segments();
}


void MDLog::try_expire(LogSegment *ls, int op_prio)
{
  C_GatherBuilder gather_bld(g_ceph_context);
  ls->try_to_expire(mds, gather_bld, op_prio);
  if (gather_bld.has_subs()) {
    assert(expiring_segments.count(ls) == 0);
    expiring_segments.insert(ls);
    expiring_events += ls->num_events;
    dout(5) << "try_expire expiring segment " << ls->offset << dendl;
    gather_bld.set_finisher(new C_MaybeExpiredSegment(this, ls, op_prio));
    gather_bld.activate();
  } else {
    dout(10) << "try_expire expired segment " << ls->offset << dendl;
    _expired(ls);
  }
  
  logger->set(l_mdl_segexg, expiring_segments.size());
  logger->set(l_mdl_evexg, expiring_events);
}

void MDLog::_maybe_expired(LogSegment *ls, int op_prio)
{
  dout(10) << "_maybe_expired segment " << ls->offset << " " << ls->num_events << " events" << dendl;
  assert(expiring_segments.count(ls));
  expiring_segments.erase(ls);
  expiring_events -= ls->num_events;
  try_expire(ls, op_prio);
}

void MDLog::_trim_expired_segments()
{
  // trim expired segments?
  bool trimmed = false;
  while (!segments.empty()) {
    LogSegment *ls = segments.begin()->second;
    if (!expired_segments.count(ls)) {
      dout(10) << "_trim_expired_segments waiting for " << ls->offset << " to expire" << dendl;
      break;
    }
    
    dout(10) << "_trim_expired_segments trimming expired " << ls->offset << dendl;
    expired_events -= ls->num_events;
    expired_segments.erase(ls);
    num_events -= ls->num_events;
      
    // this was the oldest segment, adjust expire pos
    if (journaler->get_expire_pos() < ls->offset)
      journaler->set_expire_pos(ls->offset);
    
    logger->set(l_mdl_expos, ls->offset);
    logger->inc(l_mdl_segtrm);
    logger->inc(l_mdl_evtrm, ls->num_events);
    
    segments.erase(ls->offset);
    delete ls;
    trimmed = true;
  }
  
  if (trimmed)
    journaler->write_head(0);
}

void MDLog::_expired(LogSegment *ls)
{
  dout(5) << "_expired segment " << ls->offset << " " << ls->num_events << " events" << dendl;

  if (!capped && ls == peek_current_segment()) {
    dout(5) << "_expired not expiring " << ls->offset << ", last one and !capped" << dendl;
  } else {
    // expired.
    expired_segments.insert(ls);
    expired_events += ls->num_events;
    
    logger->inc(l_mdl_evex, ls->num_events);
    logger->inc(l_mdl_segex);
  }

  logger->set(l_mdl_ev, num_events);
  logger->set(l_mdl_evexd, expired_events);
  logger->set(l_mdl_seg, segments.size());
  logger->set(l_mdl_segexd, expired_segments.size());
}



void MDLog::replay(Context *c)
{
  assert(journaler->is_active());
  assert(journaler->is_readonly());

  // empty?
  if (journaler->get_read_pos() == journaler->get_write_pos()) {
    dout(10) << "replay - journal empty, done." << dendl;
    if (c) {
      c->complete(0);
    }
    return;
  }

  // add waiter
  if (c)
    waitfor_replay.push_back(c);

  // go!
  dout(10) << "replay start, from " << journaler->get_read_pos()
	   << " to " << journaler->get_write_pos() << dendl;

  assert(num_events == 0 || already_replayed);
  already_replayed = true;

  replay_thread.create();
  replay_thread.detach();
}

class C_MDL_Replay : public Context {
  MDLog *mdlog;
public:
  C_MDL_Replay(MDLog *l) : mdlog(l) {}
  void finish(int r) { 
    mdlog->replay_cond.Signal();
  }
};


/**
 * Resolve the JournalPointer object to a journal file, and
 * instantiate a Journaler object.  This may re-write the journal
 * if the journal in RADOS appears to be in an old format.
 *
 * This is a separate thread because of the way it is initialized from inside
 * the mds lock, which is also the global objecter lock -- rather than split
 * it up into hard-to-read async operations linked up by contexts, 
 *
 * When this function completes, the `journaler` attribute will be set to
 * a Journaler instance using the latest available serialization format.
 */
void MDLog::_recovery_thread(Context *completion)
{
  assert(journaler == NULL);

  // First, read the pointer object.
  // If the pointer object is not present, then create it with
  // front = default ino and back = null
  JournalPointer jp(mds->get_nodeid(), mds->mdsmap->get_metadata_pool());
  int const read_result = jp.load(mds->objecter, &(mds->mds_lock));
  if (read_result == -ENOENT) {
    inodeno_t const default_log_ino = MDS_INO_LOG_OFFSET + mds->get_nodeid();
    jp.front = default_log_ino;
    int write_result = jp.save(mds->objecter, &(mds->mds_lock));
    // Nothing graceful we can do for this
    assert(write_result >= 0);
  } else if (read_result != 0) {
    // No graceful way of handling this: give up and leave it for support
    // to work out why RADOS preventing access.
    assert(0);
  }

  // If the back pointer is non-null, that means that a journal
  // rewrite failed part way through.  Erase the back journal
  // to clean up.
  if (jp.back) {
    dout(1) << "Erasing journal " << jp.back << dendl;
    C_SaferCond erase_waiter;
    Journaler back(jp.back, mds->mdsmap->get_metadata_pool(), CEPH_FS_ONDISK_MAGIC,
        mds->objecter, logger, l_mdl_jlat, &mds->timer);

    // Read all about this journal (header + extents)
    mds->mds_lock.Lock();
    C_SaferCond recover_wait;
    back.recover(&recover_wait);
    mds->mds_lock.Unlock();
    int recovery_result = recover_wait.wait();

    // Journaler.recover succeeds if no journal objects are present: an error
    // means something worse like a corrupt header, which we can't handle here.
    assert(recovery_result == 0);
    // We could read journal, so we can erase it.
    mds->mds_lock.Lock();
    back.erase(&erase_waiter);
    mds->mds_lock.Unlock();
    int erase_result = erase_waiter.wait();

    // If we are successful, or find no data, we can update the JournalPointer to
    // reflect that the back journal is gone.
    if (erase_result != 0 && erase_result != -ENOENT) {
      derr << "Failed to erase journal " << jp.back << ": " << cpp_strerror(erase_result) << dendl;
    } else {
      dout(1) << "Successfully erased journal, updating journal pointer" << dendl;
      jp.back = 0;
      int write_result = jp.save(mds->objecter, &(mds->mds_lock));
      // Nothing graceful we can do for this
      assert(write_result >= 0);
    }
  }

  /* Read the header from the front journal */
  Journaler *front_journal = new Journaler(jp.front, mds->mdsmap->get_metadata_pool(),
      CEPH_FS_ONDISK_MAGIC, mds->objecter, logger, l_mdl_jlat, &mds->timer);
  C_SaferCond recover_wait;
  mds->mds_lock.Lock();
  front_journal->recover(&recover_wait);
  mds->mds_lock.Unlock();
  dout(4) << "Waiting for journal " << jp.front << " to recover..." << dendl;
  int recovery_result = recover_wait.wait();
  dout(4) << "Journal " << jp.front << " recovered." << dendl;

  if (recovery_result != 0) {
    derr << "Error recovering journal " << jp.front << ": " << cpp_strerror(recovery_result) << dendl;
    completion->complete(recovery_result);
    return;
  }

  /* Check whether the front journal format is acceptable or needs re-write */
  if (front_journal->get_stream_format() >= g_conf->mds_journal_format) {
    /* Great, the journal is of current format and ready to rock, hook
     * it into this->journaler and complete */
    journaler = front_journal;
    journaler->set_write_error_handler(new C_MDL_WriteError(this));
    mds->mds_lock.Lock();
    completion->complete(0);
    mds->mds_lock.Unlock();
  } else {
    /* Hand off to reformat routine, which will ultimately set the
     * completion when it has done its thing */
    dout(1) << "Journal " << jp.front << " has old format "
      << front_journal->get_stream_format() << ", it will now be updated" << dendl;

    _reformat_journal(jp, front_journal, completion);
  }
}

/**
 * Blocking rewrite of the journal to a new file, followed by
 * swap of journal pointer to point to the new one.
 *
 * We write the new journal to the 'back' journal from the JournalPointer,
 * swapping pointers to make that one the front journal only when we have
 * safely completed.
 */
void MDLog::_reformat_journal(JournalPointer const &jp_in, Journaler *old_journal, Context *completion)
{
  assert(!jp_in.is_null());
  assert(completion != NULL);
  assert(old_journal != NULL);

  JournalPointer jp = jp_in;

  /* Set JournalPointer.back to the location we will write the new journal */
  inodeno_t primary_ino = MDS_INO_LOG_OFFSET + mds->get_nodeid();
  inodeno_t secondary_ino = MDS_INO_LOG_BACKUP_OFFSET + mds->get_nodeid();
  jp.back = (jp.front == primary_ino ? secondary_ino : primary_ino);
  int write_result = jp.save(mds->objecter, &(mds->mds_lock));
  assert(write_result == 0);

  /* Create the new Journaler file */
  Journaler *new_journal = new Journaler(jp.back, mds->mdsmap->get_metadata_pool(),
      CEPH_FS_ONDISK_MAGIC, mds->objecter, logger, l_mdl_jlat, &mds->timer);
  dout(4) << "Writing new journal header " << jp.back << dendl;
  ceph_file_layout new_layout = old_journal->get_layout();
  new_journal->set_writeable();
  new_journal->create(&new_layout, g_conf->mds_journal_format);

  /* Write the new journal header to RADOS */
  C_SaferCond write_head_wait;
  mds->mds_lock.Lock();
  new_journal->write_head(&write_head_wait);
  mds->mds_lock.Unlock();
  write_head_wait.wait();

  // Read in the old journal, and whenever we have readable events,
  // write them to the new journal.
  int r = 0;

  // The logic in here borrowed from replay_thread expects mds_lock to be held,
  // e.g. between checking readable and doing wait_for_readable so that journaler
  // state doesn't change in between.
  uint32_t events_transcribed = 0;
  mds->mds_lock.Lock();
  while (1) {
    while (!old_journal->is_readable() &&
	   old_journal->get_read_pos() < old_journal->get_write_pos() &&
	   !old_journal->get_error()) {

      // Issue a journal prefetch
      C_SaferCond readable_waiter;
      old_journal->wait_for_readable(&readable_waiter);

      // Wait for a journal prefetch to complete
      mds->mds_lock.Unlock();
      readable_waiter.wait();
      mds->mds_lock.Lock();
    }
    if (old_journal->get_error()) {
      r = old_journal->get_error();
      dout(0) << "_replay journaler got error " << r << ", aborting" << dendl;
      break;
    }

    if (!old_journal->is_readable() &&
	old_journal->get_read_pos() == old_journal->get_write_pos())
      break;

    // Read one serialized LogEvent
    assert(old_journal->is_readable());
    bufferlist bl;
    bool r = old_journal->try_read_entry(bl);
    if (!r && old_journal->get_error())
      continue;
    assert(r);

    // Write (buffered, synchronous) one serialized LogEvent
    events_transcribed += 1;
    new_journal->append_entry(bl);

    // Allow other I/O to advance, e.g. MDS beacons
    mds->mds_lock.Unlock();
    mds->mds_lock.Lock();
  }
  mds->mds_lock.Unlock();

  dout(1) << "Transcribed " << events_transcribed << " events, flushing new journal" << dendl;
  C_SaferCond flush_waiter;
  mds->mds_lock.Lock();
  new_journal->flush(&flush_waiter);
  mds->mds_lock.Unlock();
  flush_waiter.wait();

  // If failed to rewrite journal, leave the part written journal
  // as garbage to be cleaned up next startup.
  assert(r == 0);

  /* Now that the new journal is safe, we can flip the pointers */
  inodeno_t const tmp = jp.front;
  jp.front = jp.back;
  jp.back = tmp;
  write_result = jp.save(mds->objecter, &(mds->mds_lock));
  assert(write_result == 0);

  /* Delete the old journal to free space */
  dout(1) << "New journal flushed, erasing old journal" << dendl;
  C_SaferCond erase_waiter;
  mds->mds_lock.Lock();
  old_journal->erase(&erase_waiter);
  mds->mds_lock.Unlock();
  int erase_result = erase_waiter.wait();
  assert(erase_result == 0);
  delete old_journal;

  /* Update the pointer to reflect we're back in clean single journal state. */
  jp.back = 0;
  write_result = jp.save(mds->objecter, &(mds->mds_lock));
  assert(write_result == 0);

  /* Reset the Journaler object to its default state */
  dout(1) << "Journal rewrite complete, continuing with normal startup" << dendl;
  journaler = new_journal;
  journaler->set_readonly();
  journaler->set_write_error_handler(new C_MDL_WriteError(this));

  /* Trigger completion */
  mds->mds_lock.Lock();
  completion->complete(0);
  mds->mds_lock.Unlock();
}


// i am a separate thread
void MDLog::_replay_thread()
{
  mds->mds_lock.Lock();
  dout(10) << "_replay_thread start" << dendl;

  // loop
  int r = 0;
  while (1) {
    // wait for read?
    while (!journaler->is_readable() &&
	   journaler->get_read_pos() < journaler->get_write_pos() &&
	   !journaler->get_error()) {
      journaler->wait_for_readable(new C_MDL_Replay(this));
      replay_cond.Wait(mds->mds_lock);
    }
    if (journaler->get_error()) {
      r = journaler->get_error();
      dout(0) << "_replay journaler got error " << r << ", aborting" << dendl;
      if (r == -ENOENT) {
	// journal has been trimmed by somebody else?
	assert(journaler->is_readonly());
	r = -EAGAIN;
      } else if (r == -EINVAL) {
        if (journaler->get_read_pos() < journaler->get_expire_pos()) {
          // this should only happen if you're following somebody else
          assert(journaler->is_readonly());
          dout(0) << "expire_pos is higher than read_pos, returning EAGAIN" << dendl;
          r = -EAGAIN;
        } else {
          /* re-read head and check it
           * Given that replay happens in a separate thread and
           * the MDS is going to either shut down or restart when
           * we return this error, doing it synchronously is fine
           * -- as long as we drop the main mds lock--. */
          Mutex mylock("MDLog::_replay_thread lock");
          Cond cond;
          bool done = false;
          int err = 0;
          journaler->reread_head(new C_SafeCond(&mylock, &cond, &done, &err));
          mds->mds_lock.Unlock();
	  mylock.Lock();
          while (!done)
            cond.Wait(mylock);
	  mylock.Unlock();
          if (err) { // well, crap
            dout(0) << "got error while reading head: " << cpp_strerror(err)
                    << dendl;
            mds->suicide();
          }
          mds->mds_lock.Lock();
	  standby_trim_segments();
          if (journaler->get_read_pos() < journaler->get_expire_pos()) {
            dout(0) << "expire_pos is higher than read_pos, returning EAGAIN" << dendl;
            r = -EAGAIN;
          }
        }
      }
      break;
    }
    
    if (!journaler->is_readable() &&
	journaler->get_read_pos() == journaler->get_write_pos())
      break;
    
    assert(journaler->is_readable());
    
    // read it
    uint64_t pos = journaler->get_read_pos();
    bufferlist bl;
    bool r = journaler->try_read_entry(bl);
    if (!r && journaler->get_error())
      continue;
    assert(r);
    
    // unpack event
    LogEvent *le = LogEvent::decode(bl);
    if (!le) {
      dout(0) << "_replay " << pos << "~" << bl.length() << " / " << journaler->get_write_pos() 
	      << " -- unable to decode event" << dendl;
      dout(0) << "dump of unknown or corrupt event:\n";
      bl.hexdump(*_dout);
      *_dout << dendl;

      assert(!!"corrupt log event" == g_conf->mds_log_skip_corrupt_events);
      continue;
    }
    le->set_start_off(pos);

    // new segment?
    if (le->get_type() == EVENT_SUBTREEMAP ||
	le->get_type() == EVENT_RESETJOURNAL) {
      segments[pos] = new LogSegment(pos);
      logger->set(l_mdl_seg, segments.size());
    }

    // have we seen an import map yet?
    if (segments.empty()) {
      dout(10) << "_replay " << pos << "~" << bl.length() << " / " << journaler->get_write_pos() 
	       << " " << le->get_stamp() << " -- waiting for subtree_map.  (skipping " << *le << ")" << dendl;
    } else {
      dout(10) << "_replay " << pos << "~" << bl.length() << " / " << journaler->get_write_pos() 
	       << " " << le->get_stamp() << ": " << *le << dendl;
      le->_segment = get_current_segment();    // replay may need this
      le->_segment->num_events++;
      le->_segment->end = journaler->get_read_pos();
      num_events++;

      le->replay(mds);
    }
    delete le;

    logger->set(l_mdl_rdpos, pos);

    // drop lock for a second, so other events/messages (e.g. beacon timer!) can go off
    mds->mds_lock.Unlock();
    mds->mds_lock.Lock();
  }

  // done!
  if (r == 0) {
    assert(journaler->get_read_pos() == journaler->get_write_pos());
    dout(10) << "_replay - complete, " << num_events
	     << " events" << dendl;

    logger->set(l_mdl_expos, journaler->get_expire_pos());
  }

  dout(10) << "_replay_thread kicking waiters" << dendl;
  finish_contexts(g_ceph_context, waitfor_replay, r);  

  dout(10) << "_replay_thread finish" << dendl;
  mds->mds_lock.Unlock();
}

void MDLog::standby_trim_segments()
{
  dout(10) << "standby_trim_segments" << dendl;
  uint64_t expire_pos = journaler->get_expire_pos();
  dout(10) << " expire_pos=" << expire_pos << dendl;
  bool removed_segment = false;
  while (have_any_segments()) {
    LogSegment *seg = get_oldest_segment();
    if (seg->end > expire_pos)
      break;
    dout(10) << " removing segment " << seg->offset << dendl;
    seg->dirty_dirfrags.clear_list();
    seg->new_dirfrags.clear_list();
    seg->dirty_inodes.clear_list();
    seg->dirty_dentries.clear_list();
    seg->open_files.clear_list();
    seg->dirty_parent_inodes.clear_list();
    seg->dirty_dirfrag_dir.clear_list();
    seg->dirty_dirfrag_nest.clear_list();
    seg->dirty_dirfrag_dirfragtree.clear_list();
    remove_oldest_segment();
    removed_segment = true;
  }

  if (removed_segment) {
    dout(20) << " calling mdcache->trim!" << dendl;
    mds->mdcache->trim(-1);
  } else
    dout(20) << " removed no segments!" << dendl;
}
