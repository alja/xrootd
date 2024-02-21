#include "XrdPfc.hh"
#include "XrdPfcDirState.hh"
#include "XrdPfcFPurgeState.hh"
#include "XrdPfcDirPurge.hh"
#include "XrdPfcTrace.hh"

#include <fcntl.h>
#include <sys/time.h>

#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucUtils.hh"
#include "XrdOss/XrdOssAt.hh"
#include "XrdSys/XrdSysTrace.hh"

using namespace XrdPfc;
namespace XrdPfc
{

/*
XrdSysTrace* GetTrace()
{
   // needed for logging macros
   return Cache::GetInstance().GetTrace();
}*/

//==============================================================================
// DataFsState
//==============================================================================

class DataFsState
{
   int       m_max_depth;
   DirState  m_root;
   time_t    m_prev_time;

public:
   DataFsState() :
      m_max_depth ( Cache::Conf().m_dirStatsStoreDepth ),
      m_root      ( m_max_depth ),
      m_prev_time ( time(0) )
   {}

   int       get_max_depth() const { return m_max_depth; }

   DirState* get_root()            { return & m_root; }

   DirState* find_dirstate_for_lfn(const std::string& lfn)
   {
      return m_root.find_path(lfn, m_max_depth, true, true);
   }

   void reset_stats()                   { m_root.reset_stats();                   }
   void upward_propagate_stats()        { m_root.upward_propagate_stats();        }
   void upward_propagate_usage_purged() { m_root.upward_propagate_usage_purged(); }

   void dump_recursively()
   {
      time_t now = time(0);

      printf("DataFsState::dump_recursively epoch = %lld delta_t = %lld max_depth = %d\n",
             (long long) now, (long long) (now - m_prev_time), m_max_depth);

      m_prev_time = now;

      m_root.dump_recursively("root");
   }
};



const char *FPurgeState::m_traceID = "Purge";


//==============================================================================
// ResourceMonitor
//==============================================================================

// Encapsulates local variables used withing the previous mega-function Purge().
//
// This will be used within the continuously/periodically ran heart-beat / breath
// function ... and then parts of it will be passed to invoked FS scan and purge
// jobs (which will be controlled throught this as well).

class ResourceMonitor
{

};


//==============================================================================
//
//==============================================================================

namespace
{

class ScanAndPurgeJob : public XrdJob
{
public:
   ScanAndPurgeJob(const char *desc = "") : XrdJob(desc) {}

   void DoIt() {} // { Cache::GetInstance().ScanAndPurge(); }
};

}

//==============================================================================
// Cache methods
//==============================================================================

void Cache::copy_out_active_stats_and_update_data_fs_state()
{
   static const char *trc_pfx = "copy_out_active_stats_and_update_data_fs_state() ";

   StatsMMap_t updates;
   {
      XrdSysCondVarHelper lock(&m_active_cond);

      // Slurp in stats from files closed since last cycle.
      updates.swap( m_closed_files_stats );

      for (ActiveMap_i i = m_active.begin(); i != m_active.end(); ++i)
      {
         if (i->second != 0)
         {
            updates.insert(std::make_pair(i->first, i->second->DeltaStatsFromLastCall()));
         }
      }
   }

   m_fs_state->reset_stats(); // XXXX-CKSUM rethink how to do this if we keep some purge entries for next time

   for (StatsMMap_i i = updates.begin(); i != updates.end(); ++i)
   {
      DirState *ds = m_fs_state->find_dirstate_for_lfn(i->first);

      if (ds == 0)
      {
         TRACE(Error, trc_pfx << "Failed finding DirState for file '" << i->first << "'.");
         continue;
      }

      ds->add_up_stats(i->second);
   }

   m_fs_state->upward_propagate_stats();
}


//==============================================================================

void Cache::ResourceMonitorHeartBeat()
{
   // static const char *trc_pfx = "ResourceMonitorHeartBeat() ";

   // Pause before initial run
   sleep(1);

   // XXXX Setup initial / constant stats (total RAM, total disk, ???)

   XrdOucCacheStats             &S = Statistics;
   XrdOucCacheStats::CacheStats &X = Statistics.X;

   S.Lock();

   X.DiskSize = m_configuration.m_diskTotalSpace;

   X.MemSize = m_configuration.m_RamAbsAvailable;

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
      {  XrdSysMutexHelper lck(&m_RAM_mutex);
         X.MemUsed   = m_RAM_used;
         X.MemWriteQ = m_RAM_write_queue;
      }
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

//==============================================================================

void Cache::Purge()
{
   static const char *trc_pfx = "Purge() ";

   XrdOucEnv    env;
   long long    disk_usage;
   long long    estimated_file_usage = m_configuration.m_diskUsageHWM;

   // Pause before initial run
   sleep(1);

   m_fs_state = new DataFsState;

   // { PathTokenizer p("/a/b/c/f.root", 2, true); p.deboog(); }
   // { PathTokenizer p("/a/b/f.root", 2, true); p.deboog(); }
   // { PathTokenizer p("/a/f.root", 2, true); p.deboog(); }
   // { PathTokenizer p("/f.root", 2, true); p.deboog(); }

   int  age_based_purge_countdown = 0; // enforce on first purge loop entry.
   bool is_first = true;

   while (true)
   {
      time_t purge_start = time(0);

      {
         XrdSysCondVarHelper lock(&m_active_cond);

         m_in_purge = true;
      }

      TRACE(Info, trc_pfx << "Started.");

      // Bytes to remove based on total disk usage (d) and file usage (f).
      long long bytesToRemove_d = 0, bytesToRemove_f = 0;

      // get amount of space to potentially erase based on total disk usage
      XrdOssVSInfo sP; // Make sure we start when a clean slate in each loop
      if (m_oss->StatVS(&sP, m_configuration.m_data_space.c_str(), 1) < 0)
      {
         TRACE(Error, trc_pfx << "can't get StatVS for oss space " << m_configuration.m_data_space);
         continue;
      }
      else
      {
         disk_usage = sP.Total - sP.Free;
         TRACE(Debug, trc_pfx << "used disk space " << disk_usage << " bytes.");

         if (disk_usage > m_configuration.m_diskUsageHWM)
         {
            bytesToRemove_d = disk_usage - m_configuration.m_diskUsageLWM;
         }
      }

      // estimate amount of space to erase based on file usage
      if (m_configuration.are_file_usage_limits_set())
      {
         long long estimated_writes_since_last_purge;
         {
            XrdSysCondVarHelper lock(&m_writeQ.condVar);

            estimated_writes_since_last_purge = m_writeQ.writes_between_purges;
            m_writeQ.writes_between_purges = 0;
         }
         estimated_file_usage += estimated_writes_since_last_purge;

         TRACE(Debug, trc_pfx << "estimated usage by files " << estimated_file_usage << " bytes.");

         bytesToRemove_f = std::max(estimated_file_usage - m_configuration.m_fileUsageNominal, 0ll);

         // Here we estimate fractional usages -- to decide if full scan is necessary before actual purge.
         double frac_du = 0, frac_fu = 0;
         m_configuration.calculate_fractional_usages(disk_usage, estimated_file_usage, frac_du, frac_fu);

         if (frac_fu > 1.0 - frac_du)
         {
            bytesToRemove_f = std::max(bytesToRemove_f, disk_usage - m_configuration.m_diskUsageLWM);
         }
      }

      long long bytesToRemove = std::max(bytesToRemove_d, bytesToRemove_f);

      bool enforce_age_based_purge = false;
      if (m_configuration.is_age_based_purge_in_effect() || m_configuration.is_uvkeep_purge_in_effect())
      {
         // XXXX ... I could collect those guys in larger vectors (maps?) and do traversal when
         // they are empty.
         if (--age_based_purge_countdown <= 0)
         {
            enforce_age_based_purge   = true;
            age_based_purge_countdown = m_configuration.m_purgeAgeBasedPeriod;
         }
      }

      bool enforce_traversal_for_usage_collection = is_first;
      // XXX Other conditions? Periodic checks?

      copy_out_active_stats_and_update_data_fs_state();

      TRACE(Debug, trc_pfx << "Precheck:");
      TRACE(Debug, "\tbytes_to_remove_disk    = " << bytesToRemove_d << " B");
      TRACE(Debug, "\tbytes_to remove_files   = " << bytesToRemove_f << " B (" << (is_first ? "max possible for initial run" : "estimated") << ")");
      TRACE(Debug, "\tbytes_to_remove         = " << bytesToRemove   << " B");
      TRACE(Debug, "\tenforce_age_based_purge = " << enforce_age_based_purge);
      is_first = false;

      long long bytesToRemove_at_start = 0; // set after file scan
      int       deleted_file_count     = 0;

      bool purge_required = (bytesToRemove > 0 || enforce_age_based_purge);

      // XXXX-PurgeOpt Need to retain this state between purges so I can avoid doing
      // the traversal more often than really needed.
      FPurgeState purgeState(2 * bytesToRemove, *m_oss); // prepare twice more volume than required

      if (purge_required || enforce_traversal_for_usage_collection)
      {
         // Make a sorted map of file paths sorted by access time.

         if (m_configuration.is_age_based_purge_in_effect())
         {
            purgeState.setMinTime(time(0) - m_configuration.m_purgeColdFilesAge);
         }
         if (m_configuration.is_uvkeep_purge_in_effect())
         {
            purgeState.setUVKeepMinTime(time(0) - m_configuration.m_cs_UVKeep);
         }

         XrdOssDF* dh = m_oss->newDir(m_configuration.m_username.c_str());
         if (dh->Opendir("/", env) == XrdOssOK)
         {
            purgeState.begin_traversal(m_fs_state->get_root());

            purgeState.TraverseNamespace(dh);

            purgeState.end_traversal();

            dh->Close();
         }
         delete dh; dh = 0;

         estimated_file_usage = purgeState.getNBytesTotal();

         TRACE(Debug, trc_pfx << "actual usage by files " << estimated_file_usage << " bytes.");

         // Adjust bytesToRemove_f and then bytesToRemove based on actual file usage,
         // possibly retreating below nominal file usage (but not below baseline file usage).
         if (m_configuration.are_file_usage_limits_set())
         {
            bytesToRemove_f = std::max(estimated_file_usage - m_configuration.m_fileUsageNominal, 0ll);

            double frac_du = 0, frac_fu = 0;
            m_configuration.calculate_fractional_usages(disk_usage, estimated_file_usage, frac_du, frac_fu);

            if (frac_fu > 1.0 - frac_du)
            {
               bytesToRemove = std::max(bytesToRemove_f, disk_usage - m_configuration.m_diskUsageLWM);
               bytesToRemove = std::min(bytesToRemove,   estimated_file_usage - m_configuration.m_fileUsageBaseline);
            }
            else
            {
               bytesToRemove = std::max(bytesToRemove_d, bytesToRemove_f);
            }
         }
         else
         {
            bytesToRemove = std::max(bytesToRemove_d, bytesToRemove_f);
         }
         bytesToRemove_at_start = bytesToRemove;

         TRACE(Debug, trc_pfx << "After scan:");
         TRACE(Debug, "\tbytes_to_remove_disk    = " << bytesToRemove_d << " B");
         TRACE(Debug, "\tbytes_to remove_files   = " << bytesToRemove_f << " B (measured)");
         TRACE(Debug, "\tbytes_to_remove         = " << bytesToRemove   << " B");
         TRACE(Debug, "\tenforce_age_based_purge = " << enforce_age_based_purge);
         TRACE(Debug, "\tmin_time                = " << purgeState.getMinTime());

         if (enforce_age_based_purge)
         {
            purgeState.MoveListEntriesToMap();
         }
      }

      /////////////////////////////////////////////////////////////
      ///
      /// PurgePin begin
      ///
      /////////////////////////////////////////////////////////////
      if (m_dirpurge)
      {
         // set dir stat for each path and calculate nBytes to rocover for each path
         // return total bytes to recover within the plugin
         long long clearVal = m_dirpurge->GetBytesToRecover(m_fs_state->get_root());
         if (clearVal)
         {
            TRACE(Debug, "PurgePin remove total " << clearVal << " bytes");
            DirPurge::list_t &dpl = m_dirpurge->refDirInfos();
            // iterate through the plugin paths
            for (DirPurge::list_i ppit = dpl.begin(); ppit != dpl.end(); ++ppit)
            {
               TRACE(Debug, "\tPurgePin scanning dir " << ppit->path.c_str() << " to remove " << ppit->nBytesToRecover << " bytes");
               XrdOssDF *dh_plg = m_oss->newDir(m_configuration.m_username.c_str());
               FPurgeState purgeState_plg(ppit->nBytesToRecover, *m_oss);
               if (dh_plg->Opendir(ppit->path.c_str(), env) == XrdOssOK)
               {
                  DirState *plg_dirState = ppit->dirState;
                  purgeState_plg.begin_traversal(plg_dirState);
                  purgeState_plg.TraverseNamespace(dh_plg);
                  purgeState_plg.end_traversal();
                  dh_plg->Close();
               }

               // fill central map from the plugin entry
               for (FPurgeState::map_i it = purgeState_plg.refMap().begin(); it != purgeState_plg.refMap().end(); ++it)
               {
                  it->second.path = ppit->path + it->second.path;
                  TRACE(Debug, "\t\tPurgePin found file " << it->second.path.c_str()<< " size " << it->second.nBytes);
                  purgeState.refMap().insert(std::make_pair(0, it->second)); // set atime to zero to make sure this is deleted
               }
            }
            bytesToRemove = std::max(clearVal, bytesToRemove);
            purge_required = true; // set the falg!
         }
      }
      /////////////////////////////////////////////////////////////
      ///
      /// PurgePin end
      ///
      /////////////////////////////////////////////////////////////

      // Dump statistcs before actual purging so maximum usage values get recorded.
      // Should really go to gstream --- and should really go from Heartbeat.
      if (m_configuration.is_dir_stat_reporting_on())
      {
         m_fs_state->dump_recursively();
      }

      if (purge_required)
      {
         // Loop over map and remove files with oldest values of access time.
         struct stat fstat;
         size_t      info_ext_len  =  strlen(Info::s_infoExtension);
         int         protected_cnt = 0;
         long long   protected_sum = 0;
         for (FPurgeState::map_i it = purgeState.refMap().begin(); it != purgeState.refMap().end(); ++it)
         {
            // Finish when enough space has been freed but not while age-based purging is in progress.
            // Those files are marked with time-stamp = 0.
            if (bytesToRemove <= 0 && ! (enforce_age_based_purge && it->first == 0))
            {
               break;
            }

            std::string &infoPath = it->second.path;
            std::string  dataPath = infoPath.substr(0, infoPath.size() - info_ext_len);

            if (IsFileActiveOrPurgeProtected(dataPath))
            {
               ++protected_cnt;
               protected_sum += it->second.nBytes;
               TRACE(Debug, trc_pfx << "File is active or purge-protected: " << dataPath << " size: " << it->second.nBytes);
               continue;
            }

            // remove info file
            if (m_oss->Stat(infoPath.c_str(), &fstat) == XrdOssOK)
            {
               // cinfo file can be on another oss.space, do not subtract for now.
               // Could be relevant for very small block sizes.
               // bytesToRemove        -= fstat.st_size;
               // estimated_file_usage -= fstat.st_size;
               // ++deleted_file_count;

               m_oss->Unlink(infoPath.c_str());
               TRACE(Dump, trc_pfx << "Removed file: '" << infoPath << "' size: " << fstat.st_size);
            }

            // remove data file
            if (m_oss->Stat(dataPath.c_str(), &fstat) == XrdOssOK)
            {
               bytesToRemove        -= it->second.nBytes;
               estimated_file_usage -= it->second.nBytes;
               ++deleted_file_count;

               m_oss->Unlink(dataPath.c_str());
               TRACE(Dump, trc_pfx << "Removed file: '" << dataPath << "' size: " << it->second.nBytes << ", time: " << it->first);

               if (it->second.dirState != 0) // XXXX This should now always be true.
                  it->second.dirState->add_usage_purged(it->second.nBytes);
               else
                  TRACE(Error, trc_pfx << "DirState not set for file '" << dataPath << "'.");
            }
         }
         if (protected_cnt > 0)
         {
            TRACE(Info, trc_pfx << "Encountered " << protected_cnt << " protected files, sum of their size: " << protected_sum);
         }

         m_fs_state->upward_propagate_usage_purged();
      }

      {
         XrdSysCondVarHelper lock(&m_active_cond);

         m_purge_delay_set.clear();
         m_in_purge = false;
      }

      int purge_duration = time(0) - purge_start;

      TRACE(Info, trc_pfx << "Finished, removed " << deleted_file_count << " data files, total size " <<
            bytesToRemove_at_start - bytesToRemove << ", bytes to remove at end " << bytesToRemove << ", purge duration " << purge_duration);

      int sleep_time = m_configuration.m_purgeInterval - purge_duration;
      if (sleep_time > 0)
      {
         sleep(sleep_time);
      }
   }
}

} // end XrdPfc namespace
