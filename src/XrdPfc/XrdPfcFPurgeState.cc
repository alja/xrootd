#include "XrdPfcFPurgeState.hh"

#include "XrdPfcDirState.hh"
#include "XrdPfcFPurgeState.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucUtils.hh"
#include "XrdOss/XrdOssAt.hh"
#include "XrdSys/XrdSysTrace.hh"

// Temporary, extensive purge tracing
// #define TRACE_PURGE(x) TRACE(Debug, x)
#define TRACE_PURGE(x) std::cout << "PURGE " << x << "\n"
#define TRACE_PURGE(x)

using namespace XrdPfc;

namespace XrdPfc
{

XrdSysTrace* GetTrace()
{
    // needed for logging macros
    return Cache::GetInstance().GetTrace();
}


//----------------------------------------------------------------------------
//! Constructor.
//----------------------------------------------------------------------------
FPurgeState::FPurgeState(long long iNBytesReq, XrdOss &oss) :
    m_nBytesReq(iNBytesReq), m_nBytesAccum(0), m_nBytesTotal(0), m_tMinTimeStamp(0), m_tMinUVKeepTimeStamp(0),
    m_oss_at(oss),
    m_dir_state(0), m_dir_level(0),
    m_max_dir_level_for_stat_collection(Cache::Conf().m_dirStatsStoreDepth),
    m_info_ext(XrdPfc::Info::s_infoExtension),
    m_info_ext_len(strlen(XrdPfc::Info::s_infoExtension)),
    m_trace(Cache::GetInstance().GetTrace())
{
    m_current_path.reserve(256);
    m_dir_names_stack.reserve(32);
    m_dir_usage_stack.reserve(m_max_dir_level_for_stat_collection + 1);
}

//----------------------------------------------------------------------------
//! Initiate DirState for traversal.
//! @param directory statistics
//! @param path relative to caching proxy OSS
//----------------------------------------------------------------------------
void FPurgeState::begin_traversal(DirState *root, const char *root_path)
{
    m_dir_state = root;
    m_dir_level = 0;
    m_current_path = std::string(root_path);
    m_dir_usage_stack.push_back(0);

    TRACE_PURGE("FPurgeState::begin_traversal cur_path '" << m_current_path << "', usage=" << m_dir_usage_stack.back() << ", level=" << m_dir_level);
}

//----------------------------------------------------------------------------
//! Finalize DirState at the end of traversal.
//----------------------------------------------------------------------------
void FPurgeState::end_traversal()
{
    TRACE_PURGE("FPurgeState::end_traversal reporting for '" << m_current_path << "', usage=" << m_dir_usage_stack.back() << ", nBytesTotal=" << m_nBytesTotal << ", level=" << m_dir_level);

    m_dir_state->set_usage(m_dir_usage_stack.back());

    m_dir_state = 0;
}

//----------------------------------------------------------------------------
//! Move to child directory.
//! @param relative name of subdirectory
//----------------------------------------------------------------------------
void FPurgeState::cd_down(const std::string &dir_name)
{
    ++m_dir_level;

    if (m_dir_level <= m_max_dir_level_for_stat_collection)
    {
        m_dir_usage_stack.push_back(0);
        m_dir_state = m_dir_state->find_dir(dir_name, true);
    }

    m_dir_names_stack.push_back(dir_name);
    m_current_path.append(dir_name);
    m_current_path.append("/");
}

//----------------------------------------------------------------------------
//! Move to parent directory and set disk usage.
//----------------------------------------------------------------------------
void FPurgeState::cd_up()
{
    if (m_dir_level <= m_max_dir_level_for_stat_collection)
    {
        long long tail = m_dir_usage_stack.back();
        m_dir_usage_stack.pop_back();

        TRACE_PURGE("FPurgeState::cd_up reporting for '" << m_current_path << "', usage=" << tail << ", level=" << m_dir_level);

        m_dir_state->set_usage(tail);
        m_dir_state = m_dir_state->get_parent();

        m_dir_usage_stack.back() += tail;
    }

    // remove trailing / and last dir but keep the new trailing / in place.
    m_current_path.erase(m_current_path.find_last_of('/', m_current_path.size() - 2) + 1);
    m_dir_names_stack.pop_back();

    --m_dir_level;
}

//----------------------------------------------------------------------------
//! Move remaing entires to the member map.
//! This is used for cold files and for files collected from purge plugin.
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
//! @param name of the cached file
//! @param Info object
//! @param stat of the given file
//!
//----------------------------------------------------------------------------
void FPurgeState::CheckFile(const char *fname, Info &info, struct stat &fstat /*, XrdOssDF *iOssDF*/)
{
    static const char *trc_pfx = "FPurgeState::CheckFile ";

    long long nbytes = info.GetNDownloadedBytes();
    time_t atime;
    if (!info.GetLatestDetachTime(atime))
    {
        // cinfo file does not contain any known accesses, use fstat.mtime instead.
        TRACE(Debug, trc_pfx << "could not get access time for " << m_current_path << fname << ", using mtime from stat instead.");
        atime = fstat.st_mtime;
    }
    // TRACE(Dump, trc_pfx << "checking " << fname << " accessTime  " << atime);

    m_nBytesTotal += nbytes;

    m_dir_usage_stack.back() += nbytes;

    // XXXX Should remove aged-out files here ... but I have trouble getting
    // the DirState and purge report set up consistently.
    // Need some serious code reorganization here.
    // Biggest problem is maintaining overall state a traversal state consistently.
    // Sigh.

    // In first two cases we lie about FS time (set to 0) to get them all removed early.
    // The age-based purge atime would also be good as there should be nothing
    // before that time in the map anyway.
    // But we use 0 as a test in purge loop to make sure we continue even if enough
    // disk-space has been freed.

    if (m_tMinTimeStamp > 0 && atime < m_tMinTimeStamp)
    {
        m_flist.push_back(FS(m_current_path, fname, nbytes, 0, m_dir_state));
        m_nBytesAccum += nbytes;
    }
    else if (m_tMinUVKeepTimeStamp > 0 &&
             Cache::Conf().does_cschk_have_missing_bits(info.GetCkSumState()) &&
             info.GetNoCkSumTimeForUVKeep() < m_tMinUVKeepTimeStamp)
    {
        m_flist.push_back(FS(m_current_path, fname, nbytes, 0, m_dir_state));
        m_nBytesAccum += nbytes;
    }
    else if (m_nBytesAccum < m_nBytesReq || (!m_fmap.empty() && atime < m_fmap.rbegin()->first))
    {
        m_fmap.insert(std::make_pair(atime, FS(m_current_path, fname, nbytes, atime, m_dir_state)));
        m_nBytesAccum += nbytes;

        // remove newest files from map if necessary
        while (!m_fmap.empty() && m_nBytesAccum - m_fmap.rbegin()->second.nBytes >= m_nBytesReq)
        {
            m_nBytesAccum -= m_fmap.rbegin()->second.nBytes;
            m_fmap.erase(--(m_fmap.rbegin().base()));
        }
    }
}

//----------------------------------------------------------------------------
//! Recursively traverse directory. Build DirState statistics and sort files.
//! @param XrdOssDF handle
//----------------------------------------------------------------------------
void FPurgeState::TraverseNamespace(XrdOssDF *iOssDF)
{
    static const char *trc_pfx = "FPurgeState::TraverseNamespace ";

    char fname[256];
    struct stat fstat;
    XrdOucEnv env;

    TRACE_PURGE("Starting to read dir [" << m_current_path << "], iOssDF->getFD()=" << iOssDF->getFD() << ".");

    iOssDF->StatRet(&fstat);

    while (true)
    {
        int rc = iOssDF->Readdir(fname, 256);

        if (rc == -ENOENT)
        {
            TRACE_PURGE("  Skipping ENOENT dir entry [" << fname << "].");
            continue;
        }
        if (rc != XrdOssOK)
        {
            TRACE(Error, trc_pfx << "Readdir error at " << m_current_path << ", err " << XrdSysE2T(-rc) << ".");
            break;
        }

        TRACE_PURGE("  Readdir [" << fname << "]");

        if (fname[0] == 0)
        {
            TRACE_PURGE("  Finished reading dir [" << m_current_path << "]. Break loop.");
            break;
        }
        if (fname[0] == '.' && (fname[1] == 0 || (fname[1] == '.' && fname[2] == 0)))
        {
            TRACE_PURGE("  Skipping here or parent dir [" << fname << "]. Continue loop.");
            continue;
        }

        size_t fname_len = strlen(fname);
        XrdOssDF *dfh = 0;

        if (S_ISDIR(fstat.st_mode))
        {
            if (m_oss_at.Opendir(*iOssDF, fname, env, dfh) == XrdOssOK)
            {
                cd_down(fname);
                TRACE_PURGE("  cd_down -> [" << m_current_path << "].");
                TraverseNamespace(dfh);
                cd_up();
                TRACE_PURGE("  cd_up   -> [" << m_current_path << "].");
            }
            else
                TRACE(Warning, trc_pfx << "could not opendir [" << m_current_path << fname << "], " << XrdSysE2T(errno));
        }
        else if (fname_len > m_info_ext_len && strncmp(&fname[fname_len - m_info_ext_len], m_info_ext, m_info_ext_len) == 0)
        {
            // Check if the file is currently opened / purge-protected is done before unlinking of the file.

            Info cinfo(m_trace);

            if (m_oss_at.OpenRO(*iOssDF, fname, env, dfh) == XrdOssOK && cinfo.Read(dfh, m_current_path.c_str(), fname))
            {
                CheckFile(fname, cinfo, fstat);
            }
            else
            {
                TRACE(Warning, trc_pfx << "can't open or read " << m_current_path << fname << ", err " << XrdSysE2T(errno) << "; purging.");
                m_oss_at.Unlink(*iOssDF, fname);
                fname[fname_len - m_info_ext_len] = 0;
                m_oss_at.Unlink(*iOssDF, fname);
            }
        }
        else // XXXX devel debug only, to be removed
        {
            TRACE_PURGE("  Ignoring [" << fname << "], not a dir or cinfo.");
        }

        delete dfh;
    }
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

} // namespace XrdPfc

