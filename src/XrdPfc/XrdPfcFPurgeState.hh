#ifndef __XRDPFC_FPURGESTATE_HH__
#define __XRDPFC_FPURGESTATE_HH__

#include <ctime>
#include <list>
#include <map>
#include <string>

#include <sys/stat.h>

class XrdOss;

namespace XrdPfc {

class Info;
class FsTraversal;

//==============================================================================
// FPurgeState
//==============================================================================

class FPurgeState
{
public:
   struct PurgeCandidate // unknown meaning, "file that is candidate for purge", PurgeCandidate would be better.
   {
      std::string path;
      long long   nBytes;
      time_t      time;

      PurgeCandidate(const std::string &dname, const char *fname, long long n, time_t t) :
         path(dname + fname), nBytes(n), time(t)
      {}
   };

   using list_t = std::list<PurgeCandidate>;
   using list_i = list_t::iterator;
   using map_t  = std::multimap<time_t, PurgeCandidate>;
   using map_i  = map_t::iterator;

private:
   XrdOss   &m_oss;

   long long m_nBytesReq;
   long long m_nBytesAccum;
   long long m_nBytesTotal;
   time_t    m_tMinTimeStamp;
   time_t    m_tMinUVKeepTimeStamp;

   static const char *m_traceID;

   list_t  m_flist; // list of files to be removed unconditionally
   map_t   m_fmap; // map of files that are purge candidates

public:
   FPurgeState(long long iNBytesReq, XrdOss &oss);

   map_t &refMap() { return m_fmap; }
   list_t &refList() { return m_flist; }

   void      setMinTime(time_t min_time) { m_tMinTimeStamp = min_time; }
   time_t    getMinTime()          const { return m_tMinTimeStamp; }
   void      setUVKeepMinTime(time_t min_time) { m_tMinUVKeepTimeStamp = min_time; }
   long long getNBytesTotal()      const { return m_nBytesTotal; }

   void MoveListEntriesToMap();

   void CheckFile(const FsTraversal &fst, const char *fname, Info &info, struct stat &fstat);

   void ProcessDirAndRecurse(FsTraversal &fst);
   bool TraverseNamespace(const char *root_path);
};

} // namespace XrdPfc

#endif
