#include "XrdPfcFPurgeState.hh"
#include "XrdPfcFsTraversal.hh"
#include "XrdPfcInfo.hh"
#include "XrdPfc.hh"
#include "XrdPfcTrace.hh"

#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucUtils.hh"
#include "XrdOss/XrdOss.hh"
#include "XrdOss/XrdOssAt.hh"

// Temporary, extensive purge tracing
// #define TRACE_PURGE(x) TRACE(Debug, x)
// #define TRACE_PURGE(x) std::cout << "PURGE " << x << "\n"
#define TRACE_PURGE(x)

using namespace XrdPfc;

namespace
{
    XrdSysTrace* GetTrace() { return Cache::GetInstance().GetTrace(); }
}

const char *FPurgeState::m_traceID = "Purge";

//----------------------------------------------------------------------------
//! Constructor.
//----------------------------------------------------------------------------
FPurgeState::FPurgeState(long long iNBytesReq, XrdOss &oss) :
   m_oss(oss),
   m_nBytesReq(iNBytesReq), m_nBytesAccum(0), m_nBytesTotal(0),
   m_tMinTimeStamp(0), m_tMinUVKeepTimeStamp(0)
{
    // XXXX init traversal, pass oss? Note, do NOT use DirState (it would have to come from elsewhere)
    // well, depends how it's going to be called, eventually
}

//----------------------------------------------------------------------------
//! Move remaing entires to the member map.
//! This is used for cold files and for files collected from purge plugin (really?).
//----------------------------------------------------------------------------
void FPurgeState::MoveListEntriesToMap()
{
   for (list_i i = m_flist.begin(); i != m_flist.end(); ++i)
   {
      m_fmap.insert(std::make_pair(i->time, *i));
   }
   m_flist.clear();
}

//----------------------------------------------------------------------------
//! Open info file. Look at the UV stams and last access time. 
//! Store the file in sorted map or in a list.s
//! @param fname name of cache-info file
//! @param Info object
//! @param stat of the given file
//!
//----------------------------------------------------------------------------
void FPurgeState::CheckFile(const FsTraversal &fst, const char *fname, Info &info, struct stat &fstat)
{
   static const char *trc_pfx = "FPurgeState::CheckFile ";

   long long nbytes = info.GetNDownloadedBytes();
   time_t atime;
   if (!info.GetLatestDetachTime(atime))
   {
      // cinfo file does not contain any known accesses, use fstat.mtime instead.
      TRACE(Debug, trc_pfx << "could not get access time for " << fst.m_current_path << fname << ", using mtime from stat instead.");
      atime = fstat.st_mtime;
   }
   // TRACE(Dump, trc_pfx << "checking " << fname << " accessTime  " << atime);

   m_nBytesTotal += nbytes;

   // XXXX Should remove aged-out files here ... but I have trouble getting
   // the DirState and purge report set up consistently.
   // Need some serious code reorganization here.
   // Biggest problem is maintaining overall state a traversal state consistently.
   // Sigh.

   // This can be done right with transactional DirState. Also for uvkeep, it seems.

   // In first two cases we lie about PurgeCandidate time (set to 0) to get them all removed early.
   // The age-based purge atime would also be good as there should be nothing
   // before that time in the map anyway.
   // But we use 0 as a test in purge loop to make sure we continue even if enough
   // disk-space has been freed.

   if (m_tMinTimeStamp > 0 && atime < m_tMinTimeStamp)
   {
      m_flist.push_back(PurgeCandidate(fst.m_current_path, fname, nbytes, 0));
      m_nBytesAccum += nbytes;
   }
   else if (m_tMinUVKeepTimeStamp > 0 &&
            Cache::Conf().does_cschk_have_missing_bits(info.GetCkSumState()) &&
            info.GetNoCkSumTimeForUVKeep() < m_tMinUVKeepTimeStamp)
   {
      m_flist.push_back(PurgeCandidate(fst.m_current_path, fname, nbytes, 0));
      m_nBytesAccum += nbytes;
   }
   else if (m_nBytesAccum < m_nBytesReq || (!m_fmap.empty() && atime < m_fmap.rbegin()->first))
   {
      m_fmap.insert(std::make_pair(atime, PurgeCandidate(fst.m_current_path, fname, nbytes, atime)));
      m_nBytesAccum += nbytes;

      // remove newest files from map if necessary
      while (!m_fmap.empty() && m_nBytesAccum - m_fmap.rbegin()->second.nBytes >= m_nBytesReq)
      {
         m_nBytesAccum -= m_fmap.rbegin()->second.nBytes;
         m_fmap.erase(--(m_fmap.rbegin().base()));
      }
   }
}

void FPurgeState::ProcessDirAndRecurse(FsTraversal &fst)
{
   static const char *trc_pfx = "FPurgeState::ProcessDirAndRecurse ";

   for (auto it = fst.m_current_files.begin(); it != fst.m_current_files.end(); ++it)
   {
        // Check if the file is currently opened / purge-protected is done before unlinking of the file.
      const std::string &f_name = it->first;
      const std::string  i_name = f_name + Info::s_infoExtension;

      XrdOssDF    *fh = nullptr;
      struct stat  fstat;
      Info         cinfo(GetTrace());

      // XXX Note, the initial scan now uses stat information only!

      if (fst.open_at_ro(i_name.c_str(), fh) == XrdOssOK &&
          cinfo.Read(fh, fst.m_current_path.c_str(), i_name.c_str()))
      {
         CheckFile(fst, i_name.c_str(), cinfo, fstat);
      }
      else
      {
         TRACE(Warning, trc_pfx << "can't open or read " << fst.m_current_path << i_name << ", err " << XrdSysE2T(errno) << "; purging.");
         fst.unlink_at(i_name.c_str());
         fst.unlink_at(f_name.c_str());
         // generate purge event or not? or just flag possible discrepancy?
         // should this really be done in some other consistency-check traversal?
      }

      // XXX ? What do we do with the data-only / cinfo only ?
      // Protected top-directories are skipped.
   }

   std::vector<std::string> dirs;
   dirs.swap(fst.m_current_dirs);
   for (auto &dname : dirs)
   {
      if (fst.cd_down(dname))
      {
        ProcessDirAndRecurse(fst);
        fst.cd_up();
      }
   }
}

bool FPurgeState::TraverseNamespace(const char *root_path)
{
   bool success_p = true;

   FsTraversal fst(m_oss);
   fst.m_protected_top_dirs.insert("pfc-stats"); // XXXX This should come from config. Also: N2N?
                                                 // Also ... this onoly applies to /, not any root_path
   if (fst.begin_traversal(root_path))
   {
      ProcessDirAndRecurse(fst);
   }
   else
   {
      // Fail startup, can't open /.
      success_p = false;
   }
   fst.end_traversal();

   return success_p;
}

/*
void FPurgeState::UnlinkInfoAndData(const char *fname, long long nbytes, XrdOssDF *iOssDF)
{
   fname[fname_len - m_info_ext_len] = 0;
   if (nbytes > 0)
   {
      if ( ! Cache.GetInstance().IsFileActiveOrPurgeProtected(dataPath))
      {
         m_n_purged++;
         m_bytes_purged += nbytes;
      } else
      {
         m_n_purge_protected++;
         m_bytes_purge_protected += nbytes;
         m_dir_state->add_usage_purged(nbytes);
         // XXXX should also tweak other stuff?
         fname[fname_len - m_info_ext_len] = '.';
         return;
      }
   }
   m_oss_at.Unlink(*iOssDF, fname);
   fname[fname_len - m_info_ext_len] = '.';
   m_oss_at.Unlink(*iOssDF, fname);
}
*/
