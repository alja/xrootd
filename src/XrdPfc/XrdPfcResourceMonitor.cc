#include "XrdPfcResourceMonitor.hh"
#include "XrdPfc.hh"
#include "XrdPfcPathParseTools.hh"
#include "XrdPfcFsTraversal.hh"
#include "XrdPfcDirState.hh"
#include "XrdPfcDirStateSnapshot.hh"
#include "XrdPfcTrace.hh"

#include "XrdOss/XrdOss.hh"

#include <algorithm>

#define RM_DEBUG
#ifdef RM_DEBUG
#define dprintf(...) printf(__VA_ARGS__)
#else
#define dprintf(...) (void(0))
#endif

using namespace XrdPfc;

namespace
{
   XrdSysTrace* GetTrace() { return Cache::GetInstance().GetTrace(); }
   const char *m_traceID = "ResourceMonitor";
}

//------------------------------------------------------------------------------

ResourceMonitor::ResourceMonitor(XrdOss& oss) :
   m_fs_state(* new DataFsState),
   m_oss(oss)
{}

ResourceMonitor::~ResourceMonitor()
{
   delete &m_fs_state;
}

//------------------------------------------------------------------------------
// Initial scan
//------------------------------------------------------------------------------

void ResourceMonitor::CrossCheckIfScanIsInProgress(const std::string &lfn, XrdSysCondVar &cond)
{
   m_dir_scan_mutex.Lock();
   if (m_dir_scan_in_progress) {
      m_dir_scan_open_requests.push_back({lfn, cond});
      LfnCondRecord &lcr = m_dir_scan_open_requests.back();
      cond.Lock();
      m_dir_scan_mutex.UnLock();
      while ( ! lcr.f_checked)
         cond.Wait();
      cond.UnLock();
   } else {
      m_dir_scan_mutex.UnLock();
   }
}

void ResourceMonitor::process_inter_dir_scan_open_requests(FsTraversal &fst)
{
   m_dir_scan_mutex.Lock();
   while ( ! m_dir_scan_open_requests.empty())
   {
      LfnCondRecord &lcr = m_dir_scan_open_requests.front();
      m_dir_scan_mutex.UnLock();

      cross_check_or_process_oob_lfn(lcr.f_lfn, fst);
      lcr.f_cond.Lock();
      lcr.f_checked = true;
      lcr.f_cond.Signal();
      lcr.f_cond.UnLock();

      m_dir_scan_mutex.Lock();
      m_dir_scan_open_requests.pop_front();
   }
   m_dir_scan_mutex.UnLock();
}

void ResourceMonitor::cross_check_or_process_oob_lfn(const std::string &lfn, FsTraversal &fst)
{
   // Check if lfn has already been processed ... or process it now and mark
   // the DirState accordingly (partially processed oob).
   static const char *trc_pfx = "cross_check_or_process_oob_lfn() ";

   DirState *last_existing_ds = nullptr;
   DirState *ds = m_fs_state.find_dirstate_for_lfn(lfn, &last_existing_ds);
   if (ds->m_scanned)
      return;

   size_t pos = lfn.find_last_of("/");
   std::string dir = (pos == std::string::npos) ? "" : lfn.substr(0, pos);

   XrdOssDF *dhp = m_oss.newDir(trc_pfx);
   if (dhp->Opendir(dir.c_str(), fst.default_env()) == XrdOssOK)
   {
      fst.slurp_dir_ll(*dhp, ds->m_depth, dir.c_str(), trc_pfx);

      // XXXX clone of function below .... move somewhere? Esp. removal of non-paired files?
      DirUsage &here = ds->m_here_usage;
      for (auto it = fst.m_current_files.begin(); it != fst.m_current_files.end(); ++it)
      {
         if (it->second.has_data && it->second.has_cinfo) {
            here.m_StBlocks += it->second.stat_data.st_blocks;
            here.m_NFiles   += 1;
         }
      }
   }
   delete dhp;
   ds->m_scanned = true;
}

void ResourceMonitor::scan_dir_and_recurse(FsTraversal &fst)
{
   dprintf("In scan_dir_and_recurse for '%s', size of dir_vec = %d, file_stat_map = %d\n",
           fst.m_current_path.c_str(),
          (int)fst.m_current_dirs.size(), (int)fst.m_current_files.size());

   // Breadth first, accumulate into "here", unless it was already scanned via an
   // OOB open file request.
   if ( ! fst.m_dir_state->m_scanned)
   {
      DirUsage &here = fst.m_dir_state->m_here_usage;
      for (auto it = fst.m_current_files.begin(); it != fst.m_current_files.end(); ++it)
      {
         dprintf("would be doing something with %s ... has_data=%d, has_cinfo=%d\n",
               it->first.c_str(), it->second.has_data, it->second.has_cinfo);

         // XXX Make some of these optional?
         // Remove files that do not have both cinfo and data?
         // Remove empty directories before even descending?
         // Leave this for some consistency pass?
         // Note that FsTraversal supports ignored paths ... some details (config, N2N) to be clarified.

         if (it->second.has_data && it->second.has_cinfo) {
            here.m_StBlocks += it->second.stat_data.st_blocks;
            here.m_NFiles   += 1;
         }
      }
      fst.m_dir_state->m_scanned = true;
   }

   // Swap-out directories as inter_dir_scan can use the FsTraversal.
   std::vector<std::string> dirs;
   dirs.swap(fst.m_current_dirs);

   if (++m_dir_scan_check_counter >= 100)
   {
      process_inter_dir_scan_open_requests(fst);
      m_dir_scan_check_counter = 0;
   }

   // Descend into sub-dirs, do not accumulate into recursive_subdir_usage yet. This is done
   // in a separate pass to allow for proper accounting of files being opened during the initial scan.
   for (auto &dname : dirs)
   {
      if (fst.cd_down(dname))
      {
         scan_dir_and_recurse(fst);
         fst.cd_up();
      }
      // XXX else try to remove it?
   }
}

bool ResourceMonitor::perform_initial_scan()
{
   // Called after PFC configuration is complete, but before full startup of the daemon.
   // Base line usages are accumulated as part of the file-system, traversal.

   update_vs_and_file_usage_info();

   DirState   *root_ds = m_fs_state.get_root();
   FsTraversal fst(m_oss);
   fst.m_protected_top_dirs.insert("pfc-stats"); // XXXX This should come from config. Also: N2N?

   if ( ! fst.begin_traversal(root_ds, "/"))
      return false;

   {
      XrdSysMutexHelper _lock(m_dir_scan_mutex);
      m_dir_scan_in_progress = true;
      m_dir_scan_check_counter = 0; // recheck oob file-open requests periodically.
   }

   scan_dir_and_recurse(fst);

   fst.end_traversal();

   // We have all directories scanned, available in DirState tree, let all remaining files go
   // and then we shall do the upward propagation of usages.
   {
      XrdSysMutexHelper _lock(m_dir_scan_mutex);
      m_dir_scan_in_progress = false;
      m_dir_scan_check_counter = 0;

      while ( ! m_dir_scan_open_requests.empty())
      {
         LfnCondRecord &lcr = m_dir_scan_open_requests.front();
         lcr.f_cond.Lock();
         lcr.f_checked = true;
         lcr.f_cond.Signal();
         lcr.f_cond.UnLock();

         m_dir_scan_open_requests.pop_front();
      }
   }

   // Do upward propagation of usages.
   root_ds->upward_propagate_initial_scan_usages();
   m_current_usage_in_st_blocks = root_ds->m_here_usage.m_StBlocks + 
                                  root_ds->m_recursive_subdir_usage.m_StBlocks;
   update_vs_and_file_usage_info();

   return true;
}

//------------------------------------------------------------------------------
// Processing of queues
//------------------------------------------------------------------------------

int ResourceMonitor::process_queues()
{
   static const char *trc_pfx = "process_queues() ";

   // Assure that we pick up only entries that are present now.
   // We really want all open records to be processed before file-stats updates
   // and all those before the close records.
   // Purges are sort of tangential as they really just modify bytes / number
   // of files in a directory and do not deal with any persistent file id tokens.

   int n_records = 0;
   {
      XrdSysMutexHelper _lock(&m_queue_mutex);
      n_records += m_file_open_q.swap_queues();
      n_records += m_file_update_stats_q.swap_queues();
      n_records += m_file_close_q.swap_queues();
      n_records += m_file_purge_q1.swap_queues();
      n_records += m_file_purge_q2.swap_queues();
      n_records += m_file_purge_q3.swap_queues();
      ++m_queue_swap_u1;
   }

   for (auto &i : m_file_open_q.read_queue())
   {
      // i.id: LFN, i.record: OpenRecord
      AccessToken &at = token(i.id);
      dprintf("process file open for token %d, time %ld -- %s\n",
              i.id, i.record.m_open_time, at.m_filename.c_str());

      // Resolve fname into DirState.
      // We could clear the filename after this ... or keep it, should we need it later on.
      // For now it is just used for debug printouts.
      DirState *last_existing_ds = nullptr;
      DirState *ds = m_fs_state.find_dirstate_for_lfn(at.m_filename, &last_existing_ds);
      at.m_dir_state = ds;
      ds->m_here_stats.m_NFilesOpened += 1;

      // If this is a new file figure out how many new parent dirs got created along the way.
      if ( ! i.record.m_existing_file) {
         ds->m_here_stats.m_NFilesCreated += 1;
         DirState *pp = ds;
         while (pp != last_existing_ds) {
            pp = pp->get_parent();
            pp->m_here_stats.m_NDirectoriesCreated += 1;
         }
      }

      ds->m_here_usage.m_LastOpenTime = i.record.m_open_time;
   }

   for (auto &i : m_file_update_stats_q.read_queue())
   {
      // i.id: token, i.record: Stats
      AccessToken &at = token(i.id);
      // Stats
      DirState *ds = at.m_dir_state;
      dprintf("process file update for token %d, %p -- %s\n",
             i.id, ds, at.m_filename.c_str());

      ds->m_here_stats.AddUp(i.record);
      m_current_usage_in_st_blocks += i.record.m_StBlocksAdded;
   }

   for (auto &i : m_file_close_q.read_queue())
   {
      // i.id: token, i.record: CloseRecord
      AccessToken &at = token(i.id);
      dprintf("process file close for token %d, time %ld -- %s\n",
              i.id, i.record.m_close_time, at.m_filename.c_str());

      DirState *ds = at.m_dir_state;
      ds->m_here_stats.m_NFilesClosed += 1;
      ds->m_here_usage.m_LastCloseTime = i.record.m_close_time;

      at.clear();
   }
   { // Release the AccessToken slots under lock.
      XrdSysMutexHelper _lock(&m_queue_mutex);
      for (auto &i : m_file_close_q.read_queue())
         m_access_tokens_free_slots.push_back(i.id);
   }

   for (auto &i : m_file_purge_q1.read_queue())
   {
      // i.id: DirState*, i.record: PurgeRecord
      DirState *ds = i.id;
      ds->m_here_stats.m_StBlocksRemoved += i.record.m_size_in_st_blocks;
      ds->m_here_stats.m_NFilesRemoved   += i.record.m_n_files;
      m_current_usage_in_st_blocks       -= i.record.m_size_in_st_blocks;
   }
   for (auto &i : m_file_purge_q2.read_queue())
   {
      // i.id: directory-path, i.record: PurgeRecord
      DirState *ds = m_fs_state.get_root()->find_path(i.id, -1, false, false);
      if ( ! ds) {
         TRACE(Error, trc_pfx << "DirState not found for directory path '" << i.id << "'.");
         // find_path can return the last dir found ... but this clearly isn't a valid purge record.
         continue;
      }
      ds->m_here_stats.m_StBlocksRemoved += i.record.m_size_in_st_blocks;
      ds->m_here_stats.m_NFilesRemoved   += i.record.m_n_files;
      m_current_usage_in_st_blocks       -= i.record.m_size_in_st_blocks;
   }
   for (auto &i : m_file_purge_q3.read_queue())
   {
      // i.id: LFN, i.record: size of file in st_blocks
      DirState *ds = m_fs_state.get_root()->find_path(i.id, -1, true, false);
      if ( ! ds) {
         TRACE(Error, trc_pfx << "DirState not found for LFN path '" << i.id << "'.");
         continue;
      }
      ds->m_here_stats.m_StBlocksRemoved += i.record;
      ds->m_here_stats.m_NFilesRemoved   += 1;
      m_current_usage_in_st_blocks       -= i.record;
   }

   // Read queues / vectors are cleared at swap time.
   // We might consider reducing their capacity by half if, say, their usage is below 25%.

   return n_records;
}

//------------------------------------------------------------------------------
// Heart beat
//------------------------------------------------------------------------------

void ResourceMonitor::heart_beat()
{
   static const char *tpfx = "heart_beat() ";

   const Configuration &conf    =  Cache::Conf();
   const DirState      &root_ds = *m_fs_state.get_root();

   const int s_queue_proc_interval   = 10;
   // const s_stats_up_prop_interval = 60; -- for when we have dedicated purge / stat report structs
   const int s_sshot_report_interval = 60; // to be bumped (300s?) or made configurable.
   const int s_purge_check_interval  = 60;
   const int s_purge_report_interval = conf.m_purgeInterval;
   const int s_purge_cold_files_interval = conf.m_purgeInterval * conf.m_purgeAgeBasedPeriod;

   // initial scan performed as part of config

   time_t now = time(0);
   time_t next_queue_proc_time       = now + s_queue_proc_interval;
   time_t next_sshot_report_time     = now + s_sshot_report_interval;
   time_t next_purge_check_time      = now + s_purge_check_interval;
   time_t next_purge_report_time     = now + s_purge_report_interval;
   time_t next_purge_cold_files_time = now + s_purge_cold_files_interval;

   // XXXXX On initial entry should reclaim space from queues as they might have grown
   // very large during the initial scan.

   while (true)
   {
      time_t start = time(0);
      time_t next_event = std::min({ next_queue_proc_time, next_sshot_report_time,
                                     next_purge_check_time, next_purge_report_time, next_purge_cold_files_time });

      if (next_event > start)
      {
         unsigned int t_sleep = next_event - start;
         TRACE(Debug, tpfx << "sleeping for " << t_sleep << " seconds until the next beat.");
         sleep(t_sleep);
      }

      // Check if purge has been running and has completed yet.
      // For now this is only used to prevent removal of empty leaf directories
      // during stat propagation so we do not need to wait for the condition in
      // the above sleep.
      if (m_purge_task_active) {
         MutexHolder _lck(m_purge_task_cond);
         if (m_purge_task_complete) {
            m_purge_task_active = m_purge_task_complete = false;
         }
      }

      // Always process the queues.
      int n_processed = process_queues();
      next_queue_proc_time += s_queue_proc_interval;
      TRACE(Debug, tpfx << "process_queues -- n_records=" << n_processed);

      // Always update basic info on m_fs_state (space, usage, file_usage).
      update_vs_and_file_usage_info();

      now = time(0);
      if (next_sshot_report_time <= now)
      {
         next_sshot_report_time += s_sshot_report_interval;

         // XXXX pass in m_purge_task_active as control over "should empty dirs be purged";
         // Or should this be separate pass or variant in purge?
         m_fs_state.upward_propagate_stats_and_times();

         m_fs_state.apply_stats_to_usages();

         // Dump statistics before actual purging so maximum usage values get recorded.
         // This should dump out binary snapshot into /pfc-stats/, if so configured.
         // Also, optionally, json.
         // Could also go to gstream but this easily gets too large.
         if (conf.is_dir_stat_reporting_on())
         {
            const int store_depth  =  conf.m_dirStatsStoreDepth;
            const int n_sshot_dirs =  root_ds.count_dirs_to_level(store_depth);
            dprintf("Snapshot n_dirs=%d, total n_dirs=%d\n", n_sshot_dirs,
                  root_ds.m_here_usage.m_NDirectories + root_ds.m_recursive_subdir_usage.m_NDirectories + 1);

            m_fs_state.dump_recursively(store_depth);

            DataFsSnapshot ss(m_fs_state);
            ss.m_dir_states.reserve(n_sshot_dirs);

            ss.m_dir_states.emplace_back( DirStateElement(root_ds, -1) );
            fill_sshot_vec_children(root_ds, 0, ss.m_dir_states, store_depth);

            // This should really be export to a file (preferably binary, but then bin->json command is needed, too).
            ss.dump();
         }

         m_fs_state.reset_stats();

         now = time(0);
      }

      bool do_purge_check      = next_purge_check_time <= now;
      bool do_purge_report     = next_purge_report_time <= now;
      bool do_purge_cold_files = next_purge_cold_files_time <= now;
      if (do_purge_check || do_purge_report || do_purge_cold_files)
      {
         perform_purge_check(do_purge_cold_files, do_purge_report ? TRACE_Info : TRACE_Debug);

         next_purge_check_time = now + s_purge_check_interval;
         if (do_purge_report) next_purge_report_time = now + s_purge_report_interval;
         if (do_purge_cold_files) next_purge_cold_files_time = now + s_purge_cold_files_interval;
      }

   } // end while forever
}

//------------------------------------------------------------------------------
// DirState export helpers
//------------------------------------------------------------------------------

void ResourceMonitor::fill_sshot_vec_children(const DirState &parent_ds,
                                              int parent_idx,
                                              std::vector<DirStateElement> &vec,
                                              int max_depth)
{
   int pos = vec.size();
   int n_children = parent_ds.m_subdirs.size();

   for (auto const & [name, child] : parent_ds.m_subdirs)
   {
      vec.emplace_back( DirStateElement(child, parent_idx) );
   }

   if (parent_ds.m_depth < max_depth)
   {
      DirStateElement &parent_dse = vec[parent_idx];
      parent_dse.m_daughters_begin = pos;
      parent_dse.m_daughters_end   = pos + n_children;

      for (auto const & [name, child] : parent_ds.m_subdirs)
      {
         if (n_children > 0)
            fill_sshot_vec_children(child, pos, vec, max_depth);
         ++pos;
      }
   }
}

void ResourceMonitor::fill_pshot_vec_children(const DirState &parent_ds,
                                              int parent_idx,
                                              std::vector<DirPurgeElement> &vec,
                                              int max_depth)
{
   int pos = vec.size();
   int n_children = parent_ds.m_subdirs.size();

   for (auto const & [name, child] : parent_ds.m_subdirs)
   {
      vec.emplace_back( DirPurgeElement(child, parent_idx) );
   }

   if (parent_ds.m_depth < max_depth)
   {
      DirPurgeElement &parent_dpe = vec[parent_idx];
      parent_dpe.m_daughters_begin = pos;
      parent_dpe.m_daughters_end   = pos + n_children;

      for (auto const & [name, child] : parent_ds.m_subdirs)
      {
         if (n_children > 0)
            fill_pshot_vec_children(child, pos, vec, max_depth);
         ++pos;
      }
   }
}

//------------------------------------------------------------------------------
// Purge helpers, drivers, etc.
//------------------------------------------------------------------------------

void ResourceMonitor::update_vs_and_file_usage_info()
{
   static const char *trc_pfx = "update_vs_and_file_usage_info() ";

   const auto &conf = Cache::Conf();
   XrdOssVSInfo vsi;

   // StatVS error (after it succeeded in config) implies a memory corruption (according to Mr. H).
   if (m_oss.StatVS(&vsi, conf.m_data_space.c_str(), 1) < 0) {
      TRACE(Error, trc_pfx << "can't get StatVS for oss space '" << conf.m_data_space << "'. This is a fatal error.");
      _exit(1);
   }
   m_fs_state.m_disk_total = vsi.Total;
   m_fs_state.m_disk_used  = vsi.Total - vsi.Free;
   m_fs_state.m_file_usage = 512ll * m_current_usage_in_st_blocks;
   if (m_oss.StatVS(&vsi, conf.m_meta_space.c_str(), 1) < 0) {
      TRACE(Error, trc_pfx << "can't get StatVS for oss space '" << conf.m_meta_space << "'. This is a fatal error.");
      _exit(1);
   }
   m_fs_state.m_meta_total = vsi.Total;
   m_fs_state.m_meta_used  = vsi.Total - vsi.Free;
}

void ResourceMonitor::perform_purge_check(bool purge_cold_files, int tl)
{
   static const char *trc_pfx = "perform_purge_check() ";
   const Configuration &conf = Cache::Conf();

   std::unique_ptr<DataFsPurgeshot> psp( new DataFsPurgeshot(m_fs_state) );
   DataFsPurgeshot &ps = *psp;

   // Purge precheck I. -- available disk space

   if (ps.m_disk_used > conf.m_diskUsageHWM) {
      ps.m_bytes_to_remove_d = ps.m_disk_used - conf.m_diskUsageLWM;
      ps.m_space_based_purge = true;
   }

   // Purge precheck II. -- usage by files.
   // Keep it updated, but only act on it if so configured.

   ps.m_file_usage = 512ll * m_current_usage_in_st_blocks;
   // These are potentially wrong as cache might be writing over preallocated byte ranges.
   ps.m_estimated_writes_from_writeq = Cache::GetInstance().WritesSinceLastCall();
   // Can have another estimate based on eiter writes or st-blocks from purge-stats, once we have them.

   if (conf.are_file_usage_limits_set())
   {
      ps.m_bytes_to_remove_f = std::max(ps.m_file_usage - conf.m_fileUsageNominal, 0ll);

      // Here we estimate fractional usages -- to decide if full scan is necessary before actual purge.
      double frac_du = 0, frac_fu = 0;
      conf.calculate_fractional_usages(ps.m_disk_used, ps.m_file_usage, frac_du, frac_fu);

      if (frac_fu > 1.0 - frac_du)
      {
         ps.m_bytes_to_remove_f = std::max(ps.m_bytes_to_remove_f, ps.m_disk_used - conf.m_diskUsageLWM);
         ps.m_space_based_purge = true;
      }
   }

   ps.m_bytes_to_remove = std::max(ps.m_bytes_to_remove_d, ps.m_bytes_to_remove_f);

   // Purge precheck III. -- check if age-based purge is required
   // We ignore uvkeep time, it requires reading of cinfo files and it is enforced in File::Open() anyway.

   if (purge_cold_files && conf.is_age_based_purge_in_effect()) // || conf.is_uvkeep_purge_in_effect())
   {
      ps.m_age_based_purge = true;
   }

   TRACE_INT(tl, trc_pfx << "Purge check:");
   TRACE_INT(tl, "\tbytes_to_remove_disk    = " << ps.m_bytes_to_remove_d << " B");
   TRACE_INT(tl, "\tbytes_to remove_files   = " << ps.m_bytes_to_remove_f << " B");
   TRACE_INT(tl, "\tbytes_to_remove         = " << ps.m_bytes_to_remove   << " B");
   TRACE_INT(tl, "\tspace_based_purge = " << ps.m_space_based_purge);
   TRACE_INT(tl, "\tage_based_purge   = " << ps.m_age_based_purge);

   if ( ! ps.m_space_based_purge && ! ps.m_age_based_purge) {
      TRACE(Info, trc_pfx << "purge not required.");
      Cache::GetInstance().ClearPurgeProtectedSet();
      return;
   }
   if (m_purge_task_active) {
      TRACE(Warning, trc_pfx << "purge required but previous purge task is still active!");
      return;
   }

   TRACE(Info, trc_pfx << "purge required ... scheduling purge task.");

   // At this point we have all the information: report, decide on action.
   // There is still some missing infrastructure, especially as regards to purge-plugin:
   // - at what point do we start bugging the pu-pin to start coughing up purge lists?
   //   - have a new parameter or just do it "one cycle before full"?
   //   - what if it doesn't -- when do we do the old-stlye scan & purge?
   // - how do we do age-based purge and uvkeep purge?
   //   - they are really quite different -- and could run separately, registering
   //     files into a purge-candidate list. This has to be rechecked before the actual
   //     deletion -- eg, by comparing stat time of cinfo + doing the is-active / is-purge-protected.

   const DirState &root_ds = *m_fs_state.get_root();
   const int n_pshot_dirs = root_ds.count_dirs_to_level(9999);
   const int n_calc_dirs  = 1 + root_ds.m_here_usage.m_NDirectories + root_ds.m_recursive_subdir_usage.m_NDirectories;
   dprintf("purge dir count recursive=%d vs from_usage=%d\n", n_pshot_dirs, n_calc_dirs);

   ps.m_dir_vec.reserve(n_calc_dirs);
   ps.m_dir_vec.emplace_back( DirPurgeElement(root_ds, -1) );
   fill_pshot_vec_children(root_ds, 0, ps.m_dir_vec, 9999);

   m_purge_task_active = true;

   struct PurgeDriverJob : public XrdJob
   {
      DataFsPurgeshot  *m_purge_shot_ptr;

      PurgeDriverJob(DataFsPurgeshot *psp) :
         XrdJob("XrdPfc::ResourceMonitor::PurgeDriver"),
         m_purge_shot_ptr(psp)
      {}

      void DoIt() override
      {
         Cache::ResMon().perform_purge_task(*m_purge_shot_ptr);
         Cache::ResMon().perform_purge_task_cleanup();

         delete m_purge_shot_ptr;
         delete this;
      }
   };

   Cache::schedP->Schedule( new PurgeDriverJob(psp.release()) );
}

namespace XrdPfc
{
   void OldStylePurgeDriver(DataFsPurgeshot &ps);
}

void ResourceMonitor::perform_purge_task(DataFsPurgeshot &ps)
{
   // BEWARE: Runs in a dedicated thread - is only to communicate back to the
   // hear_beat() / data structs via the purge queues and condition variable.

   // const char *tpfx = "perform_purge_task ";

   {
      MutexHolder _lck(m_purge_task_cond);
      m_purge_task_start = time(0);
   }

   // For now, fall back to the old purge ... to be improved with:
   // - new scan, following the DataFsPurgeshot;
   // - usage of cinfo stat mtime for time of last access (touch already done at output);
   // - use DirState* to report back purged files.
   // Already changed to report back purged files --- but using the string / path variant.
   OldStylePurgeDriver(ps); // In XrdPfcPurge.cc
}

void ResourceMonitor::perform_purge_task_cleanup()
{
   // Separated out so the purge_task can exit without post-checks.

   {
      MutexHolder _lck(m_purge_task_cond);
      m_purge_task_end = time(0);
      m_purge_task_complete = true;
      m_purge_task_cond.Signal();
   }
   Cache::GetInstance().ClearPurgeProtectedSet();
}

//==============================================================================
// Main thread function, do initial test, then enter heart_beat().
//==============================================================================

void ResourceMonitor::init_before_main()
{
   // setup for in-scan -- this is called from initial setup.
   MutexHolder _lck(m_dir_scan_mutex);
   m_dir_scan_in_progress = true;
}

void ResourceMonitor::main_thread_function()
{
   const char *tpfx = "main_thread_function ";
   {
      time_t is_start = time(0);
      TRACE(Info, tpfx << "Stating initial directory scan.");

      if ( ! perform_initial_scan()) {
         TRACE(Error, tpfx << "Initial directory scan has failed. This is a terminal error, aborting.")
         _exit(1);
      }
      // Reset of m_dir_scan_in_progress is done in perform_initial_scan()

      time_t is_duration = time(0) - is_start;
      TRACE(Info, tpfx << "Initial directory scan complete, duration=" << is_duration <<"s");

      // run first process queues
      int n_proc_is = process_queues();
      TRACE(Info, tpfx << "First process_queues finished, n_records=" << n_proc_is);

      // shrink queues if scan time was longer than 30s.
      if (is_duration > 30 || n_proc_is > 3000)
      {
         m_file_open_q.shrink_read_queue();
         m_file_update_stats_q.shrink_read_queue();
         m_file_close_q.shrink_read_queue();
         m_file_purge_q1.shrink_read_queue();
         m_file_purge_q2.shrink_read_queue();
         m_file_purge_q3.shrink_read_queue();
      }
   }
   heart_beat();
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

      // int sleep_time = m_fs_state..m_purgeInterval - heartbeat_duration;
      int sleep_time = 60 - heartbeat_duration;
      if (sleep_time > 0)
      {
         sleep(sleep_time);
      }
   }
}
