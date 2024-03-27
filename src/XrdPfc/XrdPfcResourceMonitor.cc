#include "XrdPfcResourceMonitor.hh"
#include "XrdPfc.hh"
#include "XrdPfcPathParseTools.hh"
#include "XrdPfcFsTraversal.hh"
#include "XrdPfcDirState.hh"
#include "XrdPfcTrace.hh"

using namespace XrdPfc;

namespace
{
   XrdSysTrace* GetTrace() { return Cache::GetInstance().GetTrace(); }
   const char *m_traceID = "ResourceMonitor";
}

ResourceMonitor::ResourceMonitor(XrdOss& oss) :
   m_fs_state(new DataFsState),
   m_oss(oss)
{}

ResourceMonitor::~ResourceMonitor()
{
   delete m_fs_state;
}

//------------------------------------------------------------------------------
// Initial scan
//------------------------------------------------------------------------------

void ResourceMonitor::scan_dir_and_recurse(FsTraversal &fst)
{
   printf("In scan_dir_and_recurse for '%s', size of dir_vec = %d, file_stat_map = %d\n",
           fst.m_current_path.c_str(),
          (int)fst.m_current_dirs.size(), (int)fst.m_current_files.size());

   // Breadth first, accumulate into "here"
   DirUsage &here = fst.m_dir_state->m_here_usage;
   for (auto it = fst.m_current_files.begin(); it != fst.m_current_files.end(); ++it)
   {
      printf("would be doing something with %s ... has_data=%d, has_cinfo=%d\n",
              it->first.c_str(), it->second.has_data, it->second.has_cinfo);

      // XXX Make some of these optional?
      // Remove files that do not have both cinfo and data?
      // Remove empty directories before even descending?
      // Leave this for some consistency pass?
      // Note that FsTraversal supports ignored paths ... some details (cofig, N2N to be clarified).

      if (it->second.has_data && it->second.has_cinfo) {
         here.m_bytes_on_disk += it->second.stat_data.st_blocks * 512;
         here.m_num_files     += 1;
      }
   }

   // Sub-dirs second, accumulate into "subdirs".
   DirUsage &subdirs = fst.m_dir_state->m_recursive_subdir_usage;
   std::vector<std::string> dirs;
   dirs.swap(fst.m_current_dirs);
   for (auto &dname : dirs)
   {
      if (fst.cd_down(dname))
      {
         DirState *daughter = fst.m_dir_state;
         DirUsage &dhere    = daughter->m_here_usage;
         DirUsage &dsubdirs = daughter->m_recursive_subdir_usage;

         scan_dir_and_recurse(fst);
         fst.cd_up();

         here.m_num_subdirs += 1;

         subdirs.m_bytes_on_disk += dhere.m_bytes_on_disk + dsubdirs.m_bytes_on_disk;
         subdirs.m_num_files     += dhere.m_num_files     + dsubdirs.m_num_files;
         subdirs.m_num_subdirs   += dhere.m_num_subdirs   + dsubdirs.m_num_subdirs;
      }
      // XXX else try to remove it?
   }
}

bool ResourceMonitor::perform_initial_scan()
{
   // Called after PFC configuration is complete, but before full startup of the daemon.
   // Base line usages are accumulated as part of the file-system, traversal.

   bool success_p = true;

   FsTraversal fst(m_oss);
   fst.m_protected_top_dirs.insert("pfc-stats"); // XXXX This should come from config. Also: N2N?

   if (fst.begin_traversal(m_fs_state->get_root(), "/"))
   {
      scan_dir_and_recurse(fst);
   }
   else
   {
      // Fail startup, can't open /.
      success_p = false;
   }
   fst.end_traversal();

   return success_p;
}

//------------------------------------------------------------------------------
// Processing of queues
//------------------------------------------------------------------------------

int ResourceMonitor::process_queues()
{
   static const char *trc_pfx = "process_queues() ";

   // Assure that we pick up only entries that are present now.
   // We really want all open records to be processed before file-stats so let's get
   // the current snapshot of already opened files first.
   // On the other hand, we do not care if we miss some updates in this pass.
   int n_records = Cache::GetInstance().CopyOutActiveStats(m_file_stats_update_vec);
   {
      XrdSysMutexHelper _lock(&m_queue_mutex);
      n_records += m_file_open_q.swap_queues();
      n_records += m_file_close_q.swap_queues();
      n_records += m_file_purge_q1.swap_queues();
      n_records += m_file_purge_q2.swap_queues();
      n_records += m_file_purge_q3.swap_queues();
   }

   for (auto &i : m_file_open_q.read_queue()) {
      // time is in stat, fname is in the token slot;
      int tid = i.id;
      AccessToken &at = token(tid);
      printf("process file open for token %d, time %ld -- %s\n", tid, i.record.m_open_time, at.m_filename.c_str());
      // At this point we need to resolve fname into DirState.
      // We ould clear the filename after this ... or keep it, should we need it later on.
      // - now just used for printing.
      DirState *ds = m_fs_state->get_root()->find_path(at.m_filename, -1, true, true);
      at.m_dir_state = ds;
      ds->m_here_stats.m_NFilesOpened += 1;

      // XXXXXX Here (or, a bit before) we should figure out how many new directories were created here.
      // find_path could take an optional arg, last-dir-found -- would also help with purge troubles.
      // This should then set, if needed NFiles/Dirs_Created.

      ds->m_here_usage.m_last_open_time = i.record.m_open_time;
   }

   for (auto &i : m_file_stats_update_vec) {
      int tid = i.first;
      AccessToken &at = token(tid);
      // Stats
      DirState *ds = at.m_dir_state;
      printf("process file update for token %d, %p -- %s\n",
             tid, ds, at.m_filename.c_str());

      ds->m_here_stats.AddUp(i.second);
   }
   m_file_stats_update_vec.clear();

   for (auto &i : m_file_close_q.read_queue()) {
      // Stat and close_time are in the STAT == CloseRecord
      // Remember to release the token !!!
      int tid = i.id;
      AccessToken &at = token(tid);
      printf("process file close for token %d, time %ld -- %s\n",
             tid, i.record.m_close_time, at.m_filename.c_str());

      DirState *ds = at.m_dir_state;
      ds->m_here_stats.m_NFilesClosed += 1;

      ds->m_here_usage.m_last_close_time = i.record.m_close_time;

      at.clear();
      m_access_tokens_free_slots.push_back(tid);
   }

   for (auto &i : m_file_purge_q1.read_queue()) {
      // ID is DirState*, multiple files
      DirState *ds = i.id;
      ds->m_here_stats.m_BytesRemoved  += i.record.m_total_size;
      ds->m_here_stats.m_NFilesRemoved += i.record.n_files;
   }
   for (auto &i : m_file_purge_q2.read_queue()) {
      // ID is directory-path, multiple files
      DirState *ds = m_fs_state->get_root()->find_path(i.id, -1, false, false);
      if ( ! ds) {
         TRACE(Error, trc_pfx << "DirState not found for directory path '" << i.id << "'.");
         // XXX Should have find_path return the last dir / depth still found?
         continue;
      }
      ds->m_here_stats.m_BytesRemoved  += i.record.m_total_size;
      ds->m_here_stats.m_NFilesRemoved += i.record.n_files;
   }
   for (auto &i : m_file_purge_q3.read_queue()) {
      // ID is LFN, single file
      DirState *ds = m_fs_state->get_root()->find_path(i.id, -1, true, false);
      if ( ! ds) {
         TRACE(Error, trc_pfx << "DirState not found for LFN path '" << i.id << "'.");
         // XXX Should have find_path return the last dir / depth still found?
         continue;
      }
      ds->m_here_stats.m_BytesRemoved  += i.record;
      ds->m_here_stats.m_NFilesRemoved += 1;
   }

   // XXXX Upward-propagate here or in heart_beat, as neeed?

   // XXXX dir  purge record processing --- seems it is not needed, auto-purge with some cool-off.

   // XXXX clear the read queue, re-capacity to 50% if usage is below 25%

   return n_records;
}

//------------------------------------------------------------------------------
// Heart beat
//------------------------------------------------------------------------------

void ResourceMonitor::heart_beat()
{
   printf("RMon entering heart_beat!\n");

   // initial scan performed as part of config

   while (true)
   {
      int n_processed = process_queues();

      printf("processed %d records\n", n_processed);

      // check if more to process (just sum of sizes, not swap)?

      // check time, is it time to apply the deltas
   m_fs_state->upward_propagate_stats(); // XXXXX before or after export?
      // XXXXX ???? One could, maybe, just prop upwards the subdirs-only part .. and reset them.
      // No, how could this work?

      // check time, is it time to export into vector format, to disk /pfc-stats

      // Dump statistcs before actual purging so maximum usage values get recorded.
      // Should really go to gstream --- and should really go from Heartbeat.
      if (Cache::Conf().is_dir_stat_reporting_on())
      {
         m_fs_state->dump_recursively(Cache::Conf().m_dirStatsStoreDepth);
      }

   m_fs_state->reset_stats(); // XXXXXX this is for sure after export, otherwise they will be zero


      // check time / diskusage --> purge condition?
      // run purge as job or thread
      // m_fs_state->upward_propagate_usage_purged(); // XXXX this is the old way


      // if no work, sleep for N seconds
      unsigned int t_sleep = 10;
      printf("sleeping for %u seconds, to be improved ...\n", t_sleep);
      sleep(t_sleep);
   }
}


//==============================================================================
// Old prototype from Cache / Purge, now to go into heart_beat() here, above.
//==============================================================================

void Proto_ResourceMonitorHeartBeat()
{
   // static const char *trc_pfx = "ResourceMonitorHeartBeat() ";

   // Pause before initial run
   sleep(1);

   // XXXX Setup initial / constant stats (total RAM, total disk, ???)

   XrdOucCacheStats             &S = Cache::GetInstance().Statistics;
   XrdOucCacheStats::CacheStats &X = S.X;

   S.Lock();

   X.DiskSize = Cache::Conf().m_diskTotalSpace;

   X.MemSize = Cache::Conf().m_RamAbsAvailable;

   S.UnLock();

   // XXXX Schedule initial disk scan, time it!
   //
   // TRACE(Info, trc_pfx << "scheduling intial disk scan.");
   // schedP->Schedule( new ScanAndPurgeJob("XrdPfc::ScanAndPurge") );
   //
   // bool scan_and_purge_running = true;

   // XXXX Could we really hold last-usage for all files in memory?

   // XXXX Think how to handle disk-full, scan/purge not finishing:
   // - start dropping things out of write queue, but only when RAM gets near full;
   // - monitoring this then becomes a high-priority job, inner loop with sleep of,
   //   say, 5 or 10 seconds.

   while (true)
   {
      time_t heartbeat_start = time(0);

      // TRACE(Info, trc_pfx << "HeartBeat starting ...");

      // if sumary monitoring configured, pupulate OucCacheStats:
      S.Lock();

      // - available / used disk space (files usage calculated elsewhere (maybe))

      // - RAM usage
      /* XXXX From Cache
      {  XrdSysMutexHelper lck(&m_RAM_mutex);
         X.MemUsed   = m_RAM_used;
         X.MemWriteQ = m_RAM_write_queue;
      }
      */

      // - files opened / closed etc

      // do estimate of available space
      S.UnLock();

      // if needed, schedule purge in a different thread.
      // purge is:
      // - deep scan + gather FSPurgeState
      // - actual purge
      //
      // this thread can continue running and, if needed, stop writing to disk
      // if purge is taking too long.

      // think how data is passed / synchronized between this and purge thread

      // !!!! think how stat collection is done and propgated upwards;
      // until now it was done once per purge-interval.
      // now stats will be added up more often, but purge will be done
      // only occasionally.
      // also, do we report cumulative values or deltas? cumulative should
      // be easier and consistent with summary data.
      // still, some are state - like disk usage, num of files.

      // Do we take care of directories that need to be newly added into DirState hierarchy?
      // I.e., when user creates new directories and these are covered by either full
      // spec or by root + depth declaration.

      int heartbeat_duration = time(0) - heartbeat_start;

      // TRACE(Info, trc_pfx << "HeartBeat finished, heartbeat_duration " << heartbeat_duration);

      // int sleep_time = m_configuration.m_purgeInterval - heartbeat_duration;
      int sleep_time = 60 - heartbeat_duration;
      if (sleep_time > 0)
      {
         sleep(sleep_time);
      }
   }
}
