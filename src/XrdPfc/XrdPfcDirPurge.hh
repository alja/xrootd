#ifndef __XRDPFC_PURGEPLG_HH__
#define __XRDPFC_PURGEPLG_HH__

#include <string>
#include <vector>
#include "XrdPfc.hh"

class XrdPfcDirState;


namespace XrdPfc
{
//----------------------------------------------------------------------------
//! Base class for reguesting directory space to obtain.
//----------------------------------------------------------------------------
class DirPurge
{
public:
   struct DirInfo
   {
    std::string path;
    long long   nBytesQuota{0};
    DirState*   dirState{nullptr};// cached
    long long   nBytesToRecover{0};
   };

   typedef std::vector<DirInfo> list_t;
   typedef list_t::iterator     list_i; 

protected:
   list_t m_list;

public:
   virtual ~DirPurgeRequest() {}

   void InitDirStatesForLocalPaths(XrdPfc::DirState *rootDS)
   {
       for (list_i it = m_list.begin(); it != m_list.end(); ++it)
       {
           it->dirState = rootDS->find_path(it->path, Cache::Conf().m_dirStatsStoreDepth, false, false);
       }
   }
   
   //---------------------------------------------------------------------
   //! Provide erase information from directory statistics
   //!
   //! @param & XrdPfcDirState
   //!
   //! @return total number of bytes
   //---------------------------------------------------------------------
   virtual long long GetBytesToRecover(XrdPfc::DirState*) = 0;

   list_t& refDirInfos() { return m_list; }
};
}

#endif

