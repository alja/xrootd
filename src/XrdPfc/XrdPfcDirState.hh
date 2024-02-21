#ifndef __XRDPFC_DIRSTATE_HH__
#define __XRDPFC_DIRSTATE_HH__

#include "XrdPfcInfo.hh"

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

   long long    m_usage;        // collected / measured during purge traversal
   long long    m_usage_extra;  // collected from write events in this directory and subdirs
   long long    m_usage_purged; // amount of data purged from this directory (and subdirectories for leaf nodes)

   // begin purge traversal usage \_ so we can have a good estimate of what came in during the traversal
   // end purge traversal usage   /  (should be small, presumably)

   // quota info, enabled?

   int          m_depth;
   int          m_max_depth;    // XXXX Do we need this? Should it be passed in to find functions?
   bool         m_stat_report;  // not used yet - storing of stats requested

   typedef std::map<std::string, DirState> DsMap_t;
   typedef DsMap_t::iterator               DsMap_i;

   DsMap_t      m_subdirs;

   void init()
   {
      m_usage = 0;
      m_usage_extra  = 0;
      m_usage_purged = 0;
   }

   DirState* create_child(const std::string &dir)
   {
      std::pair<DsMap_i, bool> ir = m_subdirs.insert(std::make_pair(dir, DirState(this)));
      return &  ir.first->second;
   }

   DirState* find_path_tok(PathTokenizer &pt, int pos, bool create_subdirs)
   {
      if (pos == pt.get_n_dirs()) return this;

      DsMap_i i = m_subdirs.find(pt.m_dirs[pos]);

      DirState *ds = 0;

      if (i != m_subdirs.end())
      {
         ds = & i->second;
      }
      if (create_subdirs && m_depth < m_max_depth)
      {
         ds = create_child(pt.m_dirs[pos]);
      }
      if (ds) return ds->find_path_tok(pt, pos + 1, create_subdirs);

      return 0;
   }

public:

   DirState(int max_depth) : m_parent(0), m_depth(0), m_max_depth(max_depth)
   {
      init();
   }

   DirState(DirState *parent) : m_parent(parent), m_depth(m_parent->m_depth + 1), m_max_depth(m_parent->m_max_depth)
   {
      init();
   }

   DirState* get_parent()                     { return m_parent; }

   long long get_usage() { return m_usage; }

   void      set_usage(long long u)           { m_usage = u; m_usage_extra = 0; }
   void      add_up_stats(const Stats& stats) { m_stats.AddUp(stats); }
   void      add_usage_purged(long long up)   { m_usage_purged += up; }

   DirState* find_path(const std::string &path, int max_depth, bool parse_as_lfn, bool create_subdirs)
   {
      PathTokenizer pt(path, max_depth, parse_as_lfn);

      return find_path_tok(pt, 0, create_subdirs);
   }

   DirState* find_dir(const std::string &dir, bool create_subdirs)
   {
      DsMap_i i = m_subdirs.find(dir);

      if (i != m_subdirs.end())  return & i->second;

      if (create_subdirs && m_depth < m_max_depth)  return create_child(dir);

      return 0;
   }

   void reset_stats()
   {
      m_stats.Reset();

      for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
      {
         i->second.reset_stats();
      }
   }

   void upward_propagate_stats()
   {
      for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
      {
         i->second.upward_propagate_stats();

         m_stats.AddUp(i->second.m_stats);
      }

      m_usage_extra += m_stats.m_BytesWritten;
   }

   long long upward_propagate_usage_purged()
   {
      for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
      {
         m_usage_purged += i->second.upward_propagate_usage_purged();
      }
      m_usage -= m_usage_purged;

      long long ret = m_usage_purged;
      m_usage_purged = 0;
      return ret;
   }

   void dump_recursively(const char *name)
   {
      printf("%*d %s usage=%lld usage_extra=%lld usage_total=%lld num_ios=%d duration=%d b_hit=%lld b_miss=%lld b_byps=%lld b_wrtn=%lld\n",
             2 + 2*m_depth, m_depth, name, m_usage, m_usage_extra, m_usage + m_usage_extra,
             m_stats.m_NumIos, m_stats.m_Duration, m_stats.m_BytesHit, m_stats.m_BytesMissed, m_stats.m_BytesBypassed, m_stats.m_BytesWritten);

      for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
      {
         i->second.dump_recursively(i->first.c_str());
      }
   }
};
}

#endif
