#ifndef __XRDPFC_DIRSTATE_HH__
#define __XRDPFC_DIRSTATE_HH__

#include "XrdPfcStats.hh"

#include <ctime>
#include <map>
#include <string>

namespace XrdPfc
{

class PathTokenizer;

//==============================================================================
// Manifest:
// 1. class DirState -- state of a directory, including current delta-stats
// 2. class DataFSState -- manager of the DirState tree, starting from root (as in "/").


//==============================================================================
// DirState
//==============================================================================

struct DirUsage
{
   time_t    m_last_open_time  = 0;
   time_t    m_last_close_time = 0;
   long long m_bytes_on_disk   = 0;
   int       m_num_files_open  = 0;
   int       m_num_files       = 0;
   int       m_num_subdirs     = 0;
};

class DirState
{
public:
   DirState    *m_parent = nullptr;
   std::string  m_dir_name;

   DirStats     m_here_stats;
   DirStats     m_recursive_subdirs_stats;

   DirUsage     m_here_usage;
   DirUsage     m_recursive_subdir_usage;

   // XXX int m_possible_discrepancy; // num detected possible inconsistencies. here, subdirs?

   // flag - can potentially be inaccurate -- plus timestamp of it (min or max, if several for subdirs)?

   // + the same kind for resursive sums of all subdirs (but not in this dir).
   // Thus, to get recursive totals of here+all_subdirs, one need to add them up.
   // Have extra members or define an intermediate structure?

   // Do we need running averages of these, too, not just traffic?
   // Snapshot should be fine, no?

   // Do we need all-time stats? Files/dirs created, deleted; files opened/closed;
   // Well, those would be like Stats-running-average-infinity, just adding stuff in.

   // min/max open (or close ... or both?) time-stamps

   // Do we need string name? Probably yes, if we want to construct PFN from a given
   // inner node upwards. Also, in this case, is it really the best idea to have
   // map<string, DirState> as daughter container? It keeps them sorted for export :)

   // m_stats should be separated by "here" and "daughters_recursively", too.

   // begin purge traversal usage \_ so we can have a good estimate of what came in during the traversal
   // end purge traversal usage   /  (should be small, presumably)

   // quota info, enabled?

   int          m_depth;
   bool         m_stat_report;  // not used yet - storing of stats requested

   typedef std::map<std::string, DirState> DsMap_t;
   typedef DsMap_t::iterator               DsMap_i;

   DsMap_t      m_subdirs;

   void init();

   DirState* create_child(const std::string &dir);

   DirState* find_path_tok(PathTokenizer &pt, int pos, bool create_subdirs);

public:

   DirState();

   DirState(DirState *parent);

   DirState(DirState *parent, const std::string& dname);

   DirState* get_parent() { return m_parent; }

   void      add_up_stats(const Stats& stats);

   DirState* find_path(const std::string &path, int max_depth, bool parse_as_lfn, bool create_subdirs);

   DirState* find_dir(const std::string &dir, bool create_subdirs);

   void reset_stats();

   void upward_propagate_stats();

   long long upward_propagate_usage_purged();

   void dump_recursively(const char *name, int max_depth);
};


//==============================================================================
// DataFsState
//==============================================================================

class DataFsState
{
   DirState  m_root;
   time_t    m_prev_time;

public:
   DataFsState() :
      m_root      (),
      m_prev_time (time(0))
   {}

   DirState* get_root()            { return & m_root; }

   DirState* find_dirstate_for_lfn(const std::string& lfn)
   {
      return m_root.find_path(lfn, -1, true, true);
   }

   void reset_stats()                   { m_root.reset_stats();                   }
   void upward_propagate_stats()        { m_root.upward_propagate_stats();        }
   void upward_propagate_usage_purged() { m_root.upward_propagate_usage_purged(); }

   void dump_recursively(int max_depth)
   {
      if (max_depth < 0)
         max_depth = 4096;
      time_t now = time(0);

      printf("DataFsState::dump_recursively epoch = %lld delta_t = %lld, max_dump_depth = %d\n",
             (long long) now, (long long) (now - m_prev_time), max_depth);

      m_prev_time = now;

      m_root.dump_recursively("root", max_depth);
   }
};

}

#endif
