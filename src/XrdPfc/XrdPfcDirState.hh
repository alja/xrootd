#ifndef __XRDPFC_DIRSTATE_HH__
#define __XRDPFC_DIRSTATE_HH__

#include "XrdPfc.hh"
#include "XrdPfcInfo.hh"
#include "XrdPfcStats.hh"
#include <map>
#include <string>

using namespace XrdPfc;

namespace XrdPfc
{

//==============================================================================
// DirState
//==============================================================================

class DirState
{
   DirState    *m_parent;

   Stats        m_stats;        // access stats from client reads in this directory (and subdirs)

   long long    m_usage{0};        // collected / measured during purge traversal
   long long    m_usage_extra{0};  // collected from write events in this directory and subdirs
   long long    m_usage_purged{0}; // amount of data purged from this directory (and subdirectories for leaf nodes)

   // begin purge traversal usage \_ so we can have a good estimate of what came in during the traversal
   // end purge traversal usage   /  (should be small, presumably)

   // quota info, enabled?

   int          m_depth;
   int          m_max_depth;    // XXXX Do we need this? Should it be passed in to find functions?
   bool         m_stat_report;  // not used yet - storing of stats requested

   typedef std::map<std::string, DirState> DsMap_t;
   typedef DsMap_t::iterator               DsMap_i;

   DsMap_t      m_subdirs;

   void init();

   DirState* create_child(const std::string &dir);

   DirState* find_path_tok(PathTokenizer &pt, int pos, bool create_subdirs);

public:

   DirState(int max_depth);

   DirState(DirState *parent);

   DirState* get_parent() { return m_parent; }

   long long get_usage() { return m_usage; }

   void      set_usage(long long u);
   void      add_up_stats(const Stats& stats);
   void      add_usage_purged(long long up);

   DirState* find_path(const std::string &path, int max_depth, bool parse_as_lfn, bool create_subdirs);

   DirState* find_dir(const std::string &dir, bool create_subdirs);

   void reset_stats();

   void upward_propagate_stats();

   long long upward_propagate_usage_purged();

   void dump_recursively(const char *name);
};
}

#endif
