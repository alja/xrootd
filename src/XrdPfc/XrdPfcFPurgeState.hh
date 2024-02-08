#ifndef __XRDPFC_FPURGESTATE_HH__
#define __XRDPFC_FPURGESTATE_HH__
#include "XrdPfc.hh"
#include "XrdPfcDirState.hh"
#include "XrdPfcFPurgeState.hh"
#include "XrdPfcTrace.hh"
#include "XrdOss/XrdOssAt.hh"
#include "XrdSys/XrdSysTrace.hh"
#include "XrdOuc/XrdOucUtils.hh"
#include "XrdOuc/XrdOucEnv.hh"


namespace XrdPfc {

//==============================================================================
// FPurgeState
//==============================================================================

class FPurgeState
{
public:
   struct FS
   {
      std::string path;
      long long   nBytes;
      time_t      time;
      DirState   *dirState;

      FS(const std::string &dname, const char *fname, long long n, time_t t, DirState *ds) :
         path(dname + fname), nBytes(n), time(t), dirState(ds)
      {}
   };

   typedef std::list<FS>    list_t;
   typedef list_t::iterator list_i;
   typedef std::multimap<time_t, FS> map_t;
   typedef map_t::iterator           map_i;

private:
   long long m_nBytesReq;
   long long m_nBytesAccum;
   long long m_nBytesTotal;
   time_t    m_tMinTimeStamp;
   time_t    m_tMinUVKeepTimeStamp;
   std::vector<std::string> m_dir_names_stack;
   std::vector<long long>   m_dir_usage_stack;


   XrdOssAt  m_oss_at;

   DirState    *m_dir_state;
   std::string  m_current_path; // Includes trailing '/'
   int          m_dir_level;
   const int    m_max_dir_level_for_stat_collection; // until we honor globs from pfc.dirstats

   const char   *m_info_ext;
   const size_t  m_info_ext_len;
   XrdSysTrace  *m_trace;

   static const char *m_traceID;


   list_t  m_flist; // list of files to be removed unconditionally
   map_t   m_fmap; // map of files that are purge candidates

public:
   FPurgeState(long long iNBytesReq, XrdOss &oss);

   map_t &refMap() { return m_fmap; }
   list_t &refList() { return m_flist; }

   // ------------------------------------
   // Directory handling & stat collection
   // ------------------------------------

   void begin_traversal(DirState *root, const char *root_path = "/");

   void end_traversal();

   void cd_down(const std::string& dir_name);

   void cd_up();
   
   void      setMinTime(time_t min_time) { m_tMinTimeStamp = min_time; }
   time_t    getMinTime()          const { return m_tMinTimeStamp; }
   void      setUVKeepMinTime(time_t min_time) { m_tMinUVKeepTimeStamp = min_time; }
   long long getNBytesTotal()      const { return m_nBytesTotal; }

   void MoveListEntriesToMap();

   void CheckFile(const char *fname, Info &info, struct stat &fstat /*, XrdOssDF *iOssDF*/);

   void TraverseNamespace(XrdOssDF *iOssDF);
};

} // namespace XrdPfc

#endif
