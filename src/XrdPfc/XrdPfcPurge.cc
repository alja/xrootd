#include "XrdPfc.hh"
#include "XrdPfcDirStateSnapshot.hh"
#include "XrdPfcResourceMonitor.hh"
#include "XrdPfcFPurgeState.hh"
#include "XrdPfcPurgePin.hh"
#include "XrdPfcTrace.hh"

#include "XrdOss/XrdOss.hh"

#include <sys/time.h>

namespace
{
   XrdSysTrace* GetTrace() { return XrdPfc::Cache::GetInstance().GetTrace(); }
   const char *m_traceID = "ResourceMonitor";
}

//==============================================================================
// OldStylePurgeDriver
//==============================================================================
namespace XrdPfc
{

void OldStylePurgeDriver(DataFsPurgeshot &ps)
{
   static const char *trc_pfx = "OldStylePurgeDriver ";

   const auto &cache = Cache::TheOne();
   const auto &conf  = Cache::Conf();
   auto &resmon = Cache::ResMon();
   auto &oss = *cache.GetOss();

   TRACE(Info, trc_pfx << "Started.");
   time_t purge_start = time(0);


   FPurgeState purgeState(2 * ps.m_bytes_to_remove, oss); // prepare twice more volume than required

   // Make a map of file paths, sorted by access time.

   if (ps.m_age_based_purge)
   {
      purgeState.setMinTime(time(0) - conf.m_purgeColdFilesAge);
   }
   if (conf.is_uvkeep_purge_in_effect())
   {
      purgeState.setUVKeepMinTime(time(0) - conf.m_cs_UVKeep);
   }

   bool scan_ok = purgeState.TraverseNamespace("/");
   if ( ! scan_ok) {
      TRACE(Error, trc_pfx << "namespace traversal failed at top-directory, this should not happen.");
      return;
   }

   TRACE(Debug, trc_pfx << "usage measured from cinfo files " << purgeState.getNBytesTotal() << " bytes.");

   purgeState.MoveListEntriesToMap();

   /////////////////////////////////////////////////////////////
   /// PurgePin begin
   PurgePin *purge_pin = cache.GetPurgePin();
   if (purge_pin)
   {
      // set dir stat for each path and calculate nBytes to recover for each path
      // return total bytes to recover within the plugin
      long long clearVal = purge_pin->GetBytesToRecover(ps);
      if (clearVal)
      {
         TRACE(Debug, "PurgePin remove total " << clearVal << " bytes");
         PurgePin::list_t &dpl = purge_pin->refDirInfos();
         // iterate through the plugin paths
         for (PurgePin::list_i ppit = dpl.begin(); ppit != dpl.end(); ++ppit)
         {
            TRACE(Debug, trc_pfx << "PurgePin scanning dir " << ppit->path.c_str() << " to remove " << ppit->nBytesToRecover << " bytes");

            FPurgeState fps(ppit->nBytesToRecover, oss);
            bool scan_ok = fps.TraverseNamespace(ppit->path.c_str());
            if ( ! scan_ok) {
               TRACE(Warning, trc_pfx << "purge-pin scan of directory failed for " << ppit->path);
               continue;
            }

            // fill central map from the plugin entry
            for (FPurgeState::map_i it = fps.refMap().begin(); it != fps.refMap().end(); ++it)
            {
               it->second.path = ppit->path + it->second.path;
               TRACE(Debug, trc_pfx << "PurgePin found file " << it->second.path.c_str()<< " size " << 512ll * it->second.nStBlocks);
               purgeState.refMap().insert(std::make_pair(0, it->second)); // set atime to zero to make sure this is deleted
            }
         }
      }
   }
   /// PurgePin end
   /////////////////////////////////////////////////////////////

   int deleted_file_count = 0;
   long long deleted_st_blocks = 0;

   // Loop over map and remove files with oldest values of access time.
   struct stat fstat;
   int         protected_cnt = 0;
   long long   protected_st_blocks = 0;
   long long   st_blocks_to_remove = (ps.m_bytes_to_remove >> 9) + 1ll;
   for (FPurgeState::map_i it = purgeState.refMap().begin(); it != purgeState.refMap().end(); ++it)
   {
      // Finish when enough space has been freed but not while age-based purging is in progress.
      // Those files are marked with time-stamp = 0.
      if (st_blocks_to_remove <= 0 && it->first != 0)
      {
         break;
      }

      std::string &infoPath = it->second.path;
      std::string  dataPath = infoPath.substr(0, infoPath.size() - Info::s_infoExtensionLen);

      if (cache.IsFileActiveOrPurgeProtected(dataPath))
      {
         ++protected_cnt;
         protected_st_blocks += it->second.nStBlocks;
         TRACE(Debug, trc_pfx << "File is active or purge-protected: " << dataPath << " size: " << 512ll * it->second.nStBlocks);
         continue;
      }

      // remove info file
      if (oss.Stat(infoPath.c_str(), &fstat) == XrdOssOK)
      {
         oss.Unlink(infoPath.c_str());
         TRACE(Dump, trc_pfx << "Removed file: '" << infoPath << "' size: " << 512ll * fstat.st_size);
      }

      // remove data file
      if (oss.Stat(dataPath.c_str(), &fstat) == XrdOssOK)
      {
         st_blocks_to_remove -= it->second.nStBlocks;
         deleted_st_blocks   += it->second.nStBlocks;
         ++deleted_file_count;

         oss.Unlink(dataPath.c_str());
         TRACE(Dump, trc_pfx << "Removed file: '" << dataPath << "' size: " << 512ll * it->second.nStBlocks << ", time: " << it->first);

         resmon.register_file_purge(dataPath, it->second.nStBlocks);
      }
   }
   if (protected_cnt > 0)
   {
      TRACE(Info, trc_pfx << "Encountered " << protected_cnt << " protected files, sum of their size: " << 512ll * protected_st_blocks);
   }

   int purge_duration = time(0) - purge_start;

   TRACE(Info, trc_pfx << "Finished, removed " << deleted_file_count << " data files, removed total size " << 512ll * deleted_st_blocks
                       << ", purge duration " << purge_duration);
}

} // end namespace XrdPfc
