#include "XrdPfcDirState.hh"
#include "XrdPfcStats.hh"

#include <string>

namespace XrdPfc
{
//----------------------------------------------------------------------------
//! Constructor
//! @param int max subdir depth
//----------------------------------------------------------------------------
DirState::DirState(int max_depth) : m_parent(0), m_depth(0), m_max_depth(max_depth)
{
   // init();
}

//----------------------------------------------------------------------------
//! Constructor
//! @param DirState parent directory
//----------------------------------------------------------------------------
DirState::DirState(DirState *parent) : m_parent(parent), m_depth(m_parent->m_depth + 1), m_max_depth(m_parent->m_max_depth)
{
    // init();
}
/*
void DirState::init()
{
    m_usage = 0;
    m_usage_extra  = 0;
    m_usage_purged = 0;
}*/

//----------------------------------------------------------------------------
//! Internal function called from find_dir or find_path_tok
//! @param dir subdir name
//----------------------------------------------------------------------------
DirState* DirState::create_child(const std::string &dir)
{
    std::pair<DsMap_i, bool> ir = m_subdirs.insert(std::make_pair(dir, DirState(this)));
    return &  ir.first->second;
}


//----------------------------------------------------------------------------
//! Internal function called from find_path
//! @param dir subdir name
//----------------------------------------------------------------------------
DirState* DirState::find_path_tok(PathTokenizer &pt, int pos, bool create_subdirs)
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



//----------------------------------------------------------------------------
//! Recursive function to find DireState with given absolute dir path
//! @param dir subdir name
//! @param max_depth 
//! @param parse_as_lfn
//! @param create_subdirs 
DirState* DirState::find_path(const std::string &path, int max_depth, bool parse_as_lfn, bool create_subdirs)
{
    PathTokenizer pt(path, max_depth, parse_as_lfn);

    return find_path_tok(pt, 0, create_subdirs);
}

//----------------------------------------------------------------------------
//! Non recursive function
//! @param dir subdir name @param bool create the subdir in this DirsStat
DirState* DirState::find_dir(const std::string &dir, bool create_subdirs)
{
    DsMap_i i = m_subdirs.find(dir);

    if (i != m_subdirs.end())  return & i->second;

    if (create_subdirs && m_depth < m_max_depth)  return create_child(dir);

    return 0;
}

//----------------------------------------------------------------------------
//! Reset current transaction statistics.
//! Called from Cache::copy_out_active_stats_and_update_data_fs_state()
//----------------------------------------------------------------------------
void DirState::reset_stats()
{
    m_stats.Reset();

    for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
    {
        i->second.reset_stats();
    }
}

//----------------------------------------------------------------------------
//! Propagate stat to parents
//! Called from Cache::copy_out_active_stats_and_update_data_fs_state()
//----------------------------------------------------------------------------
void DirState::upward_propagate_stats()
{
    for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
    {
        i->second.upward_propagate_stats();

        m_stats.AddUp(i->second.m_stats);
    }

    m_usage_extra += m_stats.m_BytesWritten;
}

//----------------------------------------------------------------------------
//! Update statistics.
//! Called from Purge thread.
//----------------------------------------------------------------------------
long long DirState::upward_propagate_usage_purged()
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

//----------------------------------------------------------------------------
//! Recursive print of statistics. Called if defined in pfc configuration.
//!
//----------------------------------------------------------------------------
void DirState::dump_recursively(const char *name)
{
    printf("%*d %s usage=%lld usage_extra=%lld usage_total=%lld num_ios=%d duration=%d b_hit=%lld b_miss=%lld b_byps=%lld b_wrtn=%lld\n",
            2 + 2*m_depth, m_depth, name, m_usage, m_usage_extra, m_usage + m_usage_extra,
            m_stats.m_NumIos, m_stats.m_Duration, m_stats.m_BytesHit, m_stats.m_BytesMissed, m_stats.m_BytesBypassed, m_stats.m_BytesWritten);

    for (DsMap_i i = m_subdirs.begin(); i != m_subdirs.end(); ++i)
    {
        i->second.dump_recursively(i->first.c_str());
    }
}

//----------------------------------------------------------------------------
//! Set read usage
//!
//----------------------------------------------------------------------------
void DirState::set_usage(long long u)
{
    m_usage = u;
    m_usage_extra = 0; 
}


//----------------------------------------------------------------------------
//! Add to temporary Stat obj
//!
//----------------------------------------------------------------------------
void DirState::add_up_stats(const Stats& stats)
{
   m_stats.AddUp(stats);
}

//----------------------------------------------------------------------------
//! Accumulate usage from purged files
//!
//----------------------------------------------------------------------------
void DirState::add_usage_purged(long long up)
{
    m_usage_purged += up;
}

} // end namespace
